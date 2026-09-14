/* Reading the cell. See battery.h. */

#include "kernel/drv/battery.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHANNEL  ADC_CHANNEL_9      /* GPIO10 on ADC1, ESP32-S3 */
#define BAT_DIVIDER      2                  /* the two-to-one on the board */
#define SAMPLES          8

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int                       s_ready;

int battery_init(void) {
  adc_oneshot_unit_init_cfg_t unit = { .unit_id = BAT_ADC_UNIT };
  adc_oneshot_chan_cfg_t chan = {
    /* 12 dB: the divided cell reads up to about 2.1 V and the 12 dB range
     * reaches 3.1, so the top of the battery is well inside it. */
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
  };

  if (s_ready) return 0;

  if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
    ESP_LOGW(TAG, "no ADC unit");
    return -1;
  }
  if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan) != ESP_OK) {
    ESP_LOGW(TAG, "channel config failed");
    adc_oneshot_del_unit(s_adc);
    s_adc = NULL;
    return -1;
  }

  /* Calibration if the chip was fused with it, which the S3 generally is.
   * Without it the raw counts are out by enough to matter at these voltages,
   * so a failure here is worth a line in the log rather than silence. */
  {
    adc_cali_curve_fitting_config_t cfg = {
      .unit_id = BAT_ADC_UNIT,
      .chan = BAT_ADC_CHANNEL,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &s_cali) != ESP_OK) {
      ESP_LOGW(TAG, "no calibration on this chip; readings are approximate");
      s_cali = NULL;
    }
  }

  s_ready = 1;
  return 0;
}

int battery_mv(void) {
  int raw_total = 0, n = 0, i;

  if (!s_ready && battery_init() != 0) return 0;

  for (i = 0; i < SAMPLES; i++) {
    int raw;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
    raw_total += raw;
    n++;
  }
  if (!n) return 0;
  raw_total /= n;

  if (s_cali) {
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw_total, &mv) == ESP_OK)
      return mv * BAT_DIVIDER;
  }
  /* Uncalibrated fallback: 12 dB attenuation reaches about 3100 mV across the
   * 12-bit range. Good to a few percent, which is the difference between "low"
   * and "fine" and not much more. */
  return (raw_total * 3100 / 4095) * BAT_DIVIDER;
}

/* A lithium cell's discharge curve, as breakpoints. Between them it is
 * interpolated, which is honest enough: what matters is that 3.7 V reads as
 * a third rather than as a half. */
static const struct { int mv; int pct; } CURVE[] = {
  { 4200, 100 }, { 4100, 92 }, { 4000, 83 }, { 3900, 72 }, { 3800, 59 },
  { 3700, 44 }, { 3650, 33 }, { 3600, 22 }, { 3500, 12 }, { 3400,  5 },
  { 3300,  0 },
};

int battery_percent(void) {
  int mv = battery_mv();
  size_t i;

  if (mv <= 0) return -1;
  if (mv >= CURVE[0].mv) return 100;

  for (i = 1; i < sizeof CURVE / sizeof CURVE[0]; i++) {
    if (mv >= CURVE[i].mv) {
      int span = CURVE[i - 1].mv - CURVE[i].mv;
      int into = mv - CURVE[i].mv;
      int rise = CURVE[i - 1].pct - CURVE[i].pct;
      return CURVE[i].pct + (span ? into * rise / span : 0);
    }
  }
  return 0;
}

int battery_charging(void) {
  /* Above the cell's own full-charge voltage, something is holding it there.
   * There is no charge-status pin on this board to ask properly. */
  return battery_mv() > 4250;
}

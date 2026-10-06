/* The BMI270. See imu.h.
 *
 * Started as M5Stack's M5Unified starts it, from Bosch's sequence: soft
 * reset, power save off, the configuration uploaded, INIT_CTRL set, then the
 * sensors powered. The ranges are written rather than left at reset: +-2 g
 * gives a level the finest steps (16384 per g), +-500 dps is plenty for a
 * hand-held thing (65.5 per dps).
 */
#include "kernel/drv/imu.h"
#include "kernel/drv/board.h"
#include "kernel/drv/bmi270_config.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define R_CHIP_ID   0x00
#define R_DATA      0x0C          /* ax ay az gx gy gz, little-endian int16 */
#define R_STATUS    0x21          /* INTERNAL_STATUS: 1 = initialised */
#define R_ACC_CONF  0x40
#define R_ACC_RANGE 0x41
#define R_GYR_CONF  0x42
#define R_GYR_RANGE 0x43
#define R_INIT_CTRL 0x59
#define R_INIT_ADDR 0x5B
#define R_INIT_DATA 0x5E
#define R_PWR_CONF  0x7C
#define R_PWR_CTRL  0x7D
#define R_CMD       0x7E
#define CHIP_ID     0x24
#define CHUNK       256

static const char *TAG = "imu";
static uint8_t s_addr;           /* 0x68 or 0x69 once found */
static int     s_state;          /* 0 not tried, 1 running, -1 failed */
static int     s_tries;

static int wr(uint8_t reg, uint8_t v) { return board_i2c_write_reg(s_addr, reg, v); }
static int rd(uint8_t reg, uint8_t *v, size_t n) { return board_i2c_read_reg(s_addr, reg, v, n); }

/* The configuration, a chunk at a time: INIT_ADDR is in 2-byte words. */
static int upload(void) {
  static uint8_t buf[CHUNK + 1];
  int at;
  for (at = 0; at < (int)sizeof bmi270_config_file; at += CHUNK) {
    int n = (int)sizeof bmi270_config_file - at;
    uint8_t addr[3];
    if (n > CHUNK) n = CHUNK;
    addr[0] = R_INIT_ADDR;
    addr[1] = (uint8_t)((at / 2) & 0x0F);
    addr[2] = (uint8_t)((at / 2) >> 4);
    if (board_i2c_write(s_addr, addr, 3) != 0) return -1;
    buf[0] = R_INIT_DATA;
    memcpy(buf + 1, bmi270_config_file + at, (size_t)n);
    if (board_i2c_write(s_addr, buf, (size_t)n + 1) != 0) return -1;
  }
  return 0;
}

static int start(void) {
  static const uint8_t ADDRS[2] = { 0x69, 0x68 };
  uint8_t v = 0;
  int i;
  if (board() != BOARD_ADV) return -1;
  for (i = 0; i < 2; i++) {
    s_addr = ADDRS[i];
    if (rd(R_CHIP_ID, &v, 1) == 0 && v == CHIP_ID) break;
  }
  if (v != CHIP_ID) { ESP_LOGW(TAG, "no BMI270"); return -1; }
  wr(R_CMD, 0xB6);                             /* soft reset */
  for (i = 0; i < 20; i++) {
    vTaskDelay(pdMS_TO_TICKS(2));
    if (rd(R_PWR_CONF, &v, 1) == 0 && v) break;
  }
  if (wr(R_PWR_CONF, 0x00) != 0) return -1;    /* advanced power save off */
  vTaskDelay(pdMS_TO_TICKS(2));
  if (wr(R_INIT_CTRL, 0x00) != 0 || upload() != 0 || wr(R_INIT_CTRL, 0x01) != 0) {
    ESP_LOGW(TAG, "configuration upload failed");
    return -1;
  }
  for (i = 0; i < 30; i++) {
    vTaskDelay(pdMS_TO_TICKS(5));
    if (rd(R_STATUS, &v, 1) == 0 && (v & 0x0F) == 0x01) break;
  }
  if ((v & 0x0F) != 0x01) { ESP_LOGW(TAG, "did not initialise (status %02x)", v); return -1; }
  wr(R_ACC_CONF, 0xA8);                        /* 100 Hz, normal filtering */
  wr(R_ACC_RANGE, 0x00);                       /* +-2 g */
  wr(R_GYR_CONF, 0xA9);                        /* 200 Hz */
  wr(R_GYR_RANGE, 0x02);                       /* +-500 dps */
  wr(R_PWR_CTRL, 0x0E);                        /* accel, gyro, temperature on */
  ESP_LOGI(TAG, "BMI270 at 0x%02x", s_addr);
  return 0;
}

static int ready(void) {
  if (s_state == 1) return 1;
  if (s_state == -1 && s_tries >= 3) return 0;
  s_tries++;
  s_state = start() == 0 ? 1 : -1;
  return s_state == 1;
}

int imu_present(void) { return ready(); }

int imu_read(ImuSample *out) {
  uint8_t b[12];
  int16_t raw[6];
  int i;
  if (!ready()) return -1;
  if (rd(R_DATA, b, sizeof b) != 0) return -1;
  for (i = 0; i < 6; i++) raw[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
  /* 16384 per g to milli-g; 65.5 per dps to tenths: x 10 / 65.5 = x 20 / 131. */
  out->ax = (int16_t)((int32_t)raw[0] * 1000 / 16384);
  out->ay = (int16_t)((int32_t)raw[1] * 1000 / 16384);
  out->az = (int16_t)((int32_t)raw[2] * 1000 / 16384);
  out->gx = (int16_t)((int32_t)raw[3] * 20 / 131);
  out->gy = (int16_t)((int32_t)raw[4] * 20 / 131);
  out->gz = (int16_t)((int32_t)raw[5] * 20 / 131);
  return 0;
}

/* ST7789V2 on the Cardputer, via ESP-IDF's esp_lcd.
 *
 * Pin map confirmed against M5GFX's own board detection: BL 38, RST 33,
 * DC/RS 34, MOSI 35, SCK 36, CS 37, and no MISO -- the panel is wired 3-wire,
 * so nothing can be read back from it.
 *
 * Geometry: the controller drives a 135x240 window inside a 240x320 frame
 * buffer, so it needs a gap of 52 in x and 40 in y in portrait. CardOS runs it
 * landscape, which swaps the axes and therefore the gaps. These panels are
 * also always colour-inverted.
 */

#include "kernel/drv/display.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#define PIN_BL   38
#define PIN_RST  33
#define PIN_DC   34
#define PIN_MOSI 35
#define PIN_SCK  36
#define PIN_CS   37

#define LCD_HOST SPI2_HOST
#define LCD_HZ   (40 * 1000 * 1000)

/* Backlight PWM, with the numbers taken from M5GFX's own board table for the
 * Cardputer rather than chosen: 256 Hz, nine bits, and a duty floor.
 *
 * The first attempt used 5 kHz and a plain percentage of full scale, and on
 * this hardware *every* level below 100% was black -- not dim, black, which
 * rules out a simple threshold. The backlight is not an LED on a resistor; it
 * is a boost converter, and it does not deliver at 5 kHz. M5GFX has driven
 * this panel for years at 256 Hz (M5GFX.cpp: _set_pwm_backlight(GPIO_NUM_38,
 * 7, 256, false, 16)) and its setBrightness never emits a duty below an
 * offset, because the converter needs a minimum on-time to run at all.
 *
 * BL_FLOOR is that offset carried across: M5GFX's formula bottoms out at
 * 34/512 of full scale, so that is where this one starts. */
#define BL_TIMER LEDC_TIMER_0
#define BL_CHAN  LEDC_CHANNEL_0
#define BL_RES   LEDC_TIMER_9_BIT
#define BL_MAX   512
#define BL_FREQ  256
#define BL_FLOOR 34

#define NVS_NS     "cardos"
#define NVS_BRIGHT "bright"

/* Portrait gaps for this panel; swapped below because we run landscape.
 *
 * If the image comes up shifted, these are the numbers to adjust -- and if it
 * comes up mirrored, flip the arguments to esp_lcd_panel_mirror(). Both are
 * one-line changes, and both are things that can only be confirmed against
 * the real panel. */
#define GAP_PORTRAIT_X 52
#define GAP_PORTRAIT_Y 40

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;

/* mirror_x, mirror_y, x_gap, y_gap. swap_xy is always on: the panel is
 * natively portrait and CardOS is landscape. */
static const struct { int mx, my, gx, gy; } ORIENT[DISPLAY_ORIENTS] = {
  { 0, 0, GAP_PORTRAIT_Y, GAP_PORTRAIT_X     },
  { 1, 0, GAP_PORTRAIT_Y, GAP_PORTRAIT_X     },
  { 0, 1, GAP_PORTRAIT_Y, GAP_PORTRAIT_X     },
  { 1, 1, GAP_PORTRAIT_Y, GAP_PORTRAIT_X     },
  { 0, 0, GAP_PORTRAIT_Y, GAP_PORTRAIT_X + 1 },
  { 1, 0, GAP_PORTRAIT_Y, GAP_PORTRAIT_X + 1 },
  { 0, 1, GAP_PORTRAIT_Y, GAP_PORTRAIT_X + 1 },
  { 1, 1, GAP_PORTRAIT_Y, GAP_PORTRAIT_X + 1 },
};

/* Confirmed on the real panel 2026-09-10: orientation 2 came out upside down,
 * so both mirror axes invert. If the image is ever a pixel out at an edge, the
 * +1 x-gap variants are indices 4..7 -- use the `flip` command to find it
 * rather than guessing.
 *
 * 2026-09-11: it was. Orientation 1 left the bottom row of the glass unwritten
 * -- a line of whatever the controller's RAM held at power-up -- because
 * 240 - 135 is odd and the margin on this side is 53, not 52. Orientation 5
 * is the same mirrors with the wider gap. */
#define DISPLAY_ORIENT_DEFAULT 5
static int s_orient = DISPLAY_ORIENT_DEFAULT;

int display_orient(void) { return s_orient; }

void display_set_orient(int n) {
  if (!s_panel) return;
  s_orient = ((n % DISPLAY_ORIENTS) + DISPLAY_ORIENTS) % DISPLAY_ORIENTS;
  esp_lcd_panel_swap_xy(s_panel, true);
  esp_lcd_panel_mirror(s_panel, ORIENT[s_orient].mx, ORIENT[s_orient].my);
  esp_lcd_panel_set_gap(s_panel, ORIENT[s_orient].gx, ORIENT[s_orient].gy);
}

int display_init(void) {
  esp_lcd_panel_io_handle_t io = NULL;

  /* 256 Hz: what the converter behind this backlight will switch at. Above
   * the eye's flicker threshold, and the same rate M5GFX uses on this board. */
  ledc_timer_config_t bl_timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = BL_RES,
    .timer_num = BL_TIMER,
    .freq_hz = BL_FREQ,
    .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_channel_config_t bl_chan = {
    .gpio_num = PIN_BL,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = BL_CHAN,
    .timer_sel = BL_TIMER,
    .duty = 0,                        /* dark until there is something to show */
    .hpoint = 0,
  };
  if (ledc_timer_config(&bl_timer) != ESP_OK) return -1;
  if (ledc_channel_config(&bl_chan) != ESP_OK) return -1;

  spi_bus_config_t bus = {
    .sclk_io_num = PIN_SCK,
    .mosi_io_num = PIN_MOSI,
    .miso_io_num = -1,                /* 3-wire: the panel cannot be read */
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = DISPLAY_W * DISPLAY_H * 2 + 8,
  };
  if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize failed");
    return -1;
  }

  esp_lcd_panel_io_spi_config_t io_cfg = {
    .dc_gpio_num = PIN_DC,
    .cs_gpio_num = PIN_CS,
    .pclk_hz = LCD_HZ,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .spi_mode = 0,
    .trans_queue_depth = 10,
  };
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) {
    ESP_LOGE(TAG, "panel_io_spi failed");
    return -1;
  }

  esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = PIN_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,
  };
  if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
    ESP_LOGE(TAG, "new_panel_st7789 failed");
    return -1;
  }

  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);
  esp_lcd_panel_invert_color(s_panel, true);   /* these panels are inverted */
  display_set_orient(s_orient);
  esp_lcd_panel_disp_on_off(s_panel, true);

  display_fill(COLOR_BLACK);
  return 0;
}

void display_blit(int x, int y, int w, int h, const uint16_t *pixels) {
  if (!s_panel || w <= 0 || h <= 0) return;
  esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, pixels);
}

void display_fill(uint16_t color) {
  /* One row at a time: a full-screen 240x135 buffer is 65 KB, which is a fifth
   * of the entire heap on this board. A 480-byte row costs nothing. */
  static uint16_t row[DISPLAY_W];
  int i, y;
  for (i = 0; i < DISPLAY_W; i++) row[i] = color;
  for (y = 0; y < DISPLAY_H; y++) display_blit(0, y, DISPLAY_W, 1, row);
}

/* ---- backlight ----------------------------------------------------------
 *
 * One level, applied whenever the backlight is on. Brightness lives in NVS
 * like the other settings; nothing but display_set_brightness writes it. */

static int s_bright = DISPLAY_BRIGHT_DEFAULT;
static int s_bl_on;

static void bl_apply(void) {
  /* Percent to duty, over the floor rather than from zero: below BL_FLOOR the
   * converter does not run and the panel is black however tidy the arithmetic
   * looks. So 0% of the *setting* is the dimmest the hardware can actually
   * hold, not the dimmest number. Off is still off -- that is what s_bl_on is
   * for, and it is the one case that may sit under the floor. */
  uint32_t span = BL_MAX - BL_FLOOR;
  uint32_t duty = s_bl_on
      ? (uint32_t)(BL_FLOOR + (span * (uint32_t)s_bright + 50) / 100) : 0;
  if (duty > BL_MAX) duty = BL_MAX;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHAN, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHAN);
}

void display_backlight(int on) {
  s_bl_on = on ? 1 : 0;
  bl_apply();
}

int display_brightness(void) { return s_bright; }

/* Apply a level without saving it. Safe mode lights the panel this way: the
 * whole point is that it changes nothing, so the setting you are about to
 * inspect is still the one that was there. */
void display_set_brightness_now(int pct) {
  if (pct < DISPLAY_BRIGHT_MIN) pct = DISPLAY_BRIGHT_MIN;
  if (pct > 100) pct = 100;
  s_bright = pct;
  bl_apply();
}

void display_set_brightness(int pct) {
  nvs_handle_t h;
  if (pct < DISPLAY_BRIGHT_MIN) pct = DISPLAY_BRIGHT_MIN;
  if (pct > 100) pct = 100;
  s_bright = pct;
  bl_apply();
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_BRIGHT, (uint8_t)pct);
  nvs_commit(h);
  nvs_close(h);
}

void display_load_brightness(void) {
  nvs_handle_t h;
  uint8_t v;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  /* A stored level below the floor is not honoured: it was written by an
   * older build whose floor was lower, and applying it here would black the
   * panel out again on the first boot after the fix. */
  if (nvs_get_u8(h, NVS_BRIGHT, &v) == ESP_OK && v <= 100)
    s_bright = v < DISPLAY_BRIGHT_MIN ? DISPLAY_BRIGHT_MIN : v;
  nvs_close(h);
  bl_apply();
}

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

#include "drv/display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
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

int display_init(void) {
  esp_lcd_panel_io_handle_t io = NULL;

  gpio_config_t bl = {
    .pin_bit_mask = 1ULL << PIN_BL,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&bl) != ESP_OK) return -1;
  gpio_set_level(PIN_BL, 0);          /* dark until there is something to show */

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
  esp_lcd_panel_swap_xy(s_panel, true);        /* portrait 135x240 -> 240x135 */
  esp_lcd_panel_mirror(s_panel, false, true);
  esp_lcd_panel_set_gap(s_panel, GAP_PORTRAIT_Y, GAP_PORTRAIT_X);
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

void display_backlight(int on) {
  gpio_set_level(PIN_BL, on ? 1 : 0);
}

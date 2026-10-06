/* Which Cardputer, and the ADV's I2C bus. See board.h. */
#include "kernel/drv/board.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PIN_SDA     8
#define PIN_SCL     9
#define ADDR_TCA    0x34          /* the ADV's keyboard controller */
#define I2C_HZ      400000
#define I2C_MS      50
#define MAX_DEVS    6

static const char *TAG = "board";

static Board s_board;
static int   s_known;
static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_lock;
static struct { uint8_t addr; i2c_master_dev_handle_t h; } s_dev[MAX_DEVS];
static int s_ndev;

static int bus_up(void) {
  i2c_master_bus_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.i2c_port = -1;                       /* any free port */
  cfg.sda_io_num = PIN_SDA;
  cfg.scl_io_num = PIN_SCL;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = 1;    /* the ADV has its own; harmless */
  return i2c_new_master_bus(&cfg, &s_bus) == ESP_OK ? 0 : -1;
}

Board board_detect(void) {
  if (s_known) return s_board;
  s_known = 1;
  s_board = BOARD_ORIGINAL;
  if (bus_up() != 0) {
    ESP_LOGW(TAG, "no I2C bus: taking this for the original Cardputer");
    return s_board;
  }
  if (i2c_master_probe(s_bus, ADDR_TCA, I2C_MS) == ESP_OK) {
    s_board = BOARD_ADV;
    s_lock = xSemaphoreCreateMutex();
  } else {
    /* The original: 8 and 9 are the keyboard's, and keyboard_init will
     * configure them. Give them back exactly as they were. */
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    gpio_reset_pin(PIN_SDA);
    gpio_reset_pin(PIN_SCL);
  }
  ESP_LOGI(TAG, "%s", board_name());
  return s_board;
}

Board board(void) { return s_known ? s_board : board_detect(); }

const char *board_name(void) {
  return board() == BOARD_ADV ? "Cardputer ADV" : "Cardputer";
}

/* A device handle per address, made the first time it is asked for. */
static i2c_master_dev_handle_t dev_for(uint8_t addr) {
  i2c_device_config_t cfg;
  int i;
  for (i = 0; i < s_ndev; i++) if (s_dev[i].addr == addr) return s_dev[i].h;
  if (s_ndev >= MAX_DEVS) return NULL;
  memset(&cfg, 0, sizeof cfg);
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = addr;
  cfg.scl_speed_hz = addr == 0x18 ? 100000 : I2C_HZ;   /* the ES8311 at M5's speed */
  if (i2c_master_bus_add_device(s_bus, &cfg, &s_dev[s_ndev].h) != ESP_OK) return NULL;
  s_dev[s_ndev].addr = addr;
  return s_dev[s_ndev++].h;
}

static int locked(void) {
  return s_bus && s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE;
}

int board_i2c_write(uint8_t addr, const uint8_t *data, size_t n) {
  i2c_master_dev_handle_t h;
  int rc = -1;
  if (!locked()) return -1;
  if ((h = dev_for(addr)) != NULL && i2c_master_transmit(h, data, n, I2C_MS) == ESP_OK) rc = 0;
  xSemaphoreGive(s_lock);
  return rc;
}

int board_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
  uint8_t b[2];
  b[0] = reg;
  b[1] = value;
  return board_i2c_write(addr, b, 2);
}

int board_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out, size_t n) {
  i2c_master_dev_handle_t h;
  int rc = -1;
  if (!locked()) return -1;
  if ((h = dev_for(addr)) != NULL &&
      i2c_master_transmit_receive(h, &reg, 1, out, n, I2C_MS) == ESP_OK) rc = 0;
  xSemaphoreGive(s_lock);
  return rc;
}

int board_i2c_present(uint8_t addr) {
  int ok = 0;
  if (!locked()) return 0;
  ok = i2c_master_probe(s_bus, addr, I2C_MS) == ESP_OK;
  xSemaphoreGive(s_lock);
  return ok;
}

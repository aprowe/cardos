/* USB disk mode. See usbdisk.h. */
#include "kernel/drv/usbdisk.h"
#include "kernel/drv/keyboard.h"
#include "kernel/drv/display.h"
#include "kernel/console/console.h"
#include "kernel/fs/fs.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "usbdisk";

static void wait_then_restart(void) {
  /* The `d` that asked for this is probably still down: let it go first,
   * or the restart is immediate. */
  while (keyboard_any_down()) vTaskDelay(pdMS_TO_TICKS(20));
  while (!keyboard_any_down()) vTaskDelay(pdMS_TO_TICKS(50));
  con_write("\nrestarting...\n");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
}

void usbdisk_run(void) {
  sdmmc_card_t *card;
  tinyusb_msc_storage_handle_t storage;
  tinyusb_msc_driver_config_t drv = { 0 };
  tinyusb_msc_storage_config_t cfg = { 0 };
  const tinyusb_config_t tusb = TINYUSB_DEFAULT_CONFIG();
  uint64_t mb;

  con_clear();
  con_set_color(COLOR_AMBER);
  con_write("USB DISK MODE\n\n");
  con_set_color(COLOR_GREY);

  card = (sdmmc_card_t *)fs_raw_card();
  if (!card) {
    con_set_color(COLOR_RED);
    con_write("No SD card, or it would not start.\n");
    con_set_color(COLOR_GREY);
    con_write("\nAny key restarts into CardOS.\n");
    wait_then_restart();
  }
  mb = (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024);

  drv.callback = NULL;
  cfg.medium.card = card;
  cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
  cfg.fat_fs.do_not_format = true;        /* never format someone's card */
  if (tinyusb_msc_install_driver(&drv) != ESP_OK ||
      tinyusb_msc_new_storage_sdmmc(&cfg, &storage) != ESP_OK ||
      tinyusb_driver_install(&tusb) != ESP_OK) {
    ESP_LOGE(TAG, "could not start");
    con_set_color(COLOR_RED);
    con_write("USB would not start.\n");
    con_set_color(COLOR_GREY);
    con_write("\nAny key restarts into CardOS.\n");
    wait_then_restart();
  }

  con_printf("The SD card (%u MB) is a drive on\nthe computer now. Copy what you like.\n\n",
             (unsigned)mb);
  con_write("Music goes in /music as 16-bit WAV;\n"
            "pictures are added on the dashboard.\n\n");
  con_set_color(COLOR_GREEN);
  con_write("Eject the drive on the computer,\nthen press any key to restart.\n");
  con_set_color(COLOR_GREY);
  wait_then_restart();
}

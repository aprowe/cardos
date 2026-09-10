/* CardOS entry point.
 *
 * MVP: boot, bring up the display and keyboard, and give a console you can
 * type into. No scheduler and no filesystem yet, so this is a single loop
 * rather than a task -- the shell proper arrives with those.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "console/console.h"
#include "drv/display.h"
#include "drv/keyboard.h"
#include "mem/mem.h"

#define CARDOS_LINE_MAX 63

/* The handle heap. Carved once from the IDF heap at boot; everything CardOS
 * allocates afterwards comes through mem_alloc. Deliberately not "whatever is
 * left" -- a fixed, stated size is what makes the numbers in `mem` mean
 * something, and CLAUDE.md is emphatic that memory here gets measured rather
 * than assumed. */
#define CARDOS_HEAP_BYTES (128 * 1024)

static uint8_t *s_heap;
static char     s_line[CARDOS_LINE_MAX + 1];
static int      s_len;

static void prompt(void) {
  con_set_color(COLOR_AMBER);
  con_write("cardos> ");
  con_set_color(COLOR_GREEN);
}

static void cmd_mem(void) {
  MemStats st;
  mem_stats(&st);
  con_printf("handle heap %u KB, %u used, %u free\n",
                 (unsigned)(st.heap_size / 1024),
                 (unsigned)(st.movable_used + st.fixed_used),
                 (unsigned)st.free_bytes);
  con_printf("handles %u/%u  compactions %u\n",
                 (unsigned)st.handles_used, (unsigned)MEM_MAX_HANDLES,
                 (unsigned)st.compactions);
  con_printf("idf heap free %u KB\n",
                 (unsigned)(esp_get_free_heap_size() / 1024));
}

static void cmd_help(void) {
  con_write("help   this list\n");
  con_write("mem    memory statistics\n");
  con_write("clear  clear the screen\n");
  con_write("echo   print the rest of the line\n");
  con_write("reboot restart the device\n");
}

/* A stand-in for the real shell, which needs the scheduler and filesystem. */
static void run_line(char *line) {
  while (*line == ' ') line++;
  if (*line == 0) return;

  if (!strcmp(line, "help"))        cmd_help();
  else if (!strcmp(line, "mem"))    cmd_mem();
  else if (!strcmp(line, "clear"))  con_clear();
  else if (!strcmp(line, "reboot")) esp_restart();
  else if (!strncmp(line, "echo ", 5)) { con_write(line + 5); con_putc('\n'); }
  else if (!strcmp(line, "echo"))   con_putc('\n');
  else con_printf("unknown command: %s\n", line);
}

void app_main(void) {
  int64_t last_blink = 0;
  int blink = 0;
  size_t heap_at_boot;

  /* Tell the bootloader this image is good. Without it, an OTA-updated CardOS
   * would roll itself back on the next reset -- see the app launcher spec. */
  esp_ota_mark_app_valid_cancel_rollback();

  if (display_init() != 0) {
    /* Nothing to show it on, so the serial port is the only channel left. */
    ESP_LOGE("cardos", "display init failed");
    return;
  }
  con_init();
  display_backlight(1);

  con_set_color(COLOR_WHITE);
  con_write("CardOS 0.1\n");
  con_set_color(COLOR_GREY);
  con_write("kernel core: memory + swap\n\n");
  con_set_color(COLOR_GREEN);

  heap_at_boot = esp_get_free_heap_size();

  if (keyboard_init() != 0) {
    con_set_color(COLOR_RED);
    con_write("keyboard init failed\n");
    con_set_color(COLOR_GREEN);
  }

  s_heap = heap_caps_malloc(CARDOS_HEAP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_heap) {
    con_set_color(COLOR_RED);
    con_printf("could not reserve %u KB for the handle heap\n",
                   (unsigned)(CARDOS_HEAP_BYTES / 1024));
    con_set_color(COLOR_GREEN);
  } else {
    mem_init(s_heap, CARDOS_HEAP_BYTES);
  }

  /* Success criterion 6: the free heap is reported and understood. */
  con_printf("heap %u KB free at boot\n", (unsigned)(heap_at_boot / 1024));
  con_printf("handle heap %u KB reserved\n",
                 (unsigned)(CARDOS_HEAP_BYTES / 1024));
  con_write("type help\n\n");

  prompt();

  for (;;) {
    uint8_t k = keyboard_poll();

    if (k) {
      con_cursor(0);
      if (k == KEY_ENTER) {
        s_line[s_len] = 0;
        con_putc('\n');
        run_line(s_line);
        s_len = 0;
        prompt();
      } else if (k == KEY_BACKSPACE) {
        if (s_len > 0) { s_len--; con_putc('\b'); }
      } else if (k == KEY_ESC) {
        s_len = 0;
        con_putc('\n');
        prompt();
      } else if (k >= 0x20 && k < 0x7F && s_len < CARDOS_LINE_MAX) {
        s_line[s_len++] = (char)k;
        con_putc((char)k);
      }
      last_blink = esp_timer_get_time();
      blink = 1;
      con_cursor(1);
    }

    {
      int64_t now = esp_timer_get_time();
      if (now - last_blink > 500000) {      /* 500 ms */
        last_blink = now;
        blink = !blink;
        con_cursor(blink);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));          /* ~100 Hz scan */
  }
}

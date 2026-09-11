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
#include "esp_partition.h"

#include "kernel/console/console.h"
#include "kernel/drv/display.h"
#include "kernel/drv/keyboard.h"
#include "kernel/mem/mem.h"
#include "kernel/task/sched.h"
#include "kernel/fs/fs.h"
#include "shellcmd.h"
#include "kernel/ui/desktop.h"
#include "kernel/drv/btmouse.h"

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
static int      s_desktop;      /* the desktop owns the screen and keys */

static void prompt(void) {
  con_set_color(COLOR_AMBER);
  con_printf("%s> ", shell_cwd());
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

static const char *state_name(TaskState st) {
  switch (st) {
  case TASK_READY:    return "ready";
  case TASK_RUNNING:  return "run";
  case TASK_SLEEPING: return "sleep";
  case TASK_BLOCKED:  return "block";
  case TASK_DEAD:     return "dead";
  default:            return "free";
  }
}

static void cmd_ps(void) {
  TaskInfo info[SCHED_MAX_TASKS];
  int n = sched_list(info, SCHED_MAX_TASKS), i;
  con_write("tid  state  slices name\n");
  for (i = 0; i < n; i++)
    con_printf("%-4u %-6s %-6u %s\n", (unsigned)info[i].tid,
               state_name(info[i].state), (unsigned)info[i].slices,
               info[i].name);
}

static void cmd_help(void) {
  con_write("ls cd pwd cat mkdir rm df\n");
  con_write("apps boot bootinfo\n");
  con_write("mem ps clear echo reboot help\n");
}

/* A stand-in for the real shell, which needs the context switch so it can run
 * as a task and block on the keyboard rather than polling it. */
static void run_line(char *line) {
  char *arg;

  while (*line == ' ') line++;
  if (*line == 0) return;

  /* Split off the first argument at the first space. */
  arg = strchr(line, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
  } else {
    arg = line + strlen(line);      /* empty, never NULL */
  }

  if (!strcmp(line, "help"))        cmd_help();
  else if (!strcmp(line, "mem"))    cmd_mem();
  else if (!strcmp(line, "ps"))     cmd_ps();
  else if (!strcmp(line, "ls"))     cmd_ls(arg);
  else if (!strcmp(line, "cd"))     cmd_cd(arg);
  else if (!strcmp(line, "pwd"))    cmd_pwd();
  else if (!strcmp(line, "cat"))    cmd_cat(arg);
  else if (!strcmp(line, "mkdir"))  cmd_mkdir(arg);
  else if (!strcmp(line, "rm"))     cmd_rm(arg);
  else if (!strcmp(line, "df"))     cmd_df();
  else if (!strcmp(line, "apps"))   cmd_apps();
  else if (!strcmp(line, "boot"))   cmd_boot(arg, 0);
  else if (!strcmp(line, "boot!"))  cmd_boot(arg, 1);
  else if (!strcmp(line, "bootinfo")) cmd_bootinfo();
  else if (!strcmp(line, "taskcost")) cmd_taskcost();
  else if (!strcmp(line, "mouse")) cmd_mouse(arg);
  else if (!strcmp(line, "flip")) {
    display_set_orient(display_orient() + 1);
    con_clear();
    con_printf("orientation %d of %d\n", display_orient(), DISPLAY_ORIENTS);
    con_write("flip again if this is not right\n");
  }
  else if (!strcmp(line, "desk")) {
    desktop_init();
    s_desktop = 1;
  }
  else if (!strcmp(line, "clear"))  con_clear();
  else if (!strcmp(line, "reboot")) esp_restart();
  else if (!strcmp(line, "echo"))   { con_write(arg); con_putc('\n'); }
  else con_printf("unknown command: %s\n", line);
}

/* Monotonic milliseconds for the scheduler. */
static uint32_t clock_ms(void *ctx) {
  (void)ctx;
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void app_main(void) {
  int64_t last_blink = 0;
  int blink = 0;
  size_t heap_at_boot;

  /* Tell the bootloader this image is good, so an OTA-updated CardOS does not
   * roll itself back on the next reset -- see the app launcher spec. Only
   * meaningful from an OTA slot: running from factory it just logs an error,
   * which is noise on every boot. */
  {
    const esp_partition_t *self = esp_ota_get_running_partition();
    if (self && self->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        self->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15)
      esp_ota_mark_app_valid_cancel_rollback();
  }

  if (display_init() != 0) {
    /* Nothing to show it on, so the serial port is the only channel left. */
    ESP_LOGE("cardos", "display init failed");
    return;
  }
  con_init();
  con_set_serial(1);      /* same text on the panel and the wire */
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

  /* The scheduler's policy half runs now; the Xtensa context switch does not
   * exist yet, so this loop *is* the shell task rather than being switched to
   * it. `ps` therefore shows one task. spawn/kill arrive with the switch. */
  sched_init(clock_ms, NULL);
  sched_create("shell");
  sched_next();                  /* mark it running, so it owns its locks */

  /* A missing card is a normal condition, not a boot failure: CardOS runs
   * without one, just without apps or swap. Say which, rather than leaving
   * the user to guess why `ls` is empty. */
  if (fs_mount() == 0) {
    uint64_t total = 0, freeb = 0;
    fs_ensure_layout();
    fs_space(&total, &freeb);
    con_printf("sd %u MB, %u MB free\n",
               (unsigned)(total / (1024 * 1024)),
               (unsigned)(freeb / (1024 * 1024)));
  } else {
    con_set_color(COLOR_GREY);
    con_write("no sd card: no apps, no swap\n");
    con_set_color(COLOR_GREEN);
  }

  /* Success criterion 6: the free heap is reported and understood. */
  con_printf("heap %u KB free at boot\n", (unsigned)(heap_at_boot / 1024));
  con_printf("handle heap %u KB reserved\n",
                 (unsigned)(CARDOS_HEAP_BYTES / 1024));
  con_write("type help\n\n");

  /* Came back from a firmware the desktop launched: return there, rather than
   * to a console nobody asked for. */
  if (desktop_autostart()) {
    desktop_init();
    s_desktop = 1;
  } else {
    prompt();
  }

  for (;;) {
    uint8_t k = keyboard_poll();

    /* A character arriving on the serial console counts as a keypress, so the
     * shell can be driven from a PC as well as from the keyboard. */
    if (!k) {
      int sc = con_serial_key();
      if (sc == '\r' || sc == '\n') k = KEY_ENTER;
      else if (sc == 0x7F || sc == 0x08) k = KEY_BACKSPACE;
      else if (sc == 0x1B) k = KEY_ESC;
      else if (sc > 0) k = (uint8_t)sc;
    }

    if (s_desktop) {
      MouseReport mr;
      /* Drain whatever the radio queued. It arrives on the Bluetooth task,
       * which must not touch the display, so this is where it turns into
       * pointer movement. */
      {
        int got = 0;
        while (btmouse_poll(&mr)) { desktop_mouse_apply(&mr); got = 1; }
        if (got) desktop_mouse_done();   /* one repaint for the whole burst */
      }
      desktop_tick((uint32_t)(esp_timer_get_time() / 1000));
      if (k && desktop_key(k)) {
        /* ESC left the desktop: hand the screen back to the console. */
        s_desktop = 0;
        con_clear();
        con_write("back at the console\n");
        prompt();
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

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

    /* 5 ms while the desktop is up: the pointer wants to keep up with the
     * hand, and the keyboard matrix scan is cheap. */
    vTaskDelay(pdMS_TO_TICKS(s_desktop ? 5 : 10));
  }
}

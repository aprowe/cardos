/* CardOS entry point.
 *
 * MVP: boot, bring up the display and keyboard, and give a console you can
 * type into. No scheduler and no filesystem yet, so this is a single loop
 * rather than a task -- the shell proper arrives with those.
 */

#include "kernel/sys/prefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "psa/crypto.h"

#include "kernel/console/console.h"
#include "kernel/drv/display.h"
#include "kernel/drv/keyboard.h"
#include "kernel/drv/board.h"
#include "kernel/drv/usbdisk.h"
#include "kernel/sys/blip.h"
#include "kernel/ui/overlay.h"
#include "kernel/sys/notify.h"
#include "kernel/net/link.h"
#include "kernel/ui/sleepclock.h"
#include "kernel/drv/imu.h"
#include "kernel/fs/fs.h"
#include "shellcmd.h"
#include "kernel/fs/path.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/pins.h"
#include "kernel/ui/launchui.h"
#include "kernel/ui/icons.h"
#include "kernel/app/capprun.h"
#include "kernel/app/cmdline.h"
#include "kernel/app/capp.h"
#include "kernel/net/wifi.h"
#include "kernel/ui/shell.h"
#include "kernel/sys/env.h"
#include "kernel/sys/sio.h"
#include "kernel/ui/help.h"
#include "kernel/sys/applog.h"
#include "kernel/sys/input.h"
#include "kernel/sys/voice.h"
#include "kernel/sys/bg.h"
#include "kernel/sys/clock.h"
#include "kernel/sys/tzlookup.h"
#include "kernel/sys/busy.h"
#include "kernel/sys/shot.h"
#include "kernel/sys/serlink.h"
#include "kernel/sys/alarm.h"
#include "kernel/sys/agent.h"
#include "kernel/net/httpq.h"
#include "kernel/net/update.h"
#include "kernel/sys/power.h"
#include "kernel/drv/battery.h"
#include "kernel/sys/hotkeys.h"
#include "kernel/sys/memreport.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "kernel/drv/bthid.h"
#include "kernel/drv/speaker.h"

/* Long enough for the lines a tool sends rather than the ones a person types.
 * At 63 a Google client ID -- 72 characters on its own, 81 with the command in
 * front of it -- was silently cut in half, and the only symptom was Google
 * answering "invalid_client". A refresh token is longer still. */
#define CARDOS_LINE_MAX 200

/* The handle arena (kernel/mem), the swap allocator and the scheduler's
 * policy half are not in the firmware any more (2026-10-09). Nothing outside
 * kernel/mem ever allocated from the arena, kmem_ensure had no caller, and
 * `ps` listed the one task the loop pretended to be. The portable sources and
 * their host tests stay: they are a design the host suite still runs. */
static char     s_line[CARDOS_LINE_MAX + 1];
static int      s_len;
/* Three shells over the same kernel. The launcher is the one meant for daily
 * use, the desktop is the demonstration that windows work, and the console is
 * for development. Which one has the screen is ui_shell() (kernel/ui/shell.c)
 * and nothing else: it used to be kept here as well, in an s_mode that a
 * command opening its app, or a desktop app calling run(), could leave
 * behind -- the launcher on screen and the keys still going to the desktop.
 * go() below is the one way to change it from here. */

/* Booted with escape held: no saved setting has been applied, no radio has
 * been started, and the console has the screen. See app_main. */
static int      s_safe_mode;

static void prompt(void) {
  con_set_color(COLOR_AMBER);
  con_printf("%s> ", shell_cwd());
  con_set_color(COLOR_GREEN);
}

/* Give the screen to a shell.
 *
 * `init` 1 starts it the way its own command does: launchui_init or
 * desktop_init, which set ui_shell themselves; for the console, a cleared
 * screen, "back at the console" when it came from a painted shell, and a
 * prompt. `init` 0 only makes sure it is the one up: the launcher is started
 * if it is not already, the desktop is handed the screen back as it was
 * left, and the console gets a prompt under whatever it already shows. */
static void go(UiShell to, int init) {
  UiShell from = ui_shell();
  switch (to) {
  case UI_LAUNCHER:
    if (init || from != UI_LAUNCHER) launchui_init();
    break;
  case UI_DESKTOP:
    if (init) desktop_init();
    else if (from != UI_DESKTOP) { ui_set_shell(UI_DESKTOP); desktop_repaint(); }
    break;
  default:
    ui_set_shell(UI_NONE);
    if (init) {
      con_clear();
      if (from != UI_NONE) con_write("back at the console\n");
    }
    prompt();
    break;
  }
}

/* `mem map`: the executable heap block by block, for finding what splits
 * it. An app's image has to fit in one piece of it, and "plenty free but
 * the largest piece is small" says only that something sits in the middle,
 * not what. Collected under the walk and printed after it: the walk holds
 * the heap's lock, and printing may allocate. */
#define MAP_MAX 96
typedef struct { uintptr_t at; uint32_t size; uint8_t used; } MapRow;
static MapRow *s_map;            /* for the one command, not the uptime */
static int s_map_n, s_map_more;

/* Kind: 0 a run of small used blocks (summed), 1 a big used block, 2 a
 * free gap. Small ones are summed as the walk goes, so the list holds the
 * shape of the heap rather than its first hundred blocks. */
static uint32_t s_small, s_small_n;

static void map_add(uintptr_t at, uint32_t size, uint8_t kind) {
  if (s_map_n >= MAP_MAX) { s_map_more++; return; }
  s_map[s_map_n].at = at;
  s_map[s_map_n].size = size;
  s_map[s_map_n].used = kind;
  s_map_n++;
}

static bool map_block(walker_heap_into_t heap, walker_block_info_t b, void *ctx) {
  (void)heap; (void)ctx;
  if (b.used && b.size < 2048) { s_small += (uint32_t)b.size; s_small_n++; return true; }
  if (!b.used && b.size < 1024) return true;
  if (s_small_n) { map_add(0, s_small, 0); s_small = s_small_n = 0; }
  map_add((uintptr_t)b.ptr, (uint32_t)b.size, b.used ? 1 : 2);
  return true;
}

static void cmd_mem_map(void) {
  int i;
  if ((s_map = malloc(MAP_MAX * sizeof *s_map)) == NULL) {
    con_write("no memory for the map\n");
    return;
  }
  s_map_n = s_map_more = 0;
  s_small = s_small_n = 0;
  heap_caps_walk(MALLOC_CAP_EXEC, map_block, NULL);
  if (s_small_n) map_add(0, s_small, 0);
  for (i = 0; i < s_map_n; i++) {
    if (s_map[i].used == 0) con_printf("           small used %6u\n", (unsigned)s_map[i].size);
    else con_printf("  %08x %s %6u\n", (unsigned)s_map[i].at,
                    s_map[i].used == 1 ? "used" : "FREE", (unsigned)s_map[i].size);
  }
  if (s_map_more) con_printf("  (%d more)\n", s_map_more);
  free(s_map);
  s_map = NULL;
}

static void mem_line(const char *line, void *ctx) { (void)ctx; con_printf("%s\n", line); }

/* The same lines the Memory app shows (kernel/sys/memreport.c). */
static void cmd_mem(void) { mem_report(mem_line, NULL); }

/* Grouped by what you are trying to do, and kept next to the dispatcher so the
 * two are edited together -- a help text that drifts is worse than none. */
static void cmd_help(void) {
  con_write("files    ls cd pwd mkdir rm df\n");
  con_write("run      run NAME [args], ./prog, or just the name\n");
  con_write("         | pipes, > and >> redirect, < feeds stdin\n");
  con_write("shell    env set NAME=VALUE, hotkey X NAME, tab completes\n");
  con_write("         set TZ=PST8PDT,M3.2.0,M11.1.0 -- clocks are UTC until\n");
  con_write("         you do; every app reads it\n");
  con_write("radios   wifi [scan|SSID PASS|saved|forget|off]\n");
  con_write("         mouse, get URL, share [off|log] (card as a drive)\n");
  con_write("         usbdisk (card as a USB drive; or hold d at power-on)\n");
  con_write("         print [scan|use N|test|FILE] (bluetooth thermal printer)\n");
  con_write("screens  launch (carousel), desk (windows), escape returns\n");
  con_write("boot     apps, boot NAME, boot! NAME, bootinfo\n");
  con_write("system   mem taskcost flip clear reboot echo\n");
  con_write("         log [N|clear] -- what the apps wrote to the card\n");
  con_write("         time (ntp; no rtc on this board), battery\n");
  con_write("voice    hold the button on top, or type listen\n");
  con_write("shot     screenshot to /shots and the proxy (shot NAME)\n");
  con_write("recovery opt-0 backlight to full, hold escape at boot for safe\n");
  con_write("         mode, then defaults to clear saved settings\n");
  con_write("on card  cat grep -- run with no argument lists them\n");
}

/* What the apps have been writing to /cache/app.log. The card is where an
 * intermittent fault is actually caught: the USB serial port is not attached
 * when the device is in a pocket, and that is when the sync fails. */
static void log_line(const char *s) { con_write(s); con_write("\n"); }

static void cmd_log(const char *arg) {
  int n;
  if (arg && !strcmp(arg, "clear")) {
    applog_clear();
    con_write("log cleared\n");
    return;
  }
  n = applog_tail(arg && arg[0] ? atoi(arg) : 20, log_line);
  if (!n) con_write("nothing logged yet (" APPLOG_PATH ")\n");
}

/* A stand-in for the real shell, which needs the context switch so it can run
 * as a task and block on the keyboard rather than polling it. */
/* One command, already separated from its pipeline and redirections. */
/* `do` with nothing, or with an app's name: that app's commands from the
 * catalog (/cache/commands.txt, written by the icon scan). 1 if it listed
 * something, 0 to fall through to the older `do ID` on the focused app. */
static int do_catalog(const char *what) {
  /* Whole, not line by line: a few lines per app, and 4 KB is dozens of
   * apps' worth. Allocated for the listing, not static: a static buffer held
   * its 4 KB of heap for good, for a command typed now and then. */
  enum { CAT_MAX = 4096 };
  char *buf, app[24], *line, *next;
  int fd, n, shown = 0;
  size_t alen;

  while (*what == ' ') what++;
  alen = strcspn(what, " ");
  if (what[alen] || alen >= sizeof app) return 0;   /* not a listing */
  snprintf(app, sizeof app, "%.*s", (int)alen, what);

  fd = fs_open(CAPPRUN_CATALOG, FS_O_READ);
  if (fd < 0) return 0;
  buf = malloc(CAT_MAX);
  if (!buf) { fs_close(fd); con_write("no memory for the list\n"); return 1; }
  n = fs_read(fd, buf, CAT_MAX - 1);
  fs_close(fd);
  buf[n > 0 ? n : 0] = 0;

  for (line = buf; *line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0; else next = line + strlen(line);
    if (!alen || (!strncmp(line, app, alen) && line[alen] == ' ')) {
      con_printf("  %s\n", line);
      shown++;
    }
  }
  free(buf);
  if (!shown && alen) return 0;             /* not an app with commands */
  if (!shown) con_write("no app has commands yet\n");
  return 1;
}

/* `do APP COMMAND ARGS...`: one of an app's commands, open or not, checked
 * against what it declared (kernel/app/capprun.c). 0 when the first word is
 * not an app with commands, so a lone `do ID` still reaches the focused
 * app's action as before. */
static int do_command(const char *rest) {
  static char buf[CAPPRUN_CMD_TEXT], out[512];
  const char *w[2 + CAPP_CMD_ARGS_MAX + 8];
  int n, rc;
  n = cmdline_split(rest, buf, sizeof buf, w, (int)(sizeof w / sizeof w[0]));
  if (n < 2) {
    if (n < 0) { con_write("do: an unclosed quote, or too long\n"); return 1; }
    return 0;
  }
  rc = capprun_command(w[0], w[1], n - 2, w + 2, out, sizeof out);
  if (out[0]) con_printf("%s%s", out, out[strlen(out) - 1] == '\n' ? "" : "\n");
  else if (rc == 0) con_write("done\n");
  return 1;
}

static void run_builtin(const char *line, char *arg) {
  if (!strcmp(line, "help"))        cmd_help();
  else if (!strcmp(line, "log"))    cmd_log(arg);
  else if (!strcmp(line, "mem"))    { if (arg && !strcmp(arg, "map")) cmd_mem_map(); else cmd_mem(); }
  /* One reading of the ADV's motion sensor, raw axes: the way to learn which
   * way they point on a new board. */
  else if (!strcmp(line, "motion")) {
    ImuSample s;
    if (imu_read(&s) != 0) con_printf("no motion sensor on this %s\n", board_name());
    else con_printf("accel %d %d %d mg\ngyro  %d %d %d (0.1 dps)\n",
                    s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
  }
  /* The verbs the running app offers, and a way to run one.
   *
   * This is the point of the `id` field in CappAction: the same table that
   * draws the menus and the help panel is a list of things that can be asked
   * for by name. kernel/sys/rpc.c already argues it for voice -- an LLM
   * choosing between a few named verbs is useful, one handed a string to run
   * is not -- and this is that list, per app, written once by the app. */
  /* `line` is only the command word here -- run_stage splits at the first
   * space and hands the rest over as `arg`. The `do ID` below used to read
   * past "do" in `line`, found nothing, and so only ever listed. */
  else if (!strcmp(line, "do") && do_catalog(arg)) {
    /* listed from the command catalog: `do`, `do todo` */
  }
  else if (!strcmp(line, "do") && do_command(arg)) {
    /* ran one: `do todo add "fix car"` */
  }
  else if (!strcmp(line, "do")) {
    const AppDef *a = launchui_running();
    const char *what = arg;
    while (what && *what == ' ') what++;
    if (!a) con_write("no app is running\n");
    else if (!what || !*what) {
      int n = 0, i;
      const CappAction *act = capprun_actions(a, &n);
      if (!act || !n) con_printf("%s offers none\n", a->name);
      for (i = 0; i < n; i++)
        con_printf("  %-10s %s\n", act[i].id, act[i].label);
    }
    else if (capprun_action_invoke(a, what) != 0)
      con_printf("%s has no action '%s'\n", a->name, what);
  }
  else if (!strcmp(line, "ls"))     cmd_ls(arg);
  else if (!strcmp(line, "cd"))     cmd_cd(arg);
  else if (!strcmp(line, "pwd"))    cmd_pwd();
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
  else if (!strcmp(line, "desk"))   go(UI_DESKTOP, 1);
  else if (!strcmp(line, "launch") || !strcmp(line, "gui")) go(UI_LAUNCHER, 1);
  else if (!strcmp(line, "wifi")) cmd_wifi(arg);
  else if (!strcmp(line, "get"))  cmd_get(arg);
  /* The same as d held at power-on, from here: the card is unmounted under
   * everything, so it ends in a restart. */
  else if (!strcmp(line, "usbdisk")) {
    if (con_capturing()) { con_write("usbdisk takes the USB port; not from the dashboard\n"); return; }
    fs_unmount();
    usbdisk_run();
  }
  else if (!strcmp(line, "update")) cmd_update(arg);
  else if (!strcmp(line, "shot")) {
    if (shot_take(arg, con_repaint) == 0)
      con_printf("%s%s%s\n", shot_last_path(), *shot_error() ? ": " : "", shot_error());
    else con_printf("shot: %s\n", shot_error());
  }
  else if (!strcmp(line, "share")) cmd_share(arg);
  else if (!strcmp(line, "print")) cmd_print(arg);
  else if (!strcmp(line, "volume")) {
    if (arg && *arg) speaker_set_volume(atoi(arg));
    if (speaker_volume()) con_printf("volume %d%%\n", speaker_volume());
    else con_write("muted\n");
  }
  else if (!strcmp(line, "env"))  cmd_env();
  else if (!strcmp(line, "set"))  cmd_set(arg);
  else if (!strcmp(line, "hotkey")) cmd_hotkey(arg);
  else if (!strcmp(line, "run"))    cmd_run(arg);
  else if (!strcmp(line, "clear"))  con_clear();
  else if (!strcmp(line, "reboot")) esp_restart();
  else if (!strcmp(line, "defaults")) {
    /* Everything CardOS saves lives in one NVS namespace, so forgetting all of
     * it is one call. Deliberately not selective: the reason to be here is
     * that some setting is making the machine unusable and you cannot
     * necessarily see which. WiFi credentials go too -- say so rather than
     * letting that be a surprise. */
    nvs_handle_t h;
    if (nvs_open("cardos", NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_all(h);
      nvs_commit(h);
      nvs_close(h);
      prefs_forget_file();       /* or the next boot would put them all back */
      con_write("settings cleared: brightness, shell, PATH, bluetooth-at-boot.\n");
      con_write("wifi credentials are kept by the driver and survive this.\n");
      con_write("reboot to start fresh.\n");
    } else {
      con_write("could not open the settings store\n");
    }
  }
  else if (!strcmp(line, "time")) {
    char when[40];
    clock_full(when, sizeof when);
    con_printf("%s\n", when);
    /* Which zone that is in. NTP hands over UTC and nothing on this board
     * knows better, so a device with no TZ shows UTC and looks exactly like
     * one whose clock is simply wrong -- which is how a calendar event at
     * half five in the afternoon read as half past midnight the next day. */
    con_printf("zone     %s\n", clock_zone());
    if (!clock_zone_set())
      con_write("         set TZ=PST8PDT,M3.2.0,M11.1.0 (or your own)\n");
    if (!clock_synced()) {
      con_write("no rtc on this board: the time comes from the network\n");
      con_write("and is lost at every power cut. syncing...\n");
      bg_submit(BG_TIME_SYNC);
    }
  }
  else if (!strcmp(line, "battery")) {
    int mv = battery_mv(), pct = battery_percent();
    if (mv <= 0) con_write("no reading from the battery ADC\n");
    else con_printf("%d%%  %d.%02d V%s\n", pct, mv / 1000, (mv % 1000) / 10,
                    battery_charging() ? "  (charging)" : "");
  }
  else if (!strcmp(line, "listen")) {
    /* The button, without the button -- for trying voice over the serial
     * line, where there is no button to hold. */
    con_write("listening...\n");
    voice_once(6000, 0);   /* no button held: run the clock out */
    con_printf("%s\n", voice_status());
  }
  else if (!strcmp(line, "safe")) {
    con_printf("safe mode: %s\n", s_safe_mode ? "yes" : "no");
    con_write("hold the escape key while the device starts to enter it.\n");
  }
  /* To stdout, so `echo text > file` writes a file rather than printing. */
  else if (!strcmp(line, "echo"))   sio_write_line(arg);
  else {
    /* Not a built-in: try to run it. "./grep x" is a path and "grep x" is a
     * PATH lookup, and both end in the same place -- which is what makes a
     * command that lives on the card indistinguishable from one that does
     * not. */
    if (shell_exec(line, (arg && *arg) ? arg : NULL) != 0)
      con_printf("unknown command: %s\n", line);
  }
}


/* ---- tab completion -----------------------------------------------------
 *
 * Completes the command when the cursor is still in the first word, and a path
 * otherwise. Only one list is kept -- the commands -- and it is the same one
 * `help` prints, so a command added in one place cannot go missing from the
 * other.
 *
 * On several matches it completes as far as they agree and lists them, which
 * is what every shell does and the only behaviour that is useful when you have
 * half-remembered a name. */
static const char *const COMMANDS[] = {
  "apps", "boot", "boot!", "bootinfo", "cat", "cd", "clear", "df", "desk",
  "echo", "flip", "get", "gui", "help", "launch", "log", "ls", "mem", "mkdir", "motion",
  "battery", "defaults", "listen", "mouse", "pwd", "reboot", "rm",
  "run", "time",
  "safe",
  "print", "share", "shot", "taskcost", "update", "usbdisk", "volume", "wifi",
};
#define NCOMMANDS ((int)(sizeof COMMANDS / sizeof COMMANDS[0]))

/* How many leading characters `a` and `b` share. */
static int common(const char *a, const char *b) {
  int i = 0;
  while (a[i] && b[i] && a[i] == b[i]) i++;
  return i;
}

/* Split the path being typed into the directory to list and the stem to match
 * inside it. "/desk" -> "/" and "desk"; "/apps/mi" -> "/apps" and "mi". */
static void split_path(const char *word, char *dir, size_t dirn, const char **stem) {
  const char *slash = strrchr(word, '/');
  if (!slash) {
    snprintf(dir, dirn, "%s", ".");
    *stem = word;
    return;
  }
  if (slash == word) snprintf(dir, dirn, "%s", "/");
  else snprintf(dir, dirn, "%.*s", (int)(slash - word), word);
  *stem = slash + 1;
}

/* The directory to list for a completion, as something fs_opendir accepts:
 * absolute paths as they are, everything else under the current directory.
 * Only `.` used to be resolved, so `ls Games/mi<Tab>` opened "Games", which
 * the path normaliser refuses as relative, and offered nothing. */
static void completion_dir(const char *dir, char *full, size_t n) {
  if (dir[0] == '.' && dir[1] == 0) snprintf(full, n, "%s", shell_cwd());
  else if (dir[0] == '/') snprintf(full, n, "%s", dir);
  else {
    const char *cwd = shell_cwd();
    int root = cwd[0] == '/' && cwd[1] == 0;
    snprintf(full, n, "%s%s%s", cwd, root ? "" : "/", dir);
  }
}

/* Collects matches, tracking the longest shared prefix and printing them if
 * there is more than one. Returns what to append to what is already typed. */
typedef struct {
  const char *stem;
  size_t      stem_len;
  char        best[CARDOS_LINE_MAX + 1];
  int         count;
} Complete;

static void offer(Complete *c, const char *name) {
  if (strncmp(name, c->stem, c->stem_len) != 0) return;
  if (c->count == 0) snprintf(c->best, sizeof c->best, "%s", name);
  else c->best[common(c->best, name)] = 0;
  c->count++;
}

static void list_again(Complete *c, const char *what) {
  (void)c;
  (void)what;
}

static void complete_line(void) {
  Complete c;
  const char *word;
  int i, word_start;
  int is_first_word;

  s_line[s_len] = 0;

  /* The word under the cursor starts after the last space. */
  word_start = s_len;
  while (word_start > 0 && s_line[word_start - 1] != ' ') word_start--;
  word = s_line + word_start;
  is_first_word = (word_start == 0);

  memset(&c, 0, sizeof c);
  c.stem = word;
  c.stem_len = strlen(word);

  if (is_first_word) {
    for (i = 0; i < NCOMMANDS; i++) offer(&c, COMMANDS[i]);
  } else {
    char dir[FS_PATH_MAX], full[FS_PATH_MAX];
    const char *stem;
    FsDir d;
    FsEntry e;

    split_path(word, dir, sizeof dir, &stem);
    c.stem = stem;
    c.stem_len = strlen(stem);
    completion_dir(dir, full, sizeof full);

    if (fs_opendir(full, &d) == 0) {
      while (fs_readdir(&d, &e) == 1) offer(&c, e.name);
      fs_closedir(&d);
    }
  }

  if (c.count == 0) return;

  /* Append whatever is agreed and not already typed. */
  {
    size_t have = c.stem_len, want = strlen(c.best);
    while (have < want && s_len < CARDOS_LINE_MAX) {
      s_line[s_len++] = c.best[have++];
      con_putc(c.best[have - 1]);
    }
    s_line[s_len] = 0;
    if (c.count == 1 && s_len < CARDOS_LINE_MAX) {
      s_line[s_len++] = ' ';
      con_putc(' ');
      s_line[s_len] = 0;
    }
  }

  /* More than one, and nothing left to agree on: show what they are. */
  if (c.count > 1 && strlen(c.best) == c.stem_len) {
    con_putc('\n');
    if (is_first_word) {
      for (i = 0; i < NCOMMANDS; i++)
        if (strncmp(COMMANDS[i], c.stem, c.stem_len) == 0)
          con_printf("%s  ", COMMANDS[i]);
    } else {
      char dir[FS_PATH_MAX], full[FS_PATH_MAX];
      const char *stem;
      FsDir d;
      FsEntry e;
      split_path(word, dir, sizeof dir, &stem);
      completion_dir(dir, full, sizeof full);
      if (fs_opendir(full, &d) == 0) {
        while (fs_readdir(&d, &e) == 1)
          if (strncmp(e.name, stem, strlen(stem)) == 0) con_printf("%s  ", e.name);
        fs_closedir(&d);
      }
    }
    con_putc('\n');
    prompt();
    con_write(s_line);
  }
  (void)list_again;
}


/* --------------------------------------------------------------- pipeline --
 *
 * "cat notes | grep TODO > hits" is three decisions: what runs, where its
 * output goes, and where its input comes from. The shell makes all three and
 * the programs make none, which is the whole point -- grep has no idea whether
 * it is reading a file, a pipe, or nothing at all.
 *
 * Pipes are buffered rather than concurrent. With one cooperative loop and no
 * context switch, "both sides run at once" is not on offer: the left-hand side
 * runs to completion into a buffer and the right-hand side then reads it. The
 * only thing that changes is an infinite producer, and there are none here.
 * A pipe that looked concurrent and deadlocked would be worse than one that is
 * honest about what it does.
 */

#define PIPE_CAP   (8 * 1024)
#define MAX_STAGES 4

typedef struct {
  char text[CARDOS_LINE_MAX + 1];
  char in_file[FS_PATH_MAX];
  char out_file[FS_PATH_MAX];
  int  append;
} Stage;

/* Pull "< path" and "> path" out of a stage, leaving the command and its
 * arguments behind. Rewritten in place because a redirection is not an
 * argument, and the program must never see one. */
static void take_redirects(Stage *st) {
  char clean[CARDOS_LINE_MAX + 1];
  char quote = 0;
  int i = 0, n = 0;

  while (st->text[i]) {
    char c = st->text[i];

    /* Inside quotes an arrow is a character, as it already is for the pipe
     * split above: `echo "a > b"` used to write a file called b". The quotes
     * themselves are kept for the argument splitter. */
    if (!quote && (c == '"' || c == '\'')) quote = c;
    else if (quote && c == quote) quote = 0;

    if (!quote && (c == '<' || c == '>')) {
      char *dest = (c == '<') ? st->in_file : st->out_file;
      int m = 0;
      i++;
      if (c == '>' && st->text[i] == '>') { st->append = 1; i++; }
      while (st->text[i] == ' ') i++;
      while (st->text[i] && st->text[i] != ' ' && st->text[i] != '<' &&
             st->text[i] != '>' && m < FS_PATH_MAX - 1)
        dest[m++] = st->text[i++];
      dest[m] = 0;
      continue;
    }
    if (n < CARDOS_LINE_MAX) clean[n++] = c;
    i++;
  }
  clean[n] = 0;
  while (n > 0 && clean[n - 1] == ' ') clean[--n] = 0;
  snprintf(st->text, sizeof st->text, "%s", clean);
}

/* One stage, with stdio already pointed wherever it should go. */
static void run_stage(Stage *st) {
  char cmd[CARDOS_LINE_MAX + 1];
  char *arg;
  int i = 0;

  snprintf(cmd, sizeof cmd, "%s", st->text);
  while (cmd[i] && cmd[i] != ' ') i++;
  if (cmd[i]) {
    cmd[i] = 0;
    arg = cmd + i + 1;
    while (*arg == ' ') arg++;
  } else {
    arg = cmd + i;
  }
  if (cmd[0]) run_builtin(cmd, arg);
}

static void run_pipeline(const char *line);

/* A console line from the dashboard (api->shell, the Dashboard Link app):
 * run as typed, its output captured rather than drawn -- the screen is the
 * app's. Some lines are refused, because they take the screen: switching
 * shells, and opening an app on top of the one doing the linking, which
 * would stop the link mid-command. */
static int shell_remote(const char *line, char *out, size_t n) {
  static const char *const TAKES_SCREEN[] = {
    "launch", "desk", "gui", "flip", "run", "boot", "boot!", "clear", NULL };
  static int busy;
  char word[24];
  int i = 0, k;
  while (line[i] == ' ') i++;
  for (k = 0; line[i] && line[i] != ' ' && k < (int)sizeof word - 1; i++) word[k++] = line[i];
  word[k] = 0;
  for (k = 0; TAKES_SCREEN[k]; k++)
    if (!strcmp(word, TAKES_SCREEN[k])) {
      snprintf(out, n, "%s takes over the device's screen; not from the dashboard\n", word);
      return -1;
    }
  if (busy) { snprintf(out, n, "a command is already running\n"); return -1; }
  busy = 1;
  con_capture(out, n);
  run_pipeline(line);
  con_capture_end();
  busy = 0;
  return 0;
}

/* The whole line: split on |, wire each stage's output to the next one's
 * input, and give stdio back to the console afterwards. */
static void run_pipeline(const char *line) {
  static Stage stage[MAX_STAGES];
  int n = 0, i = 0, k;
  char *carry = NULL;

  while (line[i] && n < MAX_STAGES) {
    int m = 0;
    char quote = 0;
    while (line[i] == ' ') i++;
    /* Quotes so a pipe inside one is just a character. */
    while (line[i] && (quote || line[i] != '|')) {
      if (!quote && (line[i] == '"' || line[i] == '\'')) quote = line[i];
      else if (quote && line[i] == quote) quote = 0;
      if (m < CARDOS_LINE_MAX) stage[n].text[m++] = line[i];
      i++;
    }
    stage[n].text[m] = 0;
    while (m > 0 && stage[n].text[m - 1] == ' ') stage[n].text[--m] = 0;
    stage[n].in_file[0] = 0;
    stage[n].out_file[0] = 0;
    stage[n].append = 0;
    take_redirects(&stage[n]);
    if (stage[n].text[0]) n++;
    if (line[i] == '|') i++;
  }

  if (n == 0) return;

  for (k = 0; k < n; k++) {
    int last = (k == n - 1);

    sio_reset();

    /* stdin: a named file wins over the pipe, because it was asked for. */
    if (stage[k].in_file[0]) {
      free(carry);
      carry = NULL;
      if (sio_in_from_file(stage[k].in_file) != 0) {
        con_printf("%s: cannot read\n", stage[k].in_file);
        break;
      }
    } else if (carry) {
      sio_in_from_buffer(carry);       /* takes ownership */
      carry = NULL;
    }

    /* stdout: a named file, else a buffer if another stage follows, else the
     * console. */
    if (stage[k].out_file[0]) {
      if (sio_out_to_file(stage[k].out_file, stage[k].append) != 0) {
        con_printf("%s: cannot write\n", stage[k].out_file);
        break;
      }
    } else if (!last) {
      if (!sio_out_to_buffer(PIPE_CAP)) {
        con_write("out of memory for the pipe\n");
        break;
      }
    }

    run_stage(&stage[k]);

    if (!last && !stage[k].out_file[0]) {
      size_t len = 0;
      if (sio_out_overflowed())
        con_printf("pipe full at %d bytes, output truncated\n", PIPE_CAP);
      carry = sio_take_buffer(&len);
    }
  }

  free(carry);
  sio_reset();
}


/* ---- command history -----------------------------------------------------
 *
 * A ring of the last few lines, walked with the up and down arrows. Kept small
 * on purpose: this is a machine with 48 KB of arena and a 40-column screen,
 * and eight lines is more than anyone scrolls back through on a keyboard this
 * size.
 *
 * A line is only remembered if it differs from the one before it, because the
 * common use of history here is running the same command twice and then
 * wanting the one before that. */
#define HIST_MAX 8

static char s_hist[HIST_MAX][CARDOS_LINE_MAX + 1];
static int  s_hist_n;         /* how many are filled */
static int  s_hist_at;        /* how far back we have walked; 0 = the live line */
static char s_hist_saved[CARDOS_LINE_MAX + 1];   /* the line being typed */

static void hist_add(const char *line) {
  int i;
  if (!line || !*line) return;
  if (s_hist_n > 0 && strcmp(s_hist[0], line) == 0) return;

  for (i = HIST_MAX - 1; i > 0; i--) strcpy(s_hist[i], s_hist[i - 1]);
  snprintf(s_hist[0], sizeof s_hist[0], "%s", line);
  if (s_hist_n < HIST_MAX) s_hist_n++;
}

/* Replace what is on the line with `text`, on screen as well as in the buffer.
 * Backspacing over the old one is what the console can actually do -- there is
 * no way to repaint a line in place. */
static void line_replace(const char *text) {
  while (s_len > 0) { s_len--; con_putc(0x08); }
  snprintf(s_line, sizeof s_line, "%s", text);
  s_len = (int)strlen(s_line);
  con_write(s_line);
}

static void hist_walk(int delta) {
  int want;

  if (s_hist_n == 0) return;
  if (s_hist_at == 0 && delta > 0) {
    /* Stepping off the live line: keep it, so coming back down restores what
     * was half-typed rather than clearing it. */
    s_line[s_len] = 0;
    snprintf(s_hist_saved, sizeof s_hist_saved, "%s", s_line);
  }

  want = s_hist_at + delta;
  if (want < 0) want = 0;
  if (want > s_hist_n) want = s_hist_n;
  if (want == s_hist_at) return;

  s_hist_at = want;
  line_replace(want == 0 ? s_hist_saved : s_hist[want - 1]);
}


/* ---- global shortcuts ----------------------------------------------------
 *
 * Handled here, above every shell, because they have to work from inside an
 * app -- they are how you leave one. A shell that saw them first would hand
 * them to whatever had focus, and an editor would eat opt-3 as a character.
 *
 * Returns 1 if the key was one of these and has been dealt with. */
/* The shortcut list, over whatever is on screen. Drawn with the same panel
 * every app's ctrl-h uses, so there is one thing that looks like help. Any key
 * closes it, and the shell underneath repaints. */
static int s_opt_help;

/* opt-space from the console or the desktop takes the screen to show the
 * app search, same as fn-` does to reach the launcher -- this remembers
 * which shell it took it from, so cancelling (Escape, or backspace past
 * the start) can give it back rather than stranding the user in the bare
 * carousel. -1 is nothing pending: already in the launcher, or no search
 * is open. Forgotten, not acted on, if the search instead opens an app --
 * that app is what has the screen now, same as choosing one from the
 * carousel itself would leave it. */
static int s_search_from = -1;

static void show_opt_help(void) {
  s_opt_help = 1;
  help_paint("Shortcuts", "opt-1\tlauncher\nopt-2\tdesktop\nopt-3\tconsole\nopt-0\tbacklight to full\nopt-9\tbacklight down a step\nopt-8\tlouder\nopt-7\tquieter\nfn-o\tscreen off, black\nfn-c\tscreen off, dim clock\nfn-n\tnotifications\nopt-t\ttodo\nopt-s\tstocks\nopt-e\tedit\nopt-m\tmines\nopt-b\treconnect bluetooth\nopt-w\treconnect wifi\n",
             "fn-h\tthe keys of whatever is running\nany key\tclose this\n");
}

/* ---- what voice and the shells are allowed to do to each other ----------
 *
 * Three small functions handed to kernel/sys, which needs them and must not
 * include the desktop to get them. This file is the only one that knows how
 * the three shells relate, so this is where the knowledge stays. */

static void dispatch_key(uint8_t k);      /* defined with the loop, below */

static int ops_open_app(const char *name) {
  /* The launcher's runner, which is also what `run` uses: one way to start an
   * app, so voice cannot start one differently from the console. An app it
   * opens takes the screen through the launcher (ui_shell follows); a
   * command runs where it was asked from. */
  return launchui_run(name, NULL) != 0 ? -1 : 0;
}

static void ops_switch_shell(const char *which) {
  if (!strcmp(which, "desktop"))       go(UI_DESKTOP, 1);
  else if (!strcmp(which, "console"))  go(UI_NONE, 1);
  else                                 go(UI_LAUNCHER, 1);
}

/* The button on top: the console never claims it; a shell asks its app. */
static int sink_button(int event, const char *text) {
  switch (ui_shell()) {
  case UI_LAUNCHER: return launchui_button(event, text);
  case UI_DESKTOP:  return desktop_button(event, text);
  default:          return 0;
  }
}

static int sink_wants_text(void) {
  /* The console is always taking text; a shell running an app defers to the
   * app, which is the same question `; . , /` already asks. */
  switch (ui_shell()) {
  case UI_LAUNCHER: return launchui_wants_text();
  case UI_DESKTOP:  return desktop_wants_text();
  default:          return 1;
  }
}

/* The hotkey table is a text file on the card, /config/hotkeys.txt, so it
 * can be read and edited by hand and survives a flash that wipes NVS. No card
 * means no hotkeys, which is also what a fresh card has. */
#define HOTKEY_PATH CAPP_CONFIG "/hotkeys.txt"

static int hotkey_file_load(char *buf, int size) {
  int fd, n;
  if (!fs_mounted()) return 0;
  fd = fs_open(HOTKEY_PATH, FS_O_READ);
  if (fd < 0) return 0;
  n = fs_read(fd, buf, (size_t)(size - 1));
  fs_close(fd);
  if (n < 0) n = 0;
  buf[n] = 0;
  return n;
}

static void hotkey_file_save(const char *text) {
  int fd;
  if (!fs_mounted()) return;
  fs_mkdir(CAPP_CONFIG);
  fd = fs_open(HOTKEY_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return;
  fs_write(fd, text, strlen(text));
  fs_close(fd);
}

/* The volume from anywhere: the step, a panel that says where it is now
 * (taken down a second and a half after the last key), and a tick at the
 * new level, so it can be set by ear. */
static uint32_t s_vol_panel_until;
static void volume_step(int by) {
  speaker_set_volume(speaker_volume() + by);
  overlay_volume(speaker_volume());
  s_vol_panel_until = (uint32_t)(esp_timer_get_time() / 1000) + 1500;
  if (!s_vol_panel_until) s_vol_panel_until = 1;
  blip(BLIP_MOVE);
}

static void volume_panel_tick(void) {
  if (!s_vol_panel_until) return;
  if ((int32_t)((uint32_t)(esp_timer_get_time() / 1000) - s_vol_panel_until) < 0) return;
  s_vol_panel_until = 0;
  if (overlay_showing_volume()) overlay_close();
}

/* The loop's pause: 5 ms while someone is there; 40 once the screen sleeps,
 * when a keypress need only wake it -- an eighth of the polling. */
static int rest_ms(void) { return power_asleep() ? 40 : 5; }

/* A key at the lock screen -- the clock or black, one lock with two faces.
 * c shows the clock, o turns it black (fn-c and fn-o too); opt-backspace
 * unlocks, and so does Enter, Space, Escape or Delete pressed twice in a
 * row, within a second and a half. Anything else is a key brushed in a
 * pocket and does nothing. Returns 1 to unlock. */
#define LOCK_TWICE_MS 1500
static int lock_key(uint8_t k) {
  static uint8_t last;
  static int64_t last_us;
  int64_t now = esp_timer_get_time();
  int twice;
  if (k == KEY_QUIT) { last = 0; return 1; }
  if (k == 'c' || k == 'C' || k == KEY_FN_LETTER('c')) { power_clock_now(); last = 0; return 0; }
  if (k == 'o' || k == 'O' || k == KEY_FN_LETTER('o')) { power_off_now(); last = 0; return 0; }
  if (k == KEY_FN_LETTER('l')) { last = 0; return 0; }
  if (k == 0x7F) k = KEY_BACKSPACE;                 /* delete and backspace are one key here */
  if (k != KEY_ENTER && k != ' ' && k != KEY_ESC && k != KEY_BACKSPACE) { last = 0; return 0; }
  twice = k == last && now - last_us < (int64_t)LOCK_TWICE_MS * 1000;
  last = twice ? 0 : k;
  last_us = now;
  return twice;
}

static int global_key(uint8_t k) {
  /* The panel is above everything, so it gets the key first. */
  if (s_opt_help) {
    s_opt_help = 0;
    if (ui_shell() == UI_DESKTOP) desktop_repaint();
    else if (ui_shell() == UI_LAUNCHER) launchui_repaint();
    else { con_clear(); prompt(); }
    return 1;
  }

  switch (k) {
  case KEY_OPT_LETTER('h'):
    show_opt_help();
    return 1;
  case KEY_FN_LETTER('n'):                     /* the notification centre */
    notify_center_open();
    return 1;
  case KEY_OPT_DIGIT(1):
    go(UI_LAUNCHER, 1);
    return 1;
  case KEY_OPT_DIGIT(2):
    go(UI_DESKTOP, 1);
    return 1;
  case KEY_OPT_DIGIT(3):
    go(UI_NONE, 1);
    return 1;

  /* Brightness, from anywhere, without needing to see the screen.
   *
   * A dim setting is the one setting that can make the machine unusable: it
   * hides the menu you would use to undo it, and it survives a reboot because
   * it is in NVS. opt-0 is therefore not a convenience -- it is the way back,
   * and it works from any shell, over any app, with the panel dark. */
  case KEY_OPT_DIGIT(0):
    display_set_brightness(100);
    return 1;
  case KEY_OPT_DIGIT(9):
    display_set_brightness(display_brightness() - 25);
    return 1;
  /* The speaker, on the next two digits down: opt-8 louder, opt-7 quieter.
   * Ten percent a step, remembered, and it takes effect mid-playback. */
  case KEY_OPT_DIGIT(8):
  case KEY_VOL_UP:
    volume_step(+10);
    return 1;
  case KEY_OPT_DIGIT(7):
  case KEY_VOL_DOWN:
    volume_step(-10);
    return 1;

  /* Reconnect the radios. Both, because "get me back to where I was" is one
   * thought, and it blocks for seconds either way -- so it says what it is
   * doing on the console rather than freezing a shell silently. */
  case KEY_OPT_LETTER('b'): {
    int n;
    go(UI_NONE, 1);
    con_write("bluetooth: looking...\n");
    n = bthid_autoconnect(4);
    con_printf("  %d connected: %s\n", n, bthid_status(BTHID_MOUSE));
    prompt();
    return 1;
  }
  case KEY_OPT_LETTER('w'):
    go(UI_NONE, 1);
    con_write("wifi: joining the saved network...\n");
    wifi_connect_saved(20000);
    con_printf("  %s\n", wifi_status());
    prompt();
    return 1;

  /* Screen off, on request, from anywhere -- the same dark the idle timeout
   * reaches, so the same "any key wakes it" path brings it back. */
  case KEY_FN_LETTER('o'):
    power_off_now();
    return 1;
  /* The dim clock now: fn, not opt, because opt letters are the user's own
   * shortcuts and reserving one could take a binding away. */
  case KEY_FN_LETTER('c'):
    power_clock_now();
    return 1;
  /* Lock: the clock or black, whichever it was last. */
  case KEY_FN_LETTER('l'):
    power_lock_now();
    return 1;

  /* ctrl-opt-u: update everything now, the same code path `update all`
   * runs at the console -- so the chord works from wherever, without
   * finding the console first. */
  case KEY_UPDATE_ALL:
    go(UI_NONE, 1);
    cmd_update("all");
    prompt();
    return 1;

  /* opt-space: the app search, from wherever. Already in the launcher
   * this only opens the overlay (a fullscreen app underneath keeps
   * running); from the console or the desktop it takes the screen first,
   * the same way fn-` already does from the console. */
  case KEY_APP_SEARCH:
    if (ui_shell() != UI_LAUNCHER) {
      s_search_from = (int)ui_shell();
      go(UI_LAUNCHER, 1);
    }
    launchui_open_search();
    return 1;

  default:
    /* Any other opt letter is a shortcut if the user bound one (`hotkey` in
     * the console, or k in the launcher). Unbound, it is swallowed rather
     * than passed on: a shortcut that does nothing must not type a letter
     * into whatever has focus. */
    if (k >= KEY_OPT_LETTER('a') && k <= KEY_OPT_LETTER('z')) {
      const char *name = hotkey_get((char)('a' + (k - KEY_OPT_LETTER('a'))));
      if (name) shell_exec(name, NULL);
      return 1;
    }
    return KEY_IS_OPT(k);
  }
}

/* A build's date and time as one comparable number: the compiler's "Sep 11
 * 2026" and "18:04:27" from its app descriptor. Seconds resolution is more
 * than enough to tell two builds apart, and the SHA -- which is what the
 * updater compares -- cannot say which of two is newer. */
static int64_t build_stamp(const esp_app_desc_t *d) {
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = { d->date[0], d->date[1], d->date[2], 0 };
  const char *m = strstr(MONTHS, mon);
  int month = m ? (int)(m - MONTHS) / 3 + 1 : 0;
  int day = atoi(d->date + 4), year = atoi(d->date + 7);
  int hh = atoi(d->time), mm = atoi(d->time + 3), ss = atoi(d->time + 6);
  return ((((int64_t)year * 13 + month) * 32 + day) * 24 + hh) * 3600 +
         mm * 60 + ss;
}

/* Running from an OTA slot: is the image in factory a later build than us? */
static int factory_is_newer(const esp_partition_t *self) {
  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  esp_app_desc_t theirs;
  const esp_app_desc_t *ours = esp_app_get_description();
  (void)self;
  if (!factory || !ours) return 0;
  if (esp_ota_get_partition_description(factory, &theirs) != ESP_OK) return 0;
  if (!memcmp(theirs.app_elf_sha256, ours->app_elf_sha256, 32)) return 0;
  return build_stamp(&theirs) > build_stamp(ours);
}

/* The device held a Google login once (kernel/net/gauth.c, gone 2026-10-09):
 * a client secret and a refresh token in NVS, mirrored in plain text to
 * /config/google.txt on a card anyone can take out. The server holds the
 * login now, so a leftover copy of either is only a secret lying about. */
static void forget_google(void) {
  nvs_handle_t h;
  if (fs_remove(CAPP_CONFIG "/google.txt") == 0)
    applogf("google", "deleted the old /config/google.txt");
  /* Read-only first: opening a namespace for writing creates it. */
  if (nvs_open("cardosg", NVS_READONLY, &h) != ESP_OK) return;
  nvs_close(h);
  if (nvs_open("cardosg", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
}

/* The serial link's hooks (kernel/sys/serlink.h): a PC working on the
 * device over USB, whatever is on screen. */
static int ser_open(const char *name, const char *args) {
  return shell_exec(name, (args && *args) ? args : NULL) != 0 ? -1 : 0;
}

static void ser_state(char *out, size_t n) {
  UiShell sh = ui_shell();
  const AppDef *a = sh == UI_LAUNCHER ? launchui_running()
                  : sh == UI_DESKTOP ? desktop_focused_app() : NULL;
  snprintf(out, n, "shell=%s app=%s heap=%u low=%u up=%lus",
           sh == UI_LAUNCHER ? "launcher" : sh == UI_DESKTOP ? "desktop" : "console",
           a && a->name ? a->name : "-",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned long)(esp_timer_get_time() / 1000000));
}

/* Monotonic milliseconds for the scheduler. */
/* Hands the busy indicator a way to undo itself. There is no back buffer, so
 * the only way to remove the badge is to redraw what was under it.
 *
 * Only the shell that is actually up. Asking both draws the desktop over the
 * launcher and then the launcher back over that, which reads as the screen
 * flicking to the desktop and back every time a request finishes. The console
 * is not a painted shell and wants nothing. */
/* What has the keyboard, for the agent's `action` tool. */
static const AppDef *ops_running_app(void) {
  if (ui_shell() == UI_LAUNCHER) return launchui_running();
  if (ui_shell() == UI_DESKTOP) return desktop_focused_app();
  return NULL;
}

static void repaint_shells(void) {
  if (ui_shell() == UI_DESKTOP) desktop_repaint();
  else if (ui_shell() == UI_LAUNCHER) launchui_repaint();
}

/* Everything, the console included -- what a screenshot needs painted. */
static void repaint_all(void) {
  if (power_showing_clock()) { sleepclock_paint(); return; }   /* it has the panel */
  if (ui_shell() == UI_NONE) con_repaint();
  else repaint_shells();
  alarm_paint_over();          /* a ringing alarm stays on top */
  notify_paint_over();         /* and a notification's banner */
}

/* A screenshot asked for over the serial line, from tools/shots.py. Named by
 * count, because the byte that asks carries no name; the PC renames it. */
static void serial_shot(void) {
  static int n;
  char name[24];
  snprintf(name, sizeof name, "serial%d", ++n);
  /* Asked for over serial, so serial is where the answer goes: tools/shots.py
   * waits for a PNG, and "no shot arrived" on its own says nothing about
   * whether the card, the proxy or the post was the problem. */
  if (shot_take(name, repaint_all) != 0 || *shot_error())
    ESP_LOGW("shot", "%s: %s", name, shot_error());
}

/* The console's own key handling, lifted out of the loop so that anything
 * producing keys -- including a spoken sentence -- reaches the same code
 * rather than a second copy of it that drifts. */
static void console_key(uint8_t k) {
  if (ui_shell() == UI_NONE) con_cursor(0);
  if (k == KEY_ENTER) {
    s_line[s_len] = 0;
    con_putc('\n');
    hist_add(s_line);
    s_hist_at = 0;
    s_hist_saved[0] = 0;
    run_pipeline(s_line);
    s_len = 0;
    /* Only if the console still owns the screen. `desk` and `launch` paint a
     * whole shell from inside run_pipeline, and printing a prompt afterwards
     * drew a line of console over the top of it. */
    if (ui_shell() == UI_NONE) prompt();
  } else if (k == KEY_BACKSPACE) {
    if (s_len > 0) { s_len--; con_putc('\b'); }
  } else if (k == KEY_UP) {
    hist_walk(1);
  } else if (k == KEY_DOWN) {
    hist_walk(-1);
  } else if (k == KEY_TAB) {
    complete_line();
  } else if (k == KEY_ESC) {
    s_len = 0;
    con_putc('\n');
    prompt();
  } else if (k >= 0x20 && k < 0x7F && s_len < CARDOS_LINE_MAX) {
    s_line[s_len++] = (char)k;
    con_putc((char)k);
  }
}

/* The console's cursor: on after a key, then blinking every 500 ms. */
static int64_t s_last_blink;
static int     s_blink;

/* One key, delivered.
 *
 * Every source ends up here: the matrix, a Bluetooth keyboard, a character off
 * the serial line, a word that was spoken and a key the agent typed. That is
 * what lets voice work in apps that have never heard of it -- by the time a
 * transcript reaches an app, it is indistinguishable from typing.
 *
 * All of it, not only the shell's half. Whatever stands over the shells
 * answers first -- a ringing alarm, a ringing notification, the notification
 * centre, the global chords -- and whatever a shell hands on afterwards (the
 * launcher to the console or the desktop, the desktop to the console, the
 * console to the launcher) happens here as well. Keys from voice and the
 * agent used to come in through a second, shorter copy that skipped the
 * gating and every one of those transitions. The loop's own concerns -- the
 * lock screen, waking, repeats -- are about a physical keyboard and stay in
 * the loop. */
static void dispatch_key(uint8_t k) {
  if (!k) return;

  /* A ringing alarm answers every key: nothing underneath should open,
   * type or close because someone reached out to stop it. */
  if (alarm_ringing()) { alarm_key(k); return; }

  /* A ringing notification (a Timer finished while closed) is stopped by
   * any key, and the key goes no further. */
  if (notify_ringing()) { notify_dismiss(); return; }

  /* The notification centre (fn-n) has every key while it is open. */
  if (notify_center_active()) {
    uint8_t arrow = keyboard_arrow_for(k);
    notify_center_key(arrow ? arrow : k);
    return;
  }

  /* Before any shell sees it. */
  if (global_key(k)) return;

  switch (ui_shell()) {
  case UI_LAUNCHER: {
    int r = launchui_key(k);
    if (r == 1) {
      go(UI_NONE, 1);
    } else if (r == 2) {
      /* the launcher handed over: desktop_init has the screen */
    } else if (s_search_from >= 0 && !launchui_search_active()) {
      /* The search opt-space opened from outside the launcher just
       * closed. Enter on a hit leaves an app running here, which
       * stays; cancelling (Escape, or backspace past the start)
       * gives the screen back to what opt-space took it from. */
      int from = s_search_from;
      s_search_from = -1;
      if (!launchui_running() && from != UI_LAUNCHER)
        go((UiShell)from, from == UI_NONE);   /* the console redraws; the desktop is as left */
    }
    return;
  }
  case UI_DESKTOP:
    /* ESC left the desktop: hand the screen back to the console. */
    if (desktop_key(k)) go(UI_NONE, 1);
    return;
  default:
    /* The way out of the console is the way out of everything else: fn-` or
     * opt-backspace goes to the launcher, as the same key there comes back
     * here. Before this it reached the line editor and did nothing. */
    if (k == KEY_QUIT) {
      con_cursor(0);
      go(UI_LAUNCHER, 1);
      return;
    }
    console_key(k);
    if (ui_shell() == UI_NONE) {
      s_last_blink = esp_timer_get_time();
      s_blink = 1;
      con_cursor(1);
    }
    return;
  }
}

void app_main(void) {
  size_t heap_at_boot;
  const char *nvs_erased = NULL;    /* why, if this boot wiped the settings */

  /* The crypto library's one-time setup, now, while the heap is one piece.
   * Left to the first HTTPS request, its few hundred bytes of lifelong state
   * landed in the middle of the free region: the largest executable block
   * went from 36 KB to 13.3 KB and stayed there, and Calendar's 13.9 KB of
   * code could not load again until a reboot (measured 2026-09-23). */
  psa_crypto_init();

  /* Tell the bootloader this image is good, so an OTA-updated CardOS does not
   * roll itself back on the next reset -- see the app launcher spec. Only
   * meaningful from an OTA slot: running from factory it just logs an error,
   * which is noise on every boot. */
  {
    const esp_partition_t *self = esp_ota_get_running_partition();
    if (self && self->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        self->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
      esp_ota_mark_app_valid_cancel_rollback();
      if (factory_is_newer(self)) {
        /* A USB flash writes factory, but otadata still points here, so
         * without this the build just flashed would never run. */
        const esp_partition_t *factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (factory && esp_ota_set_boot_partition(factory) == ESP_OK)
          esp_restart();
      }
    }
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
  /* Before the keyboard: on the ADV the keyboard is an I2C chip, and on
   * the original the probe must be done with pins 8 and 9 before the matrix
   * takes them. See kernel/drv/board.h. */
  board_detect();
  /* The version is ESP-IDF's `git describe` of the tree it was built from
   * (v0.9.0, or v0.9.0-3-gabc1234 between tags; -dirty with uncommitted
   * changes): the tags are the release numbers. */
  con_printf("CardOS %s (%s) on %s\n", esp_app_get_description()->version,
             update_flavor(), board_name());
  con_set_color(COLOR_GREY);
  con_write("kernel core\n\n");
  con_set_color(COLOR_GREEN);

  heap_at_boot = esp_get_free_heap_size();

  if (keyboard_init() != 0) {
    con_set_color(COLOR_RED);
    con_write("keyboard init failed\n");
    con_set_color(COLOR_GREEN);
  }

  /* NVS, before anything reads a setting.
   *
   * It used to be initialised inside wifi_start and the Bluetooth radio, which
   * meant every nvs_open before a radio started failed silently -- and a
   * silent failure in a settings store looks exactly like a setting that does
   * not change. "BT at boot" could not be turned on, and the remembered shell
   * always came back as the default, because both were reading from a
   * partition nobody had opened. */
  {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      /* Loudly, and later to the card: this is every credential going at
       * once, and until it was written down it looked like Google forgetting
       * the device for no reason, over and over. */
      con_set_color(COLOR_RED);
      con_printf("nvs %s: erasing it. wifi and settings are gone;\n"
                 "whatever /config mirrors comes back below.\n",
                 err == ESP_ERR_NVS_NO_FREE_PAGES ? "is full" : "is another version");
      con_set_color(COLOR_GREEN);
      nvs_erased = err == ESP_ERR_NVS_NO_FREE_PAGES ? "no free pages" : "version changed";
      nvs_flash_erase();
      err = nvs_flash_init();
    }
    if (err != ESP_OK) con_write("nvs unavailable: settings will not stick\n");
  }

  /* Safe mode: hold the escape key while the machine starts.
   *
   * Every setting below this point comes out of NVS, and a setting can make
   * the machine unusable by hiding the thing that would undo it -- a
   * backlight at a level the panel does not show, a shell that opens an app
   * that crashes, a radio that eats the heap something else needs. Each of
   * those survives a reboot, because that is what persistence means, and the
   * usual answer is a cable and a full erase.
   *
   * So: one key, held at power-on, and none of it is applied. The panel comes
   * up at full brightness, no radio starts, and the console gets the screen --
   * from which `defaults` clears the lot. Nothing is written here; a safe boot
   * changes nothing on its own, so it can be tried without committing to
   * losing anything. */
  /* 60 ms, not the 400 it was: a key held through power-on is down on the
   * first scan, so the rest was only ever spent waiting on every boot. Four
   * scans (0, 20, 40, 60 ms) leave room for one that misreads. */
  /* d held through power-on: the card as a USB drive instead of CardOS.
   * Before the card is mounted -- the PC owns its filesystem from here. */
  if (keyboard_held('d', 60)) usbdisk_run();

  s_safe_mode = keyboard_held(KEY_ESC, 60);
  if (s_safe_mode) {
    display_set_brightness_now(100);
    con_set_color(COLOR_AMBER);
    con_write("\nSAFE MODE -- settings not applied\n");
    con_set_color(COLOR_GREY);
    con_write("backlight full, no radios, console shell.\n"
              "`defaults` clears saved settings, `reboot` returns.\n\n");
    con_set_color(COLOR_GREEN);
  } else {
    display_load_brightness();   /* full until now; the setting needs NVS */
  }

  env_init();

  /* A missing card is a normal condition, not a boot failure: CardOS runs
   * without one, just without apps. Say which, rather than leaving
   * the user to guess why `ls` is empty. */
  if (fs_mount() == 0) {
    uint64_t total = 0, freeb = 0;
    static const HotkeyStore FILE_STORE = { hotkey_file_load, hotkey_file_save };
    int moved;
    capprun_watch_apps();        /* before anything below can touch /apps */
    moved = fs_migrate_layout();   /* an old card into the new folders; before ensure, so the trees can rename */
    fs_ensure_layout();
    if (moved) con_printf("card layout: moved %d into /apps /sys /config /cache /home /var\n", moved);
    hotkeys_init(&FILE_STORE);   /* after the mount: the table is on the card */
    /* Credentials NVS lost -- a full-table flash wipes it -- come back from
     * their /config mirrors, so a reflash never means retyping a password
     * or repeating the Google consent dance. */
    if (nvs_erased) applogf("nvs", "erased at boot: %s", nvs_erased);
    if (wifi_restore_from_card())  con_write("wifi: network restored from /config/wifi.txt\n");
    forget_google();
    if (env_restore_from_card())   con_write("env: variables restored from /config/env.txt\n");
    /* Everything else small (prefs.h). Brightness was read before the card
     * was up, so a restored one is applied now; the rest are read later. */
    if (prefs_restore_from_card()) {
      con_write("settings: restored from /config/settings.txt\n");
      if (!s_safe_mode) display_load_brightness();
    }
    fs_space(&total, &freeb);
    con_printf("sd %u MB, %u MB free\n",
               (unsigned)(total / (1024 * 1024)),
               (unsigned)(freeb / (1024 * 1024)));
  } else {
    hotkeys_init(NULL);          /* nothing to bind to, and nowhere to keep it */
    con_set_color(COLOR_GREY);
    con_write("no sd card: no apps\n");
    con_set_color(COLOR_GREEN);
  }

  /* Success criterion 6: the free heap is reported and understood. */
  con_printf("heap %u KB free at boot\n", (unsigned)(heap_at_boot / 1024));
  con_write("type help\n\n");

  /* Came back from a firmware the desktop launched: return there, rather than
   * to a console nobody asked for. */
  /* A bonded mouse or keyboard is usually still advertising, so one short look
   * before the shell comes up saves reaching for Settings. Off unless asked
   * for: the radio costs ~67 KB for the whole uptime. */
  if (!s_safe_mode && bthid_autostart()) {
    con_write("bluetooth: looking for paired devices...\n");
    con_printf("  %d connected\n", bthid_autoconnect(3));
  }

  /* Back into whichever shell was last used. A device that always boots to the
   * same one regardless of what you were doing is a device you have to re-enter
   * a command on every time -- and it makes the firmware round trip land where
   * you left from, which is the behaviour the rollback was built for. */
  battery_init();
  pins_init();
  busy_init();
  httpq_init();
  /* Whichever shell is up paints over the badge when a request finishes.
   * Both are asked: the one that is not running does nothing with it. */
  busy_on_done(repaint_shells);
  agent_init();
  clock_init();
  bg_init();
  blip_init();
  /* A command that opens its app (CAPP_CMD_OPEN) opens it the way `run`
   * does: through the launcher. */
  capprun_set_opener(launchui_run);
  capprun_set_shell(shell_remote);
  {
    static const SerlinkHooks hooks = { shell_remote, ser_open, ser_state, repaint_all };
    serlink_init(&hooks);
  }
  alarm_set_repaint(repaint_all);    /* what a ringing alarm's panel covered */
  notify_init(repaint_all);          /* and what a banner covered */
  power_set_painters(sleepclock_paint, repaint_all);   /* the sleep clock, and waking */

  /* The icon scan, whichever shell comes up. It is also what writes a new
   * firmware's apps to the card (seed_capps) and the command catalog, and it
   * used to run only when the launcher did -- so a device that booted into
   * the console kept its old apps through every flash, and `do` found none
   * of the new commands (2026-09-23).
   *
   * Only for the console, though: launchui_init and desktop_init each scan
   * as they come up, so doing it here as well scanned the card twice on
   * every boot that went to a painted shell -- which is nearly all of them. */
  if (fs_mounted() && (s_safe_mode || ui_saved_shell() == UI_NONE)) icons_reload();
  /* Asked for rather than waited on: if WiFi is already up this is answered
   * in a second, and if it is not the job simply finds nothing. Either way
   * the shell starts now. */
  bg_submit(BG_TIME_SYNC);

  {
    static const ShellOps OPS = { ops_open_app, ops_switch_shell, dispatch_key, ops_running_app };
    static const InputSink SINK = { sink_wants_text, dispatch_key, sink_button };
    shell_set_ops(&OPS);
    input_set_sink(&SINK);
  }

  /* Safe mode always lands at the console: it is the one shell that cannot be
   * hidden by a setting, and the one with `defaults` in it. */
  if (s_safe_mode) go(UI_NONE, 0);
  else switch (ui_saved_shell()) {
  case UI_DESKTOP:  go(UI_DESKTOP, 1); break;
  case UI_NONE:     go(UI_NONE, 0); break;
  default:          go(UI_LAUNCHER, 1); break;
  }
  if (!s_safe_mode) blip(BLIP_BOOT);

  for (;;) {
    uint8_t k = keyboard_poll();

    /* The button on top, polled beside the keyboard so it works in every
     * shell and over every app. Press and hold to talk; see kernel/sys/voice.c
     * for why the whole cycle happens inside this call. */
    agent_tick();
    if (voice_tick()) {
      repaint_shells();
    }

    /* A character arriving on the serial console counts as a keypress, so the
     * shell can be driven from a PC as well as from the keyboard. */
    /* A Bluetooth keyboard is the same keyboard as the built-in one from here
     * on: the decoder emits the same byte alphabet, so nothing downstream
     * needs to know which one a key came from. */
    /* Whether this key is a held key repeating, from whichever keyboard it
     * came; the serial line never repeats. Recorded once here so every app
     * can ask (api->key_repeat) and capprun can drop it for those that
     * asked not to be told. */
    {
      int repeat = k ? keyboard_last_repeat() : 0;
      if (!k) {
        bthid_tick((uint32_t)(esp_timer_get_time() / 1000));
        if (bthid_poll_key(&k)) repeat = bthid_last_repeat();
      }
      input_set_repeat(k ? repeat : 0);
      /* A click per key from either keyboard, not per repeat. A key that
       * also does something audible (the launcher's move) is heard as that
       * instead: blip.c keeps only the newest of a burst. Not while asleep
       * (off or the dim clock): a key pressed there is either one of the
       * three that wakes it -- which is its own event, not a click -- or
       * one that does nothing at all, and a click for a key that did
       * nothing is the thing this is fixing. */
      if (k && !repeat && !power_asleep()) blip(BLIP_KEY);
    }

    int from_serial = 0;
    if (!k) {
      int link = 0;
      int sc = serlink_key(&link);   /* a frame from tools/cardctl.py is handled here */
      if (link) bg_note_activity();
      /* Not a key: 0xFF is outside the alphabet, and it means "screenshot".
       * Taken here, before any app sees it, so a shot can be of anything. */
      if (sc == 0xFF) { serial_shot(); sc = 0; }
      if (sc == '\r' || sc == '\n') k = KEY_ENTER;
      else if (sc == 0x7F || sc == 0x08) k = KEY_BACKSPACE;
      else if (sc == 0x1B) k = KEY_ESC;
      else if (sc > 0) k = (uint8_t)sc;
      from_serial = k != 0;
    }

    /* Anything the user did resets the idle clock -- which is what decides
     * whether the machine is allowed to go and do slow things, and how hard
     * this loop polls. */
    if (k) {
      bg_note_activity();
      /* Wake first. A key pressed at a dark screen means "come back", and
       * acting on it as well would open an app nobody asked for. */
      if (power_dimmed()) {
        /* Black and the clock are the lock screen, not a merely dim one:
         * lock_key decides (c, o, a key twice, opt-backspace), and a key
         * brushed by accident must not light a screen meant to stay dark.
         * A held key's repeats are not "twice". A dim screen (not yet
         * locked) still wakes on any key, as before. */
        if (power_asleep() && !from_serial) {
          if (!input_is_repeat() && lock_key(k)) power_wake();
          k = 0;
        } else {
          power_wake();
          /* Not from the serial port: that is a program typing a command, not
           * someone at a dark screen, and swallowing the first byte cut the
           * first letter off every command sent after a pause (2026-09-30). */
          if (!from_serial) k = 0;
        }
      }
    }
    power_tick();
    volume_panel_tick();
    alarm_tick();           /* reads /config/alarms.txt when the minute changes */
    notify_tick((uint32_t)(esp_timer_get_time() / 1000));   /* banners, Chat, Calendar */
    link_tick();            /* ESP-NOW frames in, resends out (kernel/net/link.c) */
    clock_persist_tick();   /* writes at most once every ten minutes */

    /* What the background task finished while nobody was waiting. One per
     * pass, and only the shell ever draws it. */
    {
      const char *done = bg_take_result();
      if (done) {
        if (ui_shell() == UI_NONE) { con_printf("%s\n", done); prompt(); }
        else if (ui_shell() == UI_LAUNCHER) launchui_note(done);
      }
    }

    /* A time zone the server found (kernel/sys/tzlookup.c), applied here
     * because this is the task that reads env and the clock. Saved like
     * `set TZ=...`, so it is asked for once, not every boot. */
    {
      char zone[48], line[80];
      const char *rule = tzlookup_take(zone, sizeof zone);
      if (rule) {
        env_set("TZ", rule);
        clock_apply_zone();
        snprintf(line, sizeof line, "timezone: %s", zone[0] ? zone : rule);
        applogf("tz", "%s (%s)", zone, rule);
        if (ui_shell() == UI_NONE) { con_printf("%s\n", line); prompt(); }
        else if (ui_shell() == UI_LAUNCHER) launchui_note(line);
      }
    }

    dispatch_key(k);

    /* The notification centre is drawn over the shells, and they wait while
     * it is open: an animating app would draw over it. */
    if (!notify_center_active()) {
      if (ui_shell() == UI_LAUNCHER) {
        MouseReport mr;
        int got = 0;
        while (bthid_poll_mouse(&mr)) { launchui_mouse_apply(&mr); got = 1; }
        if (got) launchui_mouse_done();
        launchui_tick((uint32_t)(esp_timer_get_time() / 1000));
      } else if (ui_shell() == UI_DESKTOP) {
        MouseReport mr;
        int got = 0;
        /* Drain whatever the radio queued. It arrives on the Bluetooth task,
         * which must not touch the display, so this is where it turns into
         * pointer movement. */
        while (bthid_poll_mouse(&mr)) { desktop_mouse_apply(&mr); got = 1; }
        if (got) desktop_mouse_done();   /* one repaint for the whole burst */
        desktop_tick((uint32_t)(esp_timer_get_time() / 1000));
        if (desktop_take_leave()) go(UI_NONE, 1);
      } else {
        int64_t now = esp_timer_get_time();
        if (now - s_last_blink > 500000) {   /* 500 ms */
          s_last_blink = now;
          s_blink = !s_blink;
          con_cursor(s_blink);
        }
      }
    }

    /* How hard to poll.
     *
     * 5 ms while something is happening: the pointer has to keep up with the
     * hand. But outside the moment somebody is pressing a key this screen is
     * static -- the clock ticks once a second and nothing else moves -- and
     * two hundred keyboard scans a second to discover that nothing has
     * changed is work for its own sake.
     *
     * After two seconds of quiet the poll drops to 25 ms. The cost is that
     * the first keypress after a pause can be noticed 25 ms late, which is
     * under what anyone perceives, and the very next pass is back to 5 ms
     * because that keypress reset the clock. Radios and the background task
     * get the machine in between. */
    {
      uint32_t quiet = bg_idle_ms();
      int ms = 10;
      if (quiet > 2000) ms = 25;
      if (power_asleep()) ms = 40;
      if (ui_shell() != UI_NONE || notify_center_active()) ms = rest_ms();
      vTaskDelay(pdMS_TO_TICKS(ms));
    }
  }
}

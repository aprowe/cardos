/* CardOS entry point.
 *
 * MVP: boot, bring up the display and keyboard, and give a console you can
 * type into. No scheduler and no filesystem yet, so this is a single loop
 * rather than a task -- the shell proper arrives with those.
 */

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

#include "kernel/console/console.h"
#include "kernel/drv/display.h"
#include "kernel/drv/keyboard.h"
#include "kernel/mem/mem.h"
#include "kernel/task/sched.h"
#include "kernel/fs/fs.h"
#include "shellcmd.h"
#include "kernel/fs/path.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/pins.h"
#include "kernel/ui/launchui.h"
#include "kernel/app/capprun.h"
#include "kernel/app/capp.h"
#include "kernel/net/wifi.h"
#include "kernel/net/gauth.h"
#include "kernel/ui/shell.h"
#include "kernel/sys/env.h"
#include "kernel/sys/sio.h"
#include "kernel/ui/help.h"
#include "kernel/sys/applog.h"
#include "kernel/sys/input.h"
#include "kernel/sys/voice.h"
#include "kernel/sys/bg.h"
#include "kernel/sys/clock.h"
#include "kernel/sys/busy.h"
#include "kernel/sys/shot.h"
#include "kernel/sys/agent.h"
#include "kernel/net/httpq.h"
#include "kernel/sys/power.h"
#include "kernel/drv/battery.h"
#include "kernel/sys/hotkeys.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "kernel/drv/bthid.h"
#include "kernel/drv/speaker.h"

/* Long enough for the lines a tool sends rather than the ones a person types.
 * At 63 a Google client ID -- 72 characters on its own, 81 with the command in
 * front of it -- was silently cut in half, and the only symptom was Google
 * answering "invalid_client". A refresh token is longer still. */
#define CARDOS_LINE_MAX 200

/* The handle heap. Carved once from the IDF heap at boot; everything CardOS
 * allocates afterwards comes through kmem_alloc. Deliberately not "whatever is
 * left" -- a fixed, stated size is what makes the numbers in `mem` mean
 * something, and CLAUDE.md is emphatic that memory here gets measured rather
 * than assumed. */
/* The memory manager's arena, reserved at boot.
 *
 * 128 KB when the radios did not exist; 48 KB once they did, because with
 * 128 the two together left 1156 bytes free and the WiFi driver failed buffer
 * allocations in a loop ("wifi:m f null").
 *
 * 16 KB now, and for a blunter reason: measured on the device with Bluetooth
 * up, 79588 bytes of heap were free, WiFi wanted 49792 of them and a TLS
 * handshake about 34000 more -- so Todo and Stocks could not reach the network
 * at all while a mouse was connected. The arena was holding 48 KB for a
 * subsystem that, grepped for, has no caller anywhere outside kernel/mem: the
 * loader allocates app images from the IDF heap, and so does everything else.
 * Reserving a third of the free memory for nothing was the whole shortage.
 *
 * It stays rather than going to zero because the swap allocator and handle
 * table are a real part of the design and 16 KB keeps them exercisable. If
 * something ever does allocate from here in earnest, this number is the one to
 * raise -- and `mem` is where to see that it needs raising.
 *
 * RESERVED ON FIRST USE, NOT AT BOOT (2026-09-18). The size was never the
 * whole story: a 16 KB block taken early sits in the middle of the heap and
 * splits it, and what a TLS handshake needs is not 34 KB of total free space
 * but one contiguous run of about 17 KB. With an app loaded the largest run
 * was 16384 bytes against that threshold, so Calendar's sync was refused --
 * intermittently, because it depended on what else had been loaded. Since
 * nothing outside kernel/mem calls kmem_alloc (grep says so, and a test
 * asserts it), reserving the block up front bought a split heap and nothing
 * else. kmem_ensure() below makes it appear the moment something actually
 * allocates, which is also the moment `mem` starts reporting it. */
#define CARDOS_HEAP_BYTES (16 * 1024)

static uint8_t *s_heap;

/* The handle arena, brought into existence by the first thing that wants it.
 * Nothing does today, which is exactly why it must not be taken at boot: it
 * would be 16 KB sitting in the middle of the heap, splitting the contiguous
 * run a TLS handshake needs. Every would-be caller of kmem_alloc calls this
 * first; if that ever becomes more than a handful of places, the call belongs
 * inside kmem_alloc rather than in front of it. */
int kmem_ensure(void) {
  if (s_heap) return 0;
  s_heap = heap_caps_malloc(CARDOS_HEAP_BYTES,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_heap) return -1;
  kmem_init(s_heap, CARDOS_HEAP_BYTES);
  return 0;
}
static char     s_line[CARDOS_LINE_MAX + 1];
static int      s_len;
/* Three shells over the same kernel. The launcher is the one meant for daily
 * use, the desktop is the demonstration that windows work, and the console is
 * for development. */
typedef enum { MODE_CONSOLE = 0, MODE_DESKTOP, MODE_LAUNCHER } Mode;
static Mode     s_mode;

/* Booted with escape held: no saved setting has been applied, no radio has
 * been started, and the console has the screen. See app_main. */
static int      s_safe_mode;

static void prompt(void) {
  con_set_color(COLOR_AMBER);
  con_printf("%s> ", shell_cwd());
  con_set_color(COLOR_GREEN);
}

static void cmd_mem(void) {
  con_printf("heap free        %6u B  (largest block %u)\n",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  con_printf("exec free        %6u B\n",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC));
  con_printf("low water        %6u B\n",
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  /* Reported as what it is. "reserved at boot" was true and was also the
   * bug: the reader had no way to see that the reservation was the thing
   * standing between a TLS handshake and a contiguous block. */
  if (s_heap)
    con_printf("handle arena     %6u B  in use\n", (unsigned)CARDOS_HEAP_BYTES);
  else
    con_printf("handle arena          0 B  reserved on first use (%u KB)\n",
               (unsigned)(CARDOS_HEAP_BYTES / 1024));
  con_printf("bluetooth        %6u B\n", (unsigned)bthid_heap_cost());
  con_printf("wifi             %6u B\n", (unsigned)wifi_heap_cost());
  /* The settings store, because when it fills up the next boot erases it
   * and every credential with it -- which looks like Google forgetting you
   * for no reason. Entries are 32 bytes; a page holds 126 of them. */
  {
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK)
      con_printf("nvs              %6u of %u entries used, %u free\n",
                 (unsigned)st.used_entries, (unsigned)st.total_entries,
                 (unsigned)st.free_entries);
  }
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
  con_write("         print [scan|use N|test|FILE] (bluetooth thermal printer)\n");
  con_write("screens  launch (carousel), desk (windows), escape returns\n");
  con_write("boot     apps, boot NAME, boot! NAME, bootinfo\n");
  con_write("system   mem ps taskcost flip clear reboot echo\n");
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
static void run_builtin(const char *line, char *arg) {
  if (!strcmp(line, "help"))        cmd_help();
  else if (!strcmp(line, "log"))    cmd_log(arg);
  else if (!strcmp(line, "mem"))    cmd_mem();
  /* The verbs the running app offers, and a way to run one.
   *
   * This is the point of the `id` field in CappAction: the same table that
   * draws the menus and the help panel is a list of things that can be asked
   * for by name. kernel/sys/rpc.c already argues it for voice -- an LLM
   * choosing between a few named verbs is useful, one handed a string to run
   * is not -- and this is that list, per app, written once by the app. */
  else if (!strncmp(line, "do", 2) && (line[2] == 0 || line[2] == ' ')) {
    const AppDef *a = launchui_running();
    const char *what = line[2] ? line + 3 : NULL;
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
  else if (!strcmp(line, "ps"))     cmd_ps();
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
  else if (!strcmp(line, "desk")) {
    desktop_init();
    s_mode = MODE_DESKTOP;
  }
  else if (!strcmp(line, "launch") || !strcmp(line, "gui")) {
    launchui_init();
    s_mode = MODE_LAUNCHER;
  }
  else if (!strcmp(line, "wifi")) cmd_wifi(arg);
  else if (!strcmp(line, "get"))  cmd_get(arg);
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
  else if (!strcmp(line, "google")) cmd_google(arg);
  else if (!strcmp(line, "run")) {
    /* cmd_run starts it; the mode has to change here, where the loop is. */
    cmd_run(arg);
    if (arg && *arg && ui_shell() == UI_LAUNCHER) s_mode = MODE_LAUNCHER;
  }
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
    if (shell_exec(line, (arg && *arg) ? arg : NULL) == 0) {
      if (ui_shell() == UI_LAUNCHER) s_mode = MODE_LAUNCHER;
    } else {
      con_printf("unknown command: %s\n", line);
    }
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
  "echo", "flip", "get", "gui", "help", "launch", "log", "ls", "mem", "mkdir",
  "battery", "defaults", "listen", "mouse", "ps", "pwd", "reboot", "rm",
  "run", "time",
  "safe",
  "print", "share", "shot", "taskcost", "update", "volume", "wifi",
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
static void enter_console(void) {
  s_mode = MODE_CONSOLE;
  ui_set_shell(UI_NONE);
  con_clear();
  prompt();
}

/* The shortcut list, over whatever is on screen. Drawn with the same panel
 * every app's ctrl-h uses, so there is one thing that looks like help. Any key
 * closes it, and the shell underneath repaints. */
static int s_opt_help;

static void show_opt_help(void) {
  s_opt_help = 1;
  help_paint("Shortcuts", "opt-1\tlauncher\nopt-2\tdesktop\nopt-3\tconsole\nopt-0\tbacklight to full\nopt-9\tbacklight down a step\nopt-8\tlouder\nopt-7\tquieter\nopt-t\ttodo\nopt-s\tstocks\nopt-e\tedit\nopt-m\tmines\nopt-b\treconnect bluetooth\nopt-w\treconnect wifi\n",
             "fn-h\tthe keys of whatever is running\nany key\tclose this\n");
}

/* ---- what voice and the shells are allowed to do to each other ----------
 *
 * Three small functions handed to kernel/sys, which needs them and must not
 * include the desktop to get them. This file is the only one that knows how
 * the three shells relate, so this is where the knowledge stays. */

static void feed_key(uint8_t k);          /* defined with the loop, below */

static int ops_open_app(const char *name) {
  /* The launcher's runner, which is also what `run` uses: one way to start an
   * app, so voice cannot start one differently from the console. */
  if (launchui_run(name, NULL) != 0) return -1;
  if (s_mode != MODE_LAUNCHER) { launchui_init(); s_mode = MODE_LAUNCHER; }
  return 0;
}

static void ops_switch_shell(const char *which) {
  if (!strcmp(which, "desktop"))       { desktop_init(); s_mode = MODE_DESKTOP; }
  else if (!strcmp(which, "console"))  { enter_console(); }
  else                                 { launchui_init(); s_mode = MODE_LAUNCHER; }
}

static int sink_wants_text(void) {
  /* The console is always taking text; a shell running an app defers to the
   * app, which is the same question `; . , /` already asks. */
  if (s_mode == MODE_CONSOLE) return 1;
  if (s_mode == MODE_LAUNCHER) return launchui_wants_text();
  return desktop_wants_text();
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

static int global_key(uint8_t k) {
  /* The panel is above everything, so it gets the key first. */
  if (s_opt_help) {
    s_opt_help = 0;
    if (s_mode == MODE_DESKTOP) desktop_repaint();
    else if (s_mode == MODE_LAUNCHER) launchui_repaint();
    else { con_clear(); prompt(); }
    return 1;
  }

  switch (k) {
  case KEY_OPT_LETTER('h'):
    show_opt_help();
    return 1;
  case KEY_OPT_DIGIT(1):
    launchui_init();
    s_mode = MODE_LAUNCHER;
    return 1;
  case KEY_OPT_DIGIT(2):
    desktop_init();
    s_mode = MODE_DESKTOP;
    return 1;
  case KEY_OPT_DIGIT(3):
    enter_console();
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
    speaker_set_volume(speaker_volume() + 10);
    return 1;
  case KEY_OPT_DIGIT(7):
    speaker_set_volume(speaker_volume() - 10);
    return 1;

  /* Reconnect the radios. Both, because "get me back to where I was" is one
   * thought, and it blocks for seconds either way -- so it says what it is
   * doing on the console rather than freezing a shell silently. */
  case KEY_OPT_LETTER('b'): {
    int n;
    enter_console();
    con_write("bluetooth: looking...\n");
    n = bthid_autoconnect(4);
    con_printf("  %d connected: %s\n", n, bthid_status(BTHID_MOUSE));
    prompt();
    return 1;
  }
  case KEY_OPT_LETTER('w'):
    enter_console();
    con_write("wifi: joining the saved network...\n");
    wifi_connect_saved(20000);
    con_printf("  %s\n", wifi_status());
    prompt();
    return 1;

  default:
    /* Any other opt letter is a shortcut if the user bound one (`hotkey` in
     * the console, or k in the launcher). Unbound, it is swallowed rather
     * than passed on: a shortcut that does nothing must not type a letter
     * into whatever has focus. */
    if (k >= KEY_OPT_LETTER('a') && k <= KEY_OPT_LETTER('z')) {
      const char *name = hotkey_get((char)('a' + (k - KEY_OPT_LETTER('a'))));
      if (name && shell_exec(name, NULL) == 0 && ui_shell() == UI_LAUNCHER)
        s_mode = MODE_LAUNCHER;
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
  if (s_mode == MODE_LAUNCHER) return launchui_running();
  if (s_mode == MODE_DESKTOP) return desktop_focused_app();
  return NULL;
}

static void repaint_shells(void) {
  if (s_mode == MODE_DESKTOP) desktop_repaint();
  else if (s_mode == MODE_LAUNCHER) launchui_repaint();
}

/* Everything, the console included -- what a screenshot needs painted. */
static void repaint_all(void) {
  if (s_mode == MODE_CONSOLE) con_repaint();
  else repaint_shells();
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

static uint32_t clock_ms(void *ctx) {
  (void)ctx;
  return (uint32_t)(esp_timer_get_time() / 1000);
}

/* The console's own key handling, lifted out of the loop so that anything
 * producing keys -- including a spoken sentence -- reaches the same code
 * rather than a second copy of it that drifts. */
static void console_key(uint8_t k) {
  if (s_mode == MODE_CONSOLE) con_cursor(0);
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
    if (s_mode == MODE_CONSOLE) prompt();
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

/* One key, delivered to whichever shell has the screen.
 *
 * Every source ends up here: the matrix, a Bluetooth keyboard, a character off
 * the serial line, and a word that was spoken. That is what lets voice work in
 * apps that have never heard of it -- by the time a transcript reaches an app,
 * it is indistinguishable from typing. */
static void feed_key(uint8_t k) {
  if (!k) return;
  if (global_key(k)) return;

  if (s_mode == MODE_LAUNCHER)      { launchui_key(k); return; }
  if (s_mode == MODE_DESKTOP)       { desktop_key(k); return; }
  console_key(k);
}

void app_main(void) {
  int64_t last_blink = 0;
  int blink = 0;
  size_t heap_at_boot;
  const char *nvs_erased = NULL;    /* why, if this boot wiped the settings */

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

  /* The handle arena is not taken here any more. See CARDOS_HEAP_BYTES. */

  /* The scheduler's policy half runs now; the Xtensa context switch does not
   * exist yet, so this loop *is* the shell task rather than being switched to
   * it. `ps` therefore shows one task. spawn/kill arrive with the switch. */
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
      con_printf("nvs %s: erasing it. wifi, google and settings are gone;\n"
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
  s_safe_mode = keyboard_held(KEY_ESC, 400);
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
  sched_init(clock_ms, NULL);
  sched_create("shell");
  sched_next();                  /* mark it running, so it owns its locks */

  /* A missing card is a normal condition, not a boot failure: CardOS runs
   * without one, just without apps or swap. Say which, rather than leaving
   * the user to guess why `ls` is empty. */
  if (fs_mount() == 0) {
    uint64_t total = 0, freeb = 0;
    static const HotkeyStore FILE_STORE = { hotkey_file_load, hotkey_file_save };
    int moved = fs_migrate_layout();   /* an old card into the new folders; before ensure, so the trees can rename */
    fs_ensure_layout();
    if (moved) con_printf("card layout: moved %d into /apps /sys /config /cache /home /var\n", moved);
    hotkeys_init(&FILE_STORE);   /* after the mount: the table is on the card */
    /* Credentials NVS lost -- a full-table flash wipes it -- come back from
     * their /config mirrors, so a reflash never means retyping a password
     * or repeating the Google consent dance. */
    if (nvs_erased) applogf("nvs", "erased at boot: %s", nvs_erased);
    if (wifi_restore_from_card())  con_write("wifi: network restored from /config/wifi.txt\n");
    if (gauth_restore_from_card()) con_write("google: credentials restored from /config/google.txt\n");
    if (env_restore_from_card())   con_write("env: variables restored from /config/env.txt\n");
    fs_space(&total, &freeb);
    con_printf("sd %u MB, %u MB free\n",
               (unsigned)(total / (1024 * 1024)),
               (unsigned)(freeb / (1024 * 1024)));
  } else {
    hotkeys_init(NULL);          /* nothing to bind to, and nowhere to keep it */
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
  /* Asked for rather than waited on: if WiFi is already up this is answered
   * in a second, and if it is not the job simply finds nothing. Either way
   * the shell starts now. */
  bg_submit(BG_TIME_SYNC);

  {
    static const ShellOps OPS = { ops_open_app, ops_switch_shell, feed_key, ops_running_app };
    static const InputSink SINK = { sink_wants_text, feed_key };
    shell_set_ops(&OPS);
    input_set_sink(&SINK);
  }

  /* Safe mode always lands at the console: it is the one shell that cannot be
   * hidden by a setting, and the one with `defaults` in it. */
  if (s_safe_mode) {
    prompt();
    s_mode = MODE_CONSOLE;
  } else switch (ui_saved_shell()) {
  case UI_DESKTOP:  desktop_init(); s_mode = MODE_DESKTOP; break;
  case UI_NONE:     prompt();       s_mode = MODE_CONSOLE; break;
  default:          launchui_init(); s_mode = MODE_LAUNCHER; break;
  }

  for (;;) {
    uint8_t k = keyboard_poll();

    /* The button on top, polled beside the keyboard so it works in every
     * shell and over every app. Press and hold to talk; see kernel/sys/voice.c
     * for why the whole cycle happens inside this call. */
    agent_tick();
    if (voice_tick()) {
      if (s_mode == MODE_LAUNCHER) launchui_repaint();
      else if (s_mode == MODE_DESKTOP) desktop_repaint();
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
    }

    if (!k) {
      int sc = con_serial_key();
      /* Not a key: 0xFF is outside the alphabet, and it means "screenshot".
       * Taken here, before any app sees it, so a shot can be of anything. */
      if (sc == 0xFF) { serial_shot(); sc = 0; }
      if (sc == '\r' || sc == '\n') k = KEY_ENTER;
      else if (sc == 0x7F || sc == 0x08) k = KEY_BACKSPACE;
      else if (sc == 0x1B) k = KEY_ESC;
      else if (sc > 0) k = (uint8_t)sc;
    }

    /* Anything the user did resets the idle clock -- which is what decides
     * whether the machine is allowed to go and do slow things, and how hard
     * this loop polls. */
    if (k) {
      bg_note_activity();
      /* Wake first. A key pressed at a dark screen means "come back", and
       * acting on it as well would open an app nobody asked for. */
      if (power_dimmed()) { power_wake(); k = 0; }
    }
    power_tick();
    clock_persist_tick();   /* writes at most once every ten minutes */

    /* What the background task finished while nobody was waiting. One per
     * pass, and only the shell ever draws it. */
    {
      const char *done = bg_take_result();
      if (done) {
        if (s_mode == MODE_CONSOLE) { con_printf("%s\n", done); prompt(); }
        else if (s_mode == MODE_LAUNCHER) launchui_note(done);
      }
    }

    /* Before any shell sees it. */
    if (k && global_key(k)) k = 0;

    if (s_mode == MODE_LAUNCHER) {
      MouseReport mr;
      int got = 0;
      while (bthid_poll_mouse(&mr)) { launchui_mouse_apply(&mr); got = 1; }
      if (got) launchui_mouse_done();
      launchui_tick((uint32_t)(esp_timer_get_time() / 1000));
      if (k) {
        int r = launchui_key(k);
        if (r == 1) {
          s_mode = MODE_CONSOLE;
          ui_set_shell(UI_NONE);
          con_clear();
          con_write("back at the console\n");
          prompt();
        } else if (r == 2) {
          s_mode = MODE_DESKTOP;      /* the launcher handed over */
        }
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (s_mode == MODE_DESKTOP) {
      MouseReport mr;
      /* Drain whatever the radio queued. It arrives on the Bluetooth task,
       * which must not touch the display, so this is where it turns into
       * pointer movement. */
      {
        int got = 0;
        while (bthid_poll_mouse(&mr)) { desktop_mouse_apply(&mr); got = 1; }
        if (got) desktop_mouse_done();   /* one repaint for the whole burst */
      }
      desktop_tick((uint32_t)(esp_timer_get_time() / 1000));
      if ((k && desktop_key(k)) || desktop_take_leave()) {
        /* ESC left the desktop: hand the screen back to the console. */
        s_mode = MODE_CONSOLE;
        ui_set_shell(UI_NONE);
        con_clear();
        con_write("back at the console\n");
        prompt();
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (k) {
      console_key(k);
      if (s_mode == MODE_CONSOLE) {
        last_blink = esp_timer_get_time();
        blink = 1;
        con_cursor(1);
      }
    }

    {
      int64_t now = esp_timer_get_time();
      if (s_mode == MODE_CONSOLE && now - last_blink > 500000) {   /* 500 ms */
        last_blink = now;
        blink = !blink;
        con_cursor(blink);
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
      int ms = (s_mode != MODE_CONSOLE) ? 5 : 10;
      if (quiet > 2000) ms = 25;
      vTaskDelay(pdMS_TO_TICKS(ms));
    }
  }
}

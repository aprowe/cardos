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
#include "kernel/fs/path.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/launchui.h"
#include "kernel/net/wifi.h"
#include "kernel/ui/shell.h"
#include "kernel/sys/env.h"
#include "kernel/sys/sio.h"
#include "kernel/ui/help.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "kernel/drv/bthid.h"

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
 * Was 128 KB, which was chosen when the radios did not exist. Measured with
 * both up: WiFi costs 49792 bytes and Bluetooth 67108, and with a 128 KB
 * arena the two together left 1156 bytes free and the WiFi driver began
 * failing buffer allocations ("wifi:m f null" in a loop). 48 KB is still more
 * than anything currently allocates from it and leaves the radios room to
 * coexist. */
#define CARDOS_HEAP_BYTES (48 * 1024)

static uint8_t *s_heap;
static char     s_line[CARDOS_LINE_MAX + 1];
static int      s_len;
/* Three shells over the same kernel. The launcher is the one meant for daily
 * use, the desktop is the demonstration that windows work, and the console is
 * for development. */
typedef enum { MODE_CONSOLE = 0, MODE_DESKTOP, MODE_LAUNCHER } Mode;
static Mode     s_mode;

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
  con_printf("handle arena     %6u B  reserved at boot\n",
             (unsigned)CARDOS_HEAP_BYTES);
  con_printf("bluetooth        %6u B\n", (unsigned)bthid_heap_cost());
  con_printf("wifi             %6u B\n", (unsigned)wifi_heap_cost());
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
  con_write("shell    env set NAME=VALUE, tab completes, up recalls\n");
  con_write("radios   wifi [scan|SSID PASS|saved|forget|off]\n");
  con_write("         mouse, get URL\n");
  con_write("screens  launch (carousel), desk (windows), escape returns\n");
  con_write("boot     apps, boot NAME, boot! NAME, bootinfo\n");
  con_write("system   mem ps taskcost flip clear reboot echo\n");
  con_write("on card  cat grep -- run with no argument lists them\n");
}

/* A stand-in for the real shell, which needs the context switch so it can run
 * as a task and block on the keyboard rather than polling it. */
/* One command, already separated from its pipeline and redirections. */
static void run_builtin(const char *line, char *arg) {
  if (!strcmp(line, "help"))        cmd_help();
  else if (!strcmp(line, "mem"))    cmd_mem();
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
  else if (!strcmp(line, "env"))  cmd_env();
  else if (!strcmp(line, "set"))  cmd_set(arg);
  else if (!strcmp(line, "google")) cmd_google(arg);
  else if (!strcmp(line, "run")) {
    /* cmd_run starts it; the mode has to change here, where the loop is. */
    cmd_run(arg);
    if (arg && *arg && ui_shell() == UI_LAUNCHER) s_mode = MODE_LAUNCHER;
  }
  else if (!strcmp(line, "clear"))  con_clear();
  else if (!strcmp(line, "reboot")) esp_restart();
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
  "echo", "flip", "get", "gui", "help", "launch", "ls", "mem", "mkdir",
  "mouse", "ps", "pwd", "reboot", "rm", "run", "taskcost", "wifi",
};
#define NCOMMANDS ((int)(sizeof COMMANDS / sizeof COMMANDS[0]))

/* How many leading characters `a` and `b` share. */
static int common(const char *a, const char *b) {
  int i = 0;
  while (a[i] && b[i] && a[i] == b[i]) i++;
  return i;
}

/* Split the path being typed into the directory to list and the stem to match
 * inside it. "/desk" -> "/" and "desk"; "/desktop/mi" -> "/desktop" and "mi". */
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
    if (dir[0] == '.' && dir[1] == 0) snprintf(full, sizeof full, "%s", shell_cwd());
    else snprintf(full, sizeof full, "%s", dir);

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
      if (dir[0] == '.' && dir[1] == 0) snprintf(full, sizeof full, "%s", shell_cwd());
      else snprintf(full, sizeof full, "%s", dir);
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
  int i = 0, n = 0;

  while (st->text[i]) {
    char c = st->text[i];

    if (c == '<' || c == '>') {
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
  help_paint("Shortcuts", "opt-1\tlauncher\nopt-2\tdesktop\nopt-3\tconsole\nopt-t\ttodo\nopt-s\tstocks\nopt-e\tedit\nopt-m\tmines\nopt-b\treconnect bluetooth\nopt-w\treconnect wifi\n",
             "ctrl-h\tthe keys of whatever is running\nany key\tclose this\n");
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

  /* A letter runs the app of that name. The table is here rather than in a
   * settings file because these are the two that earn a chord; anything else
   * is a name away in the launcher. */
  case KEY_OPT_LETTER('t'):
  case KEY_OPT_LETTER('s'):
  case KEY_OPT_LETTER('e'):
  case KEY_OPT_LETTER('m'): {
    const char *name = (k == KEY_OPT_LETTER('t')) ? "Todo"
                     : (k == KEY_OPT_LETTER('s')) ? "Stocks"
                     : (k == KEY_OPT_LETTER('e')) ? "Edit" : "Mines";
    if (shell_exec(name, NULL) == 0 && ui_shell() == UI_LAUNCHER)
      s_mode = MODE_LAUNCHER;
    return 1;
  }

  default:
    /* Every other opt chord is swallowed rather than passed on: a shortcut
     * that is not bound should do nothing, not type a letter. */
    return KEY_IS_OPT(k);
  }
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
    kmem_init(s_heap, CARDOS_HEAP_BYTES);
  }

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
      nvs_flash_erase();
      err = nvs_flash_init();
    }
    if (err != ESP_OK) con_write("nvs unavailable: settings will not stick\n");
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
  /* A bonded mouse or keyboard is usually still advertising, so one short look
   * before the shell comes up saves reaching for Settings. Off unless asked
   * for: the radio costs ~67 KB for the whole uptime. */
  if (bthid_autostart()) {
    con_write("bluetooth: looking for paired devices...\n");
    con_printf("  %d connected\n", bthid_autoconnect(1));
  }

  /* Back into whichever shell was last used. A device that always boots to the
   * same one regardless of what you were doing is a device you have to re-enter
   * a command on every time -- and it makes the firmware round trip land where
   * you left from, which is the behaviour the rollback was built for. */
  switch (ui_saved_shell()) {
  case UI_DESKTOP:  desktop_init(); s_mode = MODE_DESKTOP; break;
  case UI_NONE:     prompt();       s_mode = MODE_CONSOLE; break;
  default:          launchui_init(); s_mode = MODE_LAUNCHER; break;
  }

  for (;;) {
    uint8_t k = keyboard_poll();

    /* A character arriving on the serial console counts as a keypress, so the
     * shell can be driven from a PC as well as from the keyboard. */
    /* A Bluetooth keyboard is the same keyboard as the built-in one from here
     * on: the decoder emits the same byte alphabet, so nothing downstream
     * needs to know which one a key came from. */
    if (!k) {
      bthid_tick((uint32_t)(esp_timer_get_time() / 1000));
      bthid_poll_key(&k);
    }

    if (!k) {
      int sc = con_serial_key();
      if (sc == '\r' || sc == '\n') k = KEY_ENTER;
      else if (sc == 0x7F || sc == 0x08) k = KEY_BACKSPACE;
      else if (sc == 0x1B) k = KEY_ESC;
      else if (sc > 0) k = (uint8_t)sc;
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
      if (s_mode == MODE_CONSOLE) con_cursor(0);
      if (k == KEY_ENTER) {
        s_line[s_len] = 0;
        con_putc('\n');
        hist_add(s_line);
        s_hist_at = 0;
        s_hist_saved[0] = 0;
        run_pipeline(s_line);
        s_len = 0;
        /* Only if the console still owns the screen. `desk` and `launch` paint
         * a whole shell from inside run_pipeline, and printing a prompt
         * afterwards drew a line of console over the top of it. */
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

    /* 5 ms while the desktop is up: the pointer wants to keep up with the
     * hand, and the keyboard matrix scan is cheap. */
    vTaskDelay(pdMS_TO_TICKS(s_mode != MODE_CONSOLE ? 5 : 10));
  }
}

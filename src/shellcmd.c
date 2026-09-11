/* Filesystem commands for the MVP console.
 *
 * Kept out of main.c so the boot path stays readable. This is not the real
 * shell -- that needs the scheduler's context switch so it can run as a task
 * and block on the keyboard -- but the commands themselves are the ones the
 * spec lists, and they exercise the filesystem against a real card.
 */

#include "shellcmd.h"
#include "kernel/net/wifi.h"
#include "kernel/net/http.h"
#include "kernel/ui/icons.h"
#include "kernel/ui/launchui.h"

#include <stdio.h>
#include <string.h>

#include "kernel/app/appimage.h"
#include "kernel/app/launcher.h"
#include "kernel/console/console.h"
#include "kernel/drv/display.h"
#include "kernel/fs/fs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "kernel/drv/bthid.h"
#include "kernel/fs/path.h"

static char s_cwd[FS_PATH_MAX] = "/";

const char *shell_cwd(void) { return s_cwd; }

static void err(const char *what, const char *why) {
  con_set_color(COLOR_RED);
  con_printf("%s: %s\n", what, why);
  con_set_color(COLOR_GREEN);
}

/* Resolve an argument against the working directory, reporting failure. */
static int resolve(const char *arg, char *out) {
  if (path_resolve(s_cwd, arg ? arg : "", out, FS_PATH_MAX) != 0) {
    err("path", "too long or malformed");
    return -1;
  }
  return 0;
}

void cmd_pwd(void) { con_printf("%s\n", s_cwd); }

void cmd_cd(const char *arg) {
  char target[FS_PATH_MAX];
  FsStat st;

  if (!arg || !*arg) { strcpy(s_cwd, "/"); return; }
  if (resolve(arg, target) != 0) return;

  if (strcmp(target, "/") != 0) {
    if (fs_stat(target, &st) != 0) { err(arg, "no such directory"); return; }
    if (!st.is_dir) { err(arg, "not a directory"); return; }
  }
  strcpy(s_cwd, target);
}

void cmd_ls(const char *arg) {
  char target[FS_PATH_MAX];
  FsEntry e;
  FsDir d;
  int n = 0;

  if (!fs_mounted()) { err("ls", "no card mounted"); return; }
  if (resolve(arg && *arg ? arg : "", target) != 0) return;

  /* Iterating rather than filling an array: an FsEntry is 72 bytes, and the
   * array version measured 2464 bytes of stack against a 1 KB task stack. It
   * also stopped at 32 files without saying so. */
  if (fs_opendir(target, &d) != 0) { err(target, "cannot list"); return; }
  while (fs_readdir(&d, &e) == 1) {
    n++;
    if (e.is_dir) {
      con_set_color(COLOR_WHITE);
      con_printf("%s/\n", e.name);
      con_set_color(COLOR_GREEN);
    } else {
      con_printf("%-28s %6u\n", e.name, (unsigned)e.size);
    }
  }
  fs_closedir(&d);
  if (n == 0) con_write("(empty)\n");
}

void cmd_cat(const char *arg) {
  char target[FS_PATH_MAX];
  char buf[128];
  int fd, got, shown = 0;

  if (!fs_mounted()) { err("cat", "no card mounted"); return; }
  if (!arg || !*arg) { err("cat", "needs a filename"); return; }
  if (resolve(arg, target) != 0) return;

  fd = fs_open(target, FS_O_READ);
  if (fd < 0) { err(arg, "cannot open"); return; }

  /* A 40x16 console is not a pager. Stop at a screenful rather than scrolling
   * a megabyte of binary past the user. */
  while (shown < 15 * 40 && (got = fs_read(fd, buf, sizeof buf)) > 0) {
    int i;
    for (i = 0; i < got && shown < 15 * 40; i++) {
      char c = buf[i];
      con_putc((c == '\n' || (c >= 0x20 && c < 0x7F)) ? c : '.');
      shown++;
    }
  }
  fs_close(fd);
  con_putc('\n');
}

void cmd_df(void) {
  uint64_t total = 0, freeb = 0;
  if (!fs_mounted()) { err("df", "no card mounted"); return; }
  fs_space(&total, &freeb);
  con_printf("card %u MB total\n", (unsigned)(total / (1024 * 1024)));
  con_printf("     %u MB free\n", (unsigned)(freeb / (1024 * 1024)));
}

void cmd_mkdir(const char *arg) {
  char target[FS_PATH_MAX];
  if (!fs_mounted()) { err("mkdir", "no card mounted"); return; }
  if (!arg || !*arg) { err("mkdir", "needs a name"); return; }
  if (resolve(arg, target) != 0) return;
  if (fs_mkdir(target) != 0) err(arg, "cannot create");
}

void cmd_rm(const char *arg) {
  char target[FS_PATH_MAX];
  if (!fs_mounted()) { err("rm", "no card mounted"); return; }
  if (!arg || !*arg) { err("rm", "needs a name"); return; }
  if (resolve(arg, target) != 0) return;
  if (fs_remove(target) != 0) err(arg, "cannot remove");
}

/* ------------------------------------------------------------- apps ----- */

/* Read through the filesystem so the validator stays free of ESP-IDF. */
static int fs_reader(void *ctx, uint32_t offset, void *buf, size_t n) {
  int fd = *(int *)ctx;
  if (fs_seek(fd, (int32_t)offset, FS_SEEK_SET) < 0) return -1;
  return fs_read(fd, buf, n) == (int)n ? 0 : -1;
}

#define GUEST_PARTITION_BYTES (3u * 1024 * 1024)   /* ota_0 in partitions.csv */

/* Static rather than local: this and the AppImageInfo below were the bulk of
 * a 2608-byte stack frame, and task stacks are 1 KB. Nothing here is
 * re-entrant -- the kernel is cooperative and this does not yield. */
static char        s_full[FS_PATH_MAX];
static AppImageInfo s_info;

static int list_apps_in(const char *dir) {
  FsEntry e;
  FsDir d;
  int found = 0;

  if (fs_opendir(dir, &d) != 0) return -1;   /* the directory is not there */

  while (fs_readdir(&d, &e) == 1) {
    AppImageResult r;
    int fd;
    size_t len = strlen(e.name);

    if (e.is_dir) continue;
    if (len < 4 || strcmp(e.name + len - 4, ".bin") != 0) continue;
    found++;

    if (snprintf(s_full, sizeof s_full, "%s/%s", dir, e.name) < 0) continue;
    fd = fs_open(s_full, FS_O_READ);
    if (fd < 0) { con_printf("%-16s unreadable\n", e.name); continue; }

    r = appimage_parse(fs_reader, &fd, e.size, GUEST_PARTITION_BYTES, &s_info);
    fs_close(fd);

    if (r != APPIMAGE_OK) {
      con_set_color(COLOR_RED);
      con_printf("%-16s %s\n", e.name, appimage_strerror(r));
      con_set_color(COLOR_GREEN);
      continue;
    }
    /* The app descriptor is why this says "bruce 1.2.3" and not "bruce.bin". */
    con_printf("%-16s %s %s %uK\n", e.name,
               s_info.has_app_desc ? s_info.project_name : "?",
               s_info.has_app_desc ? s_info.version : "",
               (unsigned)(s_info.image_size / 1024));
  }
  fs_closedir(&d);
  return found;
}

void cmd_apps(void) {
  int a, b;
  if (!fs_mounted()) { err("apps", "no card mounted"); return; }
  /* CardLaunch keeps its firmware in /firmware. Look there as well as in the
   * layout this spec fixes, rather than making the user rearrange a card full
   * of working apps. */
  a = list_apps_in("/cardos/apps");
  b = list_apps_in("/firmware");
  if (a <= 0 && b <= 0) con_write("no .bin in /cardos/apps or /firmware\n");
}

/* ------------------------------------------------------------ booting --- */

/* Find an app by bare name or path, in either directory. */
static int find_app(const char *name, char *out) {
  static const char *dirs[] = { "/cardos/apps", "/firmware" };
  FsStat st;
  size_t i;

  if (!name || !*name) return -1;

  if (name[0] == '/') {                       /* an explicit path */
    if (path_resolve(s_cwd, name, out, FS_PATH_MAX) != 0) return -1;
    return fs_stat(out, &st) == 0 ? 0 : -1;
  }
  for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    if (snprintf(out, FS_PATH_MAX, "%s/%s", dirs[i], name) < 0) continue;
    if (fs_stat(out, &st) == 0) return 0;
    if (snprintf(out, FS_PATH_MAX, "%s/%s.bin", dirs[i], name) < 0) continue;
    if (fs_stat(out, &st) == 0) return 0;
  }
  return -1;
}

static void boot_progress(void *ctx, int percent) {
  (void)ctx;
  if (percent % 10) return;                   /* the console is not a bar */
  con_printf("%d%% ", percent);
}

void cmd_bootinfo(void) {
  LauncherInfo info;
  launcher_info(&info);
  con_printf("running from %s\n", info.running[0] ? info.running : "?");
  if (info.guest_valid)
    con_printf("guest slot: %s %s %uK\n",
               info.guest_name[0] ? info.guest_name : "?",
               info.guest_version, (unsigned)(info.guest_size / 1024));
  else
    con_write("guest slot: empty\n");
}

/* `boot` is a dry run on purpose. The real thing does not return, and a
 * mistyped name should not cost a reboot. */
void cmd_boot(const char *arg, int confirmed) {
  char full[FS_PATH_MAX];
  AppImageInfo info;
  AppImageResult why;
  LaunchResult r;

  if (!fs_mounted()) { err("boot", "no card mounted"); return; }
  if (!arg || !*arg)  { err("boot", "needs an app name"); return; }
  if (find_app(arg, full) != 0) { err(arg, "not found"); return; }

  r = launcher_check(full, &info, &why);
  if (r != LAUNCH_OK) {
    err(arg, r == LAUNCH_ERR_IMAGE ? appimage_strerror(why)
                                   : launcher_strerror(r));
    return;
  }

  if (!confirmed) {
    con_printf("%s: %s %s, %uK\n", arg,
               info.has_app_desc ? info.project_name : "?",
               info.has_app_desc ? info.version : "",
               (unsigned)(info.image_size / 1024));
    con_set_color(COLOR_AMBER);
    con_write("this ends CardOS. reset returns.\n");
    con_printf("run: boot! %s\n", arg);
    con_set_color(COLOR_GREEN);
    return;
  }

  con_printf("copying %uK ", (unsigned)(info.image_size / 1024));
  r = launcher_boot(full, boot_progress, NULL);
  /* Only reached on failure: success restarts the chip. */
  con_putc('\n');
  err(arg, launcher_strerror(r));
}


/* ------------------------------------------------------- task cost ------ */

static void idle_body(void *arg) {
  (void)arg;
  for (;;) vTaskDelay(portMAX_DELAY);
}

/* The kernel spec justifies a hand-written context switch with "~1 KB/task vs
 * FreeRTOS's ~4 KB". That number was never measured on this board, and it
 * decides whether writing Xtensa assembly is worth the risk. So measure it. */
void cmd_taskcost(void) {
  static TaskHandle_t h[8];
  const int n = 8;
  size_t before, after;
  int i, made = 0;

  before = esp_get_free_heap_size();
  for (i = 0; i < n; i++) {
    /* 1024 words is the CardOS spec's per-task stack size in bytes; FreeRTOS
     * takes the depth in words, so ask for the same 1 KB. */
    if (xTaskCreate(idle_body, "probe", 1024 / sizeof(StackType_t), NULL,
                    1, &h[i]) == pdPASS) made++;
    else h[i] = NULL;
  }
  after = esp_get_free_heap_size();

  con_printf("%d tasks x 1KB stack\n", made);
  con_printf("heap %u -> %u\n", (unsigned)before, (unsigned)after);
  if (made)
    con_printf("cost %u bytes each\n", (unsigned)((before - after) / (unsigned)made));

  for (i = 0; i < n; i++) if (h[i]) vTaskDelete(h[i]);
  vTaskDelay(pdMS_TO_TICKS(50));      /* let the idle task reap them */
  con_printf("after free %u\n", (unsigned)esp_get_free_heap_size());
}


/* -------------------------------------------------------------- mouse -- */

void cmd_mouse(const char *arg) {
  if (arg && !strcmp(arg, "off")) {
    bthid_stop(BTHID_MOUSE);
    con_printf("mouse %s\n", bthid_status(BTHID_MOUSE));
    return;
  }
  if (arg && !strcmp(arg, "status")) {
    con_printf("mouse %s\n", bthid_status(BTHID_MOUSE));
    if (bthid_heap_cost())
      con_printf("radio cost %u KB\n", (unsigned)(bthid_heap_cost() / 1024));
    return;
  }

  con_write("pairing mode only needed once\n");
  con_write("scanning 6s...\n");
  bthid_start(6, BTHID_MOUSE);
  con_printf("%s\n", bthid_status(BTHID_MOUSE));
  if (bthid_heap_cost())
    con_printf("radio cost %u KB, heap %u KB\n",
               (unsigned)(bthid_heap_cost() / 1024),
               (unsigned)(esp_get_free_heap_size() / 1024));
}


/* ---------------------------------------------------------------- wifi --- */

void cmd_wifi(const char *arg) {
  char ssid[WIFI_SSID_MAX], pass[WIFI_PASS_MAX];
  const char *sp;

  if (!arg || !*arg) {
    con_printf("wifi: %s\n", wifi_status());
    if (wifi_saved_ssid()[0])
      con_printf("saved: %s\n", wifi_saved_ssid());
    con_printf("radio cost %u KB\n", (unsigned)(wifi_heap_cost() / 1024));
    con_write("wifi scan | wifi SSID PASS | wifi saved | wifi forget | wifi off\n");
    return;
  }

  if (!strcmp(arg, "scan")) {
    WifiAp aps[WIFI_MAX_SCAN];
    int n, i;
    con_write("scanning...\n");
    n = wifi_scan(aps, WIFI_MAX_SCAN);
    if (n == 0) { con_write("nothing found\n"); return; }
    for (i = 0; i < n; i++)
      con_printf("  %-24s %4d dBm%s\n", aps[i].ssid, aps[i].rssi,
                 aps[i].open ? "  open" : "");
    return;
  }

  if (!strcmp(arg, "off"))    { wifi_stop(); con_write("radio off\n"); return; }
  if (!strcmp(arg, "forget")) { wifi_forget(); con_write("forgotten\n"); return; }
  if (!strcmp(arg, "saved")) {
    con_write("joining saved network...\n");
    wifi_connect_saved(20000);
    con_printf("%s\n", wifi_status());
    return;
  }

  /* wifi SSID PASS. An SSID with a space in it has to be joined from the
   * Settings app instead; splitting on the first space is the price of a
   * one-line shell. */
  sp = strchr(arg, ' ');
  if (!sp) {
    snprintf(ssid, sizeof ssid, "%s", arg);
    pass[0] = 0;
  } else {
    int n = (int)(sp - arg);
    if (n >= (int)sizeof ssid) n = (int)sizeof ssid - 1;
    snprintf(ssid, sizeof ssid, "%.*s", n, arg);
    snprintf(pass, sizeof pass, "%s", sp + 1);
  }
  con_printf("joining %s...\n", ssid);
  wifi_connect(ssid, pass, 20000);
  con_printf("%s\n", wifi_status());
}

void cmd_get(const char *arg) {
  static char buf[1600];
  int n;

  if (!arg || !*arg) { con_write("usage: get URL\n"); return; }
  if (!wifi_is_connected()) {
    con_write("no network, joining saved...\n");
    wifi_connect_saved(20000);
  }
  con_printf("GET %s\n", arg);
  n = http_get(arg, buf, sizeof buf, 15000);
  if (n < 0) { con_printf("failed (%d)\n", n); return; }
  con_printf("%d bytes\n%.600s\n", n, buf);
}

/* ----------------------------------------------------------------- run --- */

static const char *run_kind(IconKind k) {
  switch (k) {
  case ICON_FIRMWARE: return "firmware";
  case ICON_CAPP:     return "app";
  default:            return "built in";
  }
}

void cmd_run(const char *arg) {
  int i;

  if (!arg || !*arg) {
    icons_reload();
    if (icons_count() == 0) { con_write("nothing to run\n"); return; }
    for (i = 0; i < icons_count(); i++) {
      const Icon *ic = icon_at(i);
      con_printf("  %-14s %-9s %s\n", ic->name, run_kind(ic->kind), ic->path);
    }
    con_write("run NAME\n");
    return;
  }

  /* Firmware goes through boot, which asks first: it ends CardOS, and a
   * mistyped name should not cost a reboot. */
  icons_reload();
  for (i = 0; i < icons_count(); i++) {
    const Icon *ic = icon_at(i);
    if (!ic) continue;
    if (strcasecmp(ic->name, arg) != 0) continue;
    if (ic->kind == ICON_FIRMWARE) {
      con_printf("%s is firmware. use: boot %s\n", ic->name, ic->name);
      return;
    }
    break;
  }

  if (launchui_run(arg) != 0) {
    err(arg, "no such app -- run with no argument lists them");
    return;
  }
}

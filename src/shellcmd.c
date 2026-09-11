/* Filesystem commands for the MVP console.
 *
 * Kept out of main.c so the boot path stays readable. This is not the real
 * shell -- that needs the scheduler's context switch so it can run as a task
 * and block on the keyboard -- but the commands themselves are the ones the
 * spec lists, and they exercise the filesystem against a real card.
 */

#include "shellcmd.h"

#include <stdio.h>
#include <string.h>

#include "app/appimage.h"
#include "console/console.h"
#include "drv/display.h"
#include "fs/fs.h"
#include "fs/path.h"

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
  FsEntry entries[32];
  int n, i;

  if (!fs_mounted()) { err("ls", "no card mounted"); return; }
  if (resolve(arg && *arg ? arg : "", target) != 0) return;

  n = fs_list(target, entries, 32);
  if (n < 0) { err(target, "cannot list"); return; }
  if (n == 0) { con_write("(empty)\n"); return; }

  for (i = 0; i < n; i++) {
    if (entries[i].is_dir) {
      con_set_color(COLOR_WHITE);
      con_printf("%s/\n", entries[i].name);
      con_set_color(COLOR_GREEN);
    } else {
      con_printf("%-28s %6u\n", entries[i].name, (unsigned)entries[i].size);
    }
  }
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

#define GUEST_PARTITION_BYTES (4u * 1024 * 1024)

void cmd_apps(void) {
  FsEntry entries[32];
  int n, i, found = 0;

  if (!fs_mounted()) { err("apps", "no card mounted"); return; }

  n = fs_list("/cardos/apps", entries, 32);
  if (n < 0) { err("/cardos/apps", "cannot list"); return; }

  for (i = 0; i < n; i++) {
    char full[FS_PATH_MAX];
    AppImageInfo info;
    AppImageResult r;
    int fd;
    size_t len = strlen(entries[i].name);

    if (entries[i].is_dir) continue;
    if (len < 4 || strcmp(entries[i].name + len - 4, ".bin") != 0) continue;
    found++;

    if (snprintf(full, sizeof full, "/cardos/apps/%s", entries[i].name) < 0) continue;
    fd = fs_open(full, FS_O_READ);
    if (fd < 0) { con_printf("%-16s unreadable\n", entries[i].name); continue; }

    r = appimage_parse(fs_reader, &fd, entries[i].size,
                       GUEST_PARTITION_BYTES, &info);
    fs_close(fd);

    if (r != APPIMAGE_OK) {
      con_set_color(COLOR_RED);
      con_printf("%-16s %s\n", entries[i].name, appimage_strerror(r));
      con_set_color(COLOR_GREEN);
      continue;
    }
    /* The app descriptor is why this says "bruce 1.2.3" and not "bruce.bin". */
    con_printf("%-16s %s %s %uK\n", entries[i].name,
               info.has_app_desc ? info.project_name : "?",
               info.has_app_desc ? info.version : "",
               (unsigned)(info.image_size / 1024));
  }
  if (!found) con_write("no .bin files in /cardos/apps\n");
}

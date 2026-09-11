/* The contents of /desktop. See icons.h. */

#include "kernel/ui/icons.h"
#include "kernel/fs/fs.h"
#include "kernel/app/capprun.h"
#include "kernel/app/capp_blobs.h"
#include "kernel/app/launcher.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/icons_builtin.h"

#include <stdio.h>
#include <string.h>

static Icon s_icon[MAX_ICONS];
static int  s_nicon;

/* The .capp binaries are carried in the firmware and written out to the card.
 * There is no card reader in the loop, so an app that can only arrive by
 * copying a file onto the card is an app the user never gets.
 *
 * A checksum of every embedded blob, so a firmware whose apps changed can tell
 * that the copies on the card are stale. Presence alone is not enough: the
 * first version of this only wrote a .capp when the file was missing, and an
 * API version bump then left three unloadable binaries on the card with no
 * way to notice. */
static uint32_t blob_stamp(void) {
  uint32_t h = 2166136261u;
  size_t i, j;
  for (i = 0; i < CAPP_BLOB_COUNT; i++) {
    for (j = 0; j < CAPP_BLOBS[i].size; j++) {
      h ^= CAPP_BLOBS[i].data[j];
      h *= 16777619u;
    }
  }
  return h;
}

/* fs_write returns -1 on a short write, and the bytes it did manage are
 * already in the file. Writing in chunks and checking each one is the
 * difference between a truncated app and a retried one: edit.capp landed on
 * the card as 650 bytes of a 14720-byte file, and because the stamp was
 * written anyway, every later boot believed it was current. */
static int write_all(int fd, const uint8_t *data, size_t size) {
  size_t done = 0;
  int stalls = 0;

  while (done < size) {
    size_t chunk = size - done;
    int n;
    if (chunk > 1024) chunk = 1024;
    n = fs_write(fd, data + done, chunk);
    if (n > 0) { done += (size_t)n; stalls = 0; continue; }
    if (++stalls > 3) return -1;      /* not going to get better by asking again */
  }
  return 0;
}

static int file_size(const char *path) {
  int fd = fs_open(path, FS_O_READ);
  int n;
  if (fd < 0) return -1;
  n = fs_seek(fd, 0, FS_SEEK_END);
  fs_close(fd);
  return n;
}

static void seed_capps(void) {
  uint32_t want = blob_stamp(), have = 0;
  size_t i;
  char path[80];
  int fd, all_ok = 1;

  fd = fs_open(ICONS_DIR "/.capps", FS_O_READ);
  if (fd >= 0) {
    if (fs_read(fd, &have, sizeof have) != (int)sizeof have) have = 0;
    fs_close(fd);
  }

  for (i = 0; i < CAPP_BLOB_COUNT; i++) {
    int ok;
    snprintf(path, sizeof path, "%s/%s", ICONS_DIR, CAPP_BLOBS[i].name);

    /* The stamp says which firmware wrote these, and the size says whether the
     * write finished. Both are checked, because a matching stamp over a
     * truncated file is exactly the state that made edit.capp stay broken at
     * 650 bytes of 14720 across every reboot. */
    if (have == want && file_size(path) == (int)CAPP_BLOBS[i].size) continue;

    fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) { all_ok = 0; continue; }
    ok = (write_all(fd, CAPP_BLOBS[i].data, CAPP_BLOBS[i].size) == 0);
    fs_close(fd);

    /* Reopened and measured rather than trusted. A half-written app that
     * loads is worse than one that is plainly missing, so a bad one goes. */
    if (ok && file_size(path) != (int)CAPP_BLOBS[i].size) ok = 0;
    if (!ok) { fs_remove(path); all_ok = 0; }
  }

  /* The stamp means "every blob on the card is this firmware's". Writing it
   * after a failure is what made the truncation permanent. */
  if (!all_ok || have == want) return;
  fd = fs_open(ICONS_DIR "/.capps", FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd >= 0) { fs_write(fd, &want, sizeof want); fs_close(fd); }
}

/* Seed the folder the first time, so a fresh card still has something to click
 * rather than an empty desktop with no clue what to do. Each entry is created
 * only if absent rather than only on a wholly empty folder: a card that
 * already holds one icon should still get the rest. */
static void seed_dir(void) {
  static const char *seed[] = { "Files.app", "Settings.app" };
  size_t i;
  char path[80];

  if (fs_mkdir(ICONS_DIR) != 0) { /* already there, or no card */ }

  for (i = 0; i < sizeof seed / sizeof seed[0]; i++) {
    int fd;
    snprintf(path, sizeof path, "%s/%s", ICONS_DIR, seed[i]);
    fd = fs_open(path, FS_O_READ);
    if (fd >= 0) { fs_close(fd); continue; }
    fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd >= 0) { fs_write(fd, "cardos app", 10); fs_close(fd); }
  }

  seed_capps();
}

static int ends_with(const char *name, size_t n, const char *ext) {
  size_t e = strlen(ext);
  return n > e && strcmp(name + n - e, ext) == 0;
}

/* Scan one folder. `bin_only` for /firmware, which holds images and nothing
 * this launcher can run any other way. */
static void scan(const char *dir, int bin_only) {
  FsDir d;
  FsEntry e;

  if (fs_opendir(dir, &d) != 0) return;

  while (s_nicon < MAX_ICONS && fs_readdir(&d, &e) == 1) {
    size_t n = strlen(e.name);
    Icon *ic = &s_icon[s_nicon];
    if (e.is_dir) continue;
    if (e.name[0] == '.') continue;      /* the stamp file, and anything like it */

    snprintf(ic->path, sizeof ic->path, "%s/%s", dir, e.name);

    if (ends_with(e.name, n, ".bin")) {
      ic->kind = ICON_FIRMWARE;
      ic->slot = -1;
      snprintf(ic->name, sizeof ic->name, "%.*s", (int)(n - 4), e.name);
    } else if (bin_only) {
      continue;
    } else if (ends_with(e.name, n, ".capp")) {
      /* Loaded eagerly and left loaded: the label under the icon and the icon
       * itself are the app's own, and asking it is the only way to know
       * them. */
      const AppDef *def;
      ic->slot = capprun_load(ic->path);
      if (ic->slot < 0) continue;
      def = capprun_def(ic->slot);
      ic->kind = ICON_CAPP;
      snprintf(ic->name, sizeof ic->name, "%s", def->name);
    } else if (ends_with(e.name, n, ".app")) {
      char stem[20];
      snprintf(stem, sizeof stem, "%.*s", (int)(n - 4), e.name);
      ic->slot = app_index_by_name(stem);
      if (ic->slot < 0) continue;            /* names an app we do not have */
      ic->kind = ICON_BUILTIN;
      snprintf(ic->name, sizeof ic->name, "%s", stem);
    } else {
      continue;
    }
    s_nicon++;
  }
  fs_closedir(&d);
}

void icons_reload(void) {
  s_nicon = 0;
  capprun_unload_all();
  if (!fs_mounted()) return;

  seed_dir();
  scan(ICONS_DIR, 0);
  scan(FIRMWARE_DIR, 1);
}

int icons_count(void) { return s_nicon; }

const Icon *icon_at(int i) {
  if (i < 0 || i >= s_nicon) return NULL;
  return &s_icon[i];
}

const AppDef *icon_app(int i) {
  const Icon *ic = icon_at(i);
  if (!ic) return NULL;
  if (ic->kind == ICON_BUILTIN) return app_at(ic->slot);
  if (ic->kind == ICON_CAPP) return capprun_def(ic->slot);
  return NULL;
}

/* Every entry has an icon, so the launcher never has to fall back to drawing a
 * letter in a box. A loaded app supplies its own; a built-in is matched by
 * name; anything else gets the generic page. */
const uint8_t *icon_bitmap(int i) {
  const Icon *ic = icon_at(i);
  if (!ic) return NULL;

  if (ic->kind == ICON_CAPP) {
    const uint8_t *b = capprun_icon(ic->slot);
    return b ? b : ICON_GENERIC;
  }
  if (ic->kind == ICON_FIRMWARE) return ICON_FIRMWARE_;

  if (strcmp(ic->name, "Files") == 0)    return ICON_FILES;
  if (strcmp(ic->name, "Memory") == 0)   return ICON_MEMORY;
  if (strcmp(ic->name, "Settings") == 0) return ICON_SETTINGS;
  if (strcmp(ic->name, "About") == 0)    return ICON_ABOUT;
  return ICON_GENERIC;
}

int icons_boot_firmware(int i) {
  const Icon *ic = icon_at(i);
  AppImageInfo info;
  AppImageResult why;

  if (!ic || ic->kind != ICON_FIRMWARE) return 0;

  /* Remember that CardOS launched it, so that when the bootloader rolls back
   * after the guest is reset we come straight back to the shell the user was
   * in rather than to a console they never asked for. */
  desktop_set_autostart(1);
  if (launcher_check(ic->path, &info, &why) != LAUNCH_OK) {
    desktop_set_autostart(0);
    return 0;
  }
  launcher_boot(ic->path, NULL, NULL);    /* does not return on success */
  desktop_set_autostart(0);
  return 0;
}

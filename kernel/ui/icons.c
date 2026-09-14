/* The contents of /desktop. See icons.h. */

#include "kernel/ui/icons.h"
#include "kernel/fs/fs.h"
#include "kernel/app/capprun.h"
#include "kernel/app/capp_blobs.h"
#include "kernel/app/launcher.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/icons_builtin.h"
#include "kernel/ui/icons_color.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

/* One colour icon, 16x16 RGB565. Loaded on demand and kept: 512 bytes each,
 * and only for entries that actually have a file. */
#define CIC_PIXELS (16 * 16)
#define CIC_HEADER 8

static Icon s_icon[MAX_ICONS];
static int  s_nicon;      /* everything found */
static int  s_nvisible;   /* everything with an icon, sorted to the front */

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

/* Make room for a blob whose name carries a folder ("Games/mines.capp"), and
 * bring any copy left at the top level in with it.
 *
 * The apps were all seeded flat before they were grouped, so a card that has
 * been booted on an older firmware still holds /desktop/mines.capp. Left
 * there it is a second, identical icon -- the launcher scans the top level
 * and one level down, and would find both.
 *
 * Moved rather than deleted, when there is nowhere to move it to: `update
 * apps` may have put a newer build on the card than the one this firmware
 * carries, and that is exactly the copy worth keeping. If both exist the flat
 * one is the leftover, and goes. */
static void settle_folder(const char *rel) {
  const char *slash = strrchr(rel, '/');
  char dir[80], flat[80], full[80];

  if (!slash) return;                       /* top level: nothing to do */

  snprintf(dir, sizeof dir, "%s/%.*s", ICONS_DIR, (int)(slash - rel), rel);
  if (fs_mkdir(dir) != 0) { /* already there, or no card */ }

  snprintf(flat, sizeof flat, "%s/%s", ICONS_DIR, slash + 1);
  if (file_size(flat) < 0) return;

  snprintf(full, sizeof full, "%s/%s", ICONS_DIR, rel);
  if (file_size(full) >= 0) fs_remove(flat);
  else if (fs_rename(flat, full) != 0) fs_remove(flat);
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
    /* Before the stamp is consulted: a card seeded by an older firmware has a
     * matching stamp and the file in the wrong place, and the early `continue`
     * below would leave it there. */
    settle_folder(CAPP_BLOBS[i].name);
    snprintf(path, sizeof path, "%s/%s", ICONS_DIR, CAPP_BLOBS[i].name);

    /* The stamp says which firmware wrote these, and it is only written once
     * every blob has been measured back at its full size, so a matching stamp
     * means the copies were good. A file that has since gone is rewritten; one
     * whose size differs is left alone, because that is what a remote update
     * looks like -- `update apps` puts a newer pinball.capp on the card than
     * the one this firmware carries, and it must survive the next boot. (The
     * size used to be checked too, from when the stamp was written even after
     * a failure and edit.capp stayed truncated at 650 bytes across reboots.) */
    if (have == want && file_size(path) >= 0) continue;

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

/* The colour icons, written out beside the apps. Same reasoning as the
 * binaries: a file the user can only get onto the card with a card reader is a
 * file they will not have. Overwritten whenever the size differs, which is the
 * cheap half of the check the .capp seeding does. */
static void seed_colour_icons(void) {
  size_t i;
  char path[96];
  int fd;

  if (fs_mkdir(ICON_DIR) != 0) { /* already there, or no card */ }

  for (i = 0; i < CIC_BLOB_COUNT; i++) {
    snprintf(path, sizeof path, "%s/%s", ICON_DIR, CIC_BLOBS[i].name);
    if (file_size(path) == (int)CIC_BLOBS[i].size) continue;

    fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) continue;
    if (write_all(fd, CIC_BLOBS[i].data, CIC_BLOBS[i].size) != 0) {
      fs_close(fd);
      fs_remove(path);
      continue;
    }
    fs_close(fd);
  }
}

/* One example program for the IDE, so it opens on something that runs.
 *
 * Same reasoning as the .capp blobs above: a file that can only arrive by
 * card reader is a file the user does not have, and an assembler with an
 * empty buffer teaches nobody the syntax. Written once and never again --
 * unlike the apps, this is a document, and a version the user has edited is
 * worth more than the one shipped. */
static void seed_example(void) {
  static const char PROG[] =
  "; Sum 1..100 and print it.\n"\
    ";\n"\
    "; ctrl-b runs this on the VM, ctrl-l compiles it to real Xtensa\n"\
    "; and runs that, ctrl-x does both and checks they agree.\n"\
    "\n"\
    "        movi r0, 0        ; total\n"\
    "        movi r1, 1        ; i\n"\
    "        movi r2, 101      ; limit\n"\
    "        movi r3, 1\n"\
    "loop:   add  r0, r0, r1\n"\
    "        add  r1, r1, r3\n"\
    "        blt  r1, r2, loop\n"\
    "        sys  1            ; print r0  -- expect 5050\n"\
    "        halt\n";

  int fd;
  if (fs_mkdir(ASM_DIR) != 0) { /* already there, or no card */ }
  fd = fs_open(ASM_DIR "/sum.s", FS_O_READ);
  if (fd >= 0) { fs_close(fd); return; }
  fd = fs_open(ASM_DIR "/sum.s", FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return;
  write_all(fd, (const uint8_t *)PROG, sizeof PROG - 1);
  fs_close(fd);
}

/* Seed the folder the first time, so a fresh card still has something to click
 * rather than an empty desktop with no clue what to do. Each entry is created
 * only if absent rather than only on a wholly empty folder: a card that
 * already holds one icon should still get the rest. */
static void seed_dir(void) {
  /* Files.app is gone: the file manager is a .capp now, seeded like the
   * others. Settings is still built in, so it still needs a stub. */
  static const char *seed[] = { "Settings.app" };
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
  seed_colour_icons();
  seed_example();
}

static int ends_with(const char *name, size_t n, const char *ext) {
  size_t e = strlen(ext);
  return n > e && strcmp(name + n - e, ext) == 0;
}

/* Folders are one level deep. Their names are collected while the parent is
 * being read and scanned afterwards, so there is never more than one
 * directory open on the card at a time. */
#define MAX_FOLDERS 8

/* Scan one folder. `bin_only` for /firmware, which holds images and nothing
 * this launcher can run any other way. `parent` is the flat index of the
 * folder entry this is the inside of, or -1 at the top. */
static void scan(const char *dir, int bin_only, int parent) {
  FsDir d;
  FsEntry e;
  char folders[MAX_FOLDERS][20];
  int nfolders = 0, i;

  if (fs_opendir(dir, &d) != 0) return;

  while (s_nicon < MAX_ICONS && fs_readdir(&d, &e) == 1) {
    size_t n = strlen(e.name);
    Icon *ic = &s_icon[s_nicon];
    if (e.name[0] == '.') continue;      /* the stamp file, and anything like it */

    if (e.is_dir) {
      /* A folder at the top is an entry; one inside a folder is not, and
       * neither is the colour-icon store. */
      if (bin_only || parent >= 0 || strcmp(e.name, "icons") == 0) continue;
      if (nfolders < MAX_FOLDERS && n < sizeof folders[0])
        memcpy(folders[nfolders++], e.name, n + 1);
      continue;
    }

    /* Cleared before it is filled. The colour pointer in particular is freed
     * by the next reload, and leaving a stale one here meant the second reload
     * freed it twice -- which aborts in the allocator with a backtrace that
     * points at free() and says nothing about icons. */
    memset(ic, 0, sizeof *ic);
    ic->parent = parent;

    snprintf(ic->path, sizeof ic->path, "%s/%s", dir, e.name);

    if (ends_with(e.name, n, ".bin")) {
      ic->kind = ICON_FIRMWARE;
      ic->slot = -1;
      ic->cli = 0;
      snprintf(ic->name, sizeof ic->name, "%.*s", (int)(n - 4), e.name);
    } else if (bin_only) {
      continue;
    } else if (ends_with(e.name, n, ".capp")) {
      /* Loaded eagerly and left loaded: the label under the icon and the icon
       * itself are the app's own, and asking it is the only way to know
       * them. */
      /* Loaded, but not run: the name and icon come from the descriptor,
       * which is exactly why the descriptor exists. Running a program to find
       * out what it is called is the wrong way round. */
      ic->slot = capprun_load(ic->path);
      if (ic->slot < 0) continue;
      ic->kind = ICON_CAPP;
      ic->cli = capprun_is_cli(ic->slot);
      snprintf(ic->name, sizeof ic->name, "%s", capprun_name(ic->slot));
    } else if (ends_with(e.name, n, ".app")) {
      char stem[20];
      ic->cli = 0;
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

  for (i = 0; i < nfolders && s_nicon < MAX_ICONS; i++) {
    Icon *ic = &s_icon[s_nicon];
    memset(ic, 0, sizeof *ic);
    ic->kind = ICON_FOLDER;
    ic->slot = -1;
    ic->parent = -1;
    snprintf(ic->name, sizeof ic->name, "%s", folders[i]);
    snprintf(ic->path, sizeof ic->path, "%s/%s", dir, folders[i]);
    s_nicon++;
    scan(ic->path, 0, s_nicon - 1);
  }
}

/* /firmware, behind a folder of its own.
 *
 * The images used to be scanned straight onto the top level, which put a
 * 1.4 MB chain-boot image next to Pinball -- and once the apps were grouped,
 * they were the only loose things left on the desktop. They are also the one
 * kind of entry here that does not launch an app but replaces the whole
 * operating system, which is worth a deliberate step into a folder.
 *
 * The entry is made first and withdrawn if nothing turned up, because the
 * count is only knowable by scanning: a card with no images should show no
 * folder rather than an empty one. /firmware itself is left alone -- the
 * images were there before CardOS was, put there by the tools that built
 * them, and this only changes where they appear. */
static void scan_firmware_folder(void) {
  Icon *ic;
  int before;

  if (s_nicon >= MAX_ICONS) return;

  ic = &s_icon[s_nicon];
  memset(ic, 0, sizeof *ic);
  ic->kind = ICON_FOLDER;
  ic->slot = -1;
  ic->parent = -1;
  snprintf(ic->name, sizeof ic->name, "%s", "Firmware");
  snprintf(ic->path, sizeof ic->path, "%s", FIRMWARE_DIR);
  s_nicon++;

  before = s_nicon;
  scan(FIRMWARE_DIR, 1, s_nicon - 1);
  if (s_nicon == before) s_nicon--;         /* nothing in it: take it back */
}

/* Stable partition: visible entries keep their order, commands move to the
 * end keeping theirs. The carousel can then walk 0..icons_count()-1 with no
 * gaps, and a lookup still sees everything. Parents are flat indices, so
 * they are remapped through the same move. */
static void partition_cli(void) {
  Icon tmp[MAX_ICONS];
  int  newpos[MAX_ICONS];
  int i, n = 0;

  for (i = 0; i < s_nicon; i++) if (!s_icon[i].cli) { newpos[i] = n; tmp[n++] = s_icon[i]; }
  s_nvisible = n;
  for (i = 0; i < s_nicon; i++) if (s_icon[i].cli)  { newpos[i] = n; tmp[n++] = s_icon[i]; }
  for (i = 0; i < s_nicon; i++) {
    if (tmp[i].parent >= 0) tmp[i].parent = newpos[tmp[i].parent];
    s_icon[i] = tmp[i];
  }
}

void icons_reload(void) {
  int i;
  for (i = 0; i < s_nicon; i++) {
    free(s_icon[i].colour);
    s_icon[i].colour = NULL;
    s_icon[i].colour_tried = 0;
  }
  s_nicon = 0;
  s_nvisible = 0;
  capprun_unload_all();
  if (!fs_mounted()) return;

  seed_dir();
  scan(ICONS_DIR, 0, -1);
  scan_firmware_folder();
  partition_cli();
}

int icons_count(void) { return s_nvisible; }
int icons_total(void) { return s_nicon; }

const Icon *icon_at(int i) {
  if (i < 0 || i >= s_nicon) return NULL;
  return &s_icon[i];
}

int icons_in_count(int folder) {
  int i, n = 0;
  for (i = 0; i < s_nvisible; i++) n += s_icon[i].parent == folder;
  return n;
}

const Icon *icons_in_at(int folder, int i) {
  int k;
  for (k = 0; k < s_nvisible; k++) {
    if (s_icon[k].parent != folder) continue;
    if (i-- == 0) return &s_icon[k];
  }
  return NULL;
}

int icon_index(const Icon *ic) {
  if (!ic || ic < s_icon || ic >= s_icon + s_nicon) return -1;
  return (int)(ic - s_icon);
}

/* For a built-in, the definition is compiled in. For a loaded program there is
 * nothing until it has run and installed an interface -- which is the point:
 * a program is not an app until it says it is. */
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
  if (ic->kind == ICON_FOLDER)   return ICON_FOLDER_;

  if (strcmp(ic->name, "Files") == 0)    return ICON_FILES;
  if (strcmp(ic->name, "Memory") == 0)   return ICON_MEMORY;
  if (strcmp(ic->name, "Settings") == 0) return ICON_SETTINGS;
  if (strcmp(ic->name, "About") == 0)    return ICON_ABOUT;
  return ICON_GENERIC;
}

/* Read NAME.cic, if there is one. The header is checked rather than trusted:
 * this is a file on a removable card, and anything at all can be in it. */
static uint16_t *load_cic(const char *name) {
  char path[96];
  uint8_t head[CIC_HEADER];
  uint16_t *px;
  int fd, want = CIC_PIXELS * 2, got = 0;

  snprintf(path, sizeof path, "%s/%s.cic", ICON_DIR, name);
  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return NULL;

  if (fs_read(fd, head, CIC_HEADER) != CIC_HEADER ||
      head[0] != 'C' || head[1] != 'I' || head[2] != 'C' || head[3] != '1' ||
      head[4] != 16 || head[5] != 0 || head[6] != 16 || head[7] != 0) {
    fs_close(fd);
    return NULL;
  }

  px = (uint16_t *)malloc((size_t)want);
  if (!px) { fs_close(fd); return NULL; }
  while (got < want) {
    int n = fs_read(fd, (uint8_t *)px + got, (size_t)(want - got));
    if (n <= 0) break;
    got += n;
  }
  fs_close(fd);
  if (got != want) { free(px); return NULL; }
  return px;
}

const uint16_t *icon_colour(int i) {
  Icon *ic;
  if (i < 0 || i >= s_nicon) return NULL;
  ic = &s_icon[i];
  if (ic->colour_tried) return ic->colour;
  ic->colour_tried = 1;
  if (ic->kind == ICON_FOLDER) {
    /* Folders look their icon up in a namespace of their own. The card is
     * FAT, which matches names without regard to case, so a folder called
     * Firmware and the chip drawn for a firmware *image* would otherwise be
     * competing for one file called firmware.cic. */
    char key[32];
    snprintf(key, sizeof key, "folder-%s", ic->name);
    ic->colour = load_cic(key);
    /* No generic fallback: generic.cic is a blank page -- an app with nothing
     * drawn for it -- and folders wearing it would look like apps, and like
     * each other. Without a colour they fall through to ICON_FOLDER_, which
     * is at least the right shape. */
    return ic->colour;
  }
  ic->colour = load_cic(ic->kind == ICON_FIRMWARE ? "firmware" : ic->name);
  if (!ic->colour && ic->kind != ICON_FIRMWARE) ic->colour = load_cic("generic");
  return ic->colour;
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

/* CardOS filesystem API.
 *
 * A thin CardOS-shaped layer over FAT32, so the implementation can be replaced
 * without touching callers. Paths are CardOS-absolute ("/cardos/apps"); the
 * backing mount point is an implementation detail.
 *
 * The card stays plain FAT32 and readable on any PC, which is the whole point
 * of choosing it.
 */
#ifndef CARDOS_FS_H
#define CARDOS_FS_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/path.h"

#define FS_O_READ   0x01
#define FS_O_WRITE  0x02
#define FS_O_CREATE 0x04
#define FS_O_APPEND 0x08
#define FS_O_TRUNC  0x10

#define FS_SEEK_SET 0
#define FS_SEEK_CUR 1
#define FS_SEEK_END 2

#define FS_NAME_MAX 63
#define FS_MAX_OPEN 8

typedef struct {
  uint32_t size;
  int      is_dir;
  uint32_t mtime;      /* seconds since the epoch, 0 if the card does not say */
} FsStat;

typedef struct {
  char     name[FS_NAME_MAX + 1];
  uint32_t size;
  int      is_dir;
  uint32_t mtime;
} FsEntry;

/* Mount the SD card. Returns 0 on success. A missing or unformatted card is a
 * normal condition, not a failure to boot -- CardOS runs without one, just
 * without swap or apps. */
int  fs_mount(void);
int  fs_mounted(void);
void fs_unmount(void);

/* Total and free bytes on the card. Zero if not mounted. */
void fs_space(uint64_t *total, uint64_t *freebytes);

int  fs_open(const char *path, int flags);      /* fd, or -1 */
int  fs_read(int fd, void *buf, size_t n);      /* bytes read, or -1 */
int  fs_write(int fd, const void *buf, size_t n);
int  fs_seek(int fd, int32_t off, int whence);  /* new position, or -1 */
void fs_close(int fd);

int  fs_stat(const char *path, FsStat *out);

/* Directory iteration.
 *
 * Prefer this to fs_list. A caller of fs_list has to hold the array, and an
 * FsEntry is 72 bytes: the shell's `ls` was measured at 2464 bytes of stack
 * against task stacks the spec sets at 1 KB. Iterating costs one entry. It
 * also removes the arbitrary cap -- fs_list silently truncated a directory
 * with more files than the caller guessed. */
/* The directory being read, and where it is, so that two tasks can each be
 * in the middle of a listing. Was a static, which meant one listing at a
 * time for the whole machine. */
typedef struct {
  void *impl;
  char  prefix[FS_PATH_MAX + 8];
} FsDir;

int  fs_opendir(const char *path, FsDir *d);
int  fs_readdir(FsDir *d, FsEntry *out);   /* 1 = entry, 0 = end, -1 = error */
void fs_closedir(FsDir *d);

/* Batch convenience over the iterator; the spec names it. Truncates at max. */
int  fs_list(const char *dir, FsEntry *out, int max);   /* count, or -1 */
int  fs_mkdir(const char *path);
int  fs_remove(const char *path);
int  fs_rename(const char *from, const char *to);

/* Told the path of anything written, created, removed or renamed (both
 * names), after it happened -- or, for an open to write, as it is opened.
 * One listener: the app index (kernel/app/capprun.c), which must forget
 * what it knows about /apps the moment anything there changes, whoever
 * changed it -- `update apps`, Share, Build, Files. NULL to stop. */
void fs_on_change(void (*fn)(const char *path));

/* Create the directory layout (capp.h: /sys, /config, /cache, /home, /apps,
 * /var). Idempotent. */
int  fs_ensure_layout(void);

/* Move a card laid out the old way into the new one: /desktop to /apps,
 * /cardos to /sys, the keys into /config, the caches into /cache, and so on.
 * Each move happens only when the old path exists and the new one does not,
 * so it is safe to run every boot and does nothing on a card already moved.
 * Returns how many paths it moved. */
int  fs_migrate_layout(void);

#endif /* CARDOS_FS_H */

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
} FsStat;

typedef struct {
  char     name[FS_NAME_MAX + 1];
  uint32_t size;
  int      is_dir;
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
int  fs_list(const char *dir, FsEntry *out, int max);   /* count, or -1 */
int  fs_mkdir(const char *path);
int  fs_remove(const char *path);
int  fs_rename(const char *from, const char *to);

/* Create the directory layout the spec fixes: /cardos, /cardos/apps,
 * /cardos/src, /home. Idempotent. */
int  fs_ensure_layout(void);

#endif /* CARDOS_FS_H */

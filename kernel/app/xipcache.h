/* The flash cache of relocated app code.
 *
 * A ring of 4 KB sectors over an abstract flash (device: the appcode
 * partition, xipflash.c; host: an array). An entry is a 128-byte header and
 * an app's code, relocated for one (map base, arena address) pair, in whole
 * sectors. New entries go at the head; the erase evicts whatever was there,
 * oldest-written first, so every sector is erased once a lap and nothing
 * needs an allocator. See
 * docs/superpowers/specs/2026-10-09-xip-app-code-design.md.
 *
 * Crash safety is in the order of writes: erase, code, header, and last the
 * commit word. Anything without a commit is free space. There is no index
 * in RAM: a lookup walks the headers, at most 128 small reads. */
#ifndef CARDOS_XIPCACHE_H
#define CARDOS_XIPCACHE_H

#include <stdint.h>

#define XIP_SECTOR    4096u
#define XIP_HDR       128u          /* code starts here in an entry */
#define XIP_MAGIC     0x43504958u   /* "XIPC" */
#define XIP_PATH_MAX  24
#define XIP_INUSE_MAX 4             /* entries mapped by loaded apps at once */

enum { XIP_OK = 0, XIP_MISS = 1,
       XIP_ERR_IO = -1, XIP_ERR_FULL = -2, XIP_ERR_BUSY = -3, XIP_ERR_STATE = -4 };

typedef struct {
  int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
  int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t n);  /* only clears bits */
  int (*erase)(void *ctx, uint32_t off, uint32_t n);                   /* whole sectors */
  void    *ctx;
  uint32_t size;                    /* a multiple of XIP_SECTOR */
} XipFlash;

/* What an entry is valid for. All of it must match. */
typedef struct {
  uint32_t path_hash, file_size, file_mtime;   /* the .capp, as appidx keys it */
  uint32_t api, code_size;
  uint32_t map_base, arena;                    /* what it was relocated against */
} XipKey;

typedef struct {                    /* on flash, XIP_HDR bytes */
  uint32_t magic, seq, sectors;
  XipKey   key;
  uint32_t crc;                     /* of the code as written */
  char     path[XIP_PATH_MAX];      /* the tail of the path, for listings */
  uint32_t reserved[13];            /* left erased */
  uint32_t live;                    /* 0xFFFFFFFF; 0 kills it, no erase needed */
  uint32_t commit;                  /* XIP_MAGIC, written last */
} XipHdr;

typedef struct { uint32_t off, len; uint16_t refs; } XipUse;

typedef struct {
  XipFlash f;
  uint32_t head, next_seq;
  uint32_t pend_off, pend_len;      /* begun, not committed; len 0 for none */
  XipUse   use[XIP_INUSE_MAX];
} XipCache;

/* Bind to a flash and find the head (the end of the highest seq). */
int  xip_open(XipCache *c, const XipFlash *f);
/* A live, committed entry matching k whose code still checks: XIP_OK and
 * its offset, else XIP_MISS. */
int  xip_find(XipCache *c, const XipKey *k, uint32_t *off);
/* Room for code_size bytes at the head (or at 0, if the head is too near
 * the end), erased. XIP_ERR_FULL if it could never fit, XIP_ERR_BUSY if it
 * would erase an entry in use. One at a time. */
int  xip_begin(XipCache *c, uint32_t code_size, uint32_t *off);
/* Code bytes at `at` into the begun entry. */
int  xip_write(XipCache *c, uint32_t at, const void *buf, uint32_t n);
/* Header, then the commit word; then every other live entry for the same
 * path is killed. */
int  xip_commit(XipCache *c, const XipKey *k, uint32_t crc, const char *path);
void xip_abandon(XipCache *c);
/* A loaded app maps the entry at off: never erase it until unref. */
int  xip_ref(XipCache *c, uint32_t off);
void xip_unref(XipCache *c, uint32_t off);
/* The .capp at this path changed: kill its live entries. Its code stays
 * where it is, so an app running from one carries on. */
int  xip_forget(XipCache *c, uint32_t path_hash);
/* Erase everything. XIP_ERR_BUSY while anything is in use. */
int  xip_wipe(XipCache *c);
/* Every committed entry, live or dead, in flash order. */
int  xip_each(XipCache *c, void (*fn)(const XipHdr *h, uint32_t off, int in_use, void *ctx),
              void *ctx);

uint32_t xip_crc32(uint32_t crc, const void *p, uint32_t n);   /* crc32(0, ...) to start */
/* FNV-1a of the path, letters folded: FAT does not care about case. */
uint32_t xip_path_hash(const char *path);

#endif /* CARDOS_XIPCACHE_H */

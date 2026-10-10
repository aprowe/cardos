/* The flash cache of relocated app code. See xipcache.h. */

#include "kernel/app/xipcache.h"

#include <stddef.h>
#include <string.h>

typedef char xip_hdr_is_128_bytes[sizeof(XipHdr) == XIP_HDR ? 1 : -1];

#define LIVE 0xFFFFFFFFu

uint32_t xip_crc32(uint32_t crc, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  int i;
  crc = ~crc;
  while (n--) {
    crc ^= *b++;
    for (i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

uint32_t xip_path_hash(const char *path) {
  uint32_t h = 0x811C9DC5u;
  for (; path && *path; path++) {
    char ch = *path;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    h = (h ^ (uint8_t)ch) * 0x01000193u;
  }
  return h;
}

static int overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return a < b + blen && b < a + alen;
}

static int committed(const XipCache *c, const XipHdr *h, uint32_t off) {
  return h->magic == XIP_MAGIC && h->commit == XIP_MAGIC && h->sectors > 0 &&
         h->sectors <= (c->f.size - off) / XIP_SECTOR &&
         XIP_HDR + h->key.code_size <= h->sectors * XIP_SECTOR;
}

/* Every committed entry in flash order; fn returns nonzero to stop, and
 * that is what walk returns. An uncommitted sector is stepped over one at a
 * time, which is how a torn write or the tail of an evicted entry reads. */
static int walk(XipCache *c, int (*fn)(XipCache *, const XipHdr *, uint32_t, void *),
                void *ctx) {
  uint32_t off = 0;
  XipHdr h;
  int r;
  while (off < c->f.size) {
    if (c->f.read(c->f.ctx, off, &h, sizeof h) != 0) return XIP_ERR_IO;
    if (committed(c, &h, off)) {
      if ((r = fn(c, &h, off, ctx)) != 0) return r;
      off += h.sectors * XIP_SECTOR;
    } else {
      off += XIP_SECTOR;
    }
  }
  return 0;
}

static int kill(XipCache *c, uint32_t off) {
  uint32_t zero = 0;
  return c->f.write(c->f.ctx, off + (uint32_t)offsetof(XipHdr, live), &zero, 4) == 0
         ? 0 : XIP_ERR_IO;
}

static int in_use(const XipCache *c, uint32_t off) {
  int i;
  for (i = 0; i < XIP_INUSE_MAX; i++)
    if (c->use[i].refs && c->use[i].off == off) return 1;
  return 0;
}

struct newest { uint32_t seq, end; int any; };
static int find_newest(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  struct newest *n = (struct newest *)ctx;
  (void)c;
  if (!n->any || h->seq > n->seq) {
    n->any = 1;
    n->seq = h->seq;
    n->end = off + h->sectors * XIP_SECTOR;
  }
  return 0;
}

int xip_open(XipCache *c, const XipFlash *f) {
  struct newest n;
  memset(c, 0, sizeof *c);
  c->f = *f;
  memset(&n, 0, sizeof n);
  if (walk(c, find_newest, &n) < 0) return XIP_ERR_IO;
  c->head = n.any && n.end < c->f.size ? n.end : 0;
  c->next_seq = n.any ? n.seq + 1 : 1;
  return XIP_OK;
}

static int code_crc(XipCache *c, uint32_t off, uint32_t n, uint32_t *crc) {
  uint8_t buf[256];
  uint32_t at = 0, k, x = 0;
  while (at < n) {
    k = n - at < sizeof buf ? n - at : (uint32_t)sizeof buf;
    if (c->f.read(c->f.ctx, off + XIP_HDR + at, buf, k) != 0) return -1;
    x = xip_crc32(x, buf, k);
    at += k;
  }
  *crc = x;
  return 0;
}

struct lookup { const XipKey *k; uint32_t off; };
static int match(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  struct lookup *l = (struct lookup *)ctx;
  uint32_t crc;
  if (h->live != LIVE || memcmp(&h->key, l->k, sizeof *l->k) != 0) return 0;
  if (code_crc(c, off, h->key.code_size, &crc) != 0 || crc != h->crc) return 0;
  l->off = off;
  return 1;
}

int xip_find(XipCache *c, const XipKey *k, uint32_t *off) {
  struct lookup l;
  l.k = k;
  l.off = 0;
  if (walk(c, match, &l) != 1) return XIP_MISS;
  *off = l.off;
  return XIP_OK;
}

int xip_begin(XipCache *c, uint32_t code_size, uint32_t *off) {
  uint32_t len, start;
  int i;
  if (c->pend_len) return XIP_ERR_STATE;
  if (code_size == 0 || code_size > c->f.size) return XIP_ERR_FULL;
  len = (XIP_HDR + code_size + XIP_SECTOR - 1) / XIP_SECTOR * XIP_SECTOR;
  if (len > c->f.size) return XIP_ERR_FULL;
  /* The head is the end of the newest entry, or 0: every entry with a
   * header before it ends at or before it, so an erase from here never
   * leaves a header standing over a hole. The soak test holds that. */
  start = c->head + len > c->f.size ? 0 : c->head;
  for (i = 0; i < XIP_INUSE_MAX; i++)
    if (c->use[i].refs && overlap(c->use[i].off, c->use[i].len, start, len))
      return XIP_ERR_BUSY;
  if (c->f.erase(c->f.ctx, start, len) != 0) return XIP_ERR_IO;
  c->pend_off = start;
  c->pend_len = len;
  *off = start;
  return XIP_OK;
}

int xip_write(XipCache *c, uint32_t at, const void *buf, uint32_t n) {
  uint32_t room;
  if (!c->pend_len) return XIP_ERR_STATE;
  room = c->pend_len - XIP_HDR;
  if (at > room || n > room - at) return XIP_ERR_STATE;
  return c->f.write(c->f.ctx, c->pend_off + XIP_HDR + at, buf, n) == 0 ? XIP_OK : XIP_ERR_IO;
}

struct same { uint32_t hash, self; };
static int kill_same(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  const struct same *s = (const struct same *)ctx;
  if (off == s->self || h->live != LIVE || h->key.path_hash != s->hash) return 0;
  return kill(c, off);
}

int xip_commit(XipCache *c, const XipKey *k, uint32_t crc, const char *path) {
  XipHdr h;
  uint32_t magic = XIP_MAGIC;
  struct same s;
  size_t n;
  if (!c->pend_len || XIP_HDR + k->code_size > c->pend_len) return XIP_ERR_STATE;
  memset(&h, 0xFF, sizeof h);              /* what is not written stays erased */
  h.magic = XIP_MAGIC;
  h.seq = c->next_seq;
  h.sectors = c->pend_len / XIP_SECTOR;
  h.key = *k;
  h.crc = crc;
  memset(h.path, 0, sizeof h.path);
  if (path) {                              /* the tail: the name is at the end */
    n = strlen(path);
    if (n > XIP_PATH_MAX - 1) path += n - (XIP_PATH_MAX - 1);
    memcpy(h.path, path, strlen(path));
  }
  if (c->f.write(c->f.ctx, c->pend_off, &h, sizeof h) != 0) return XIP_ERR_IO;
  if (c->f.write(c->f.ctx, c->pend_off + (uint32_t)offsetof(XipHdr, commit), &magic, 4) != 0)
    return XIP_ERR_IO;
  s.hash = k->path_hash;
  s.self = c->pend_off;
  c->head = c->pend_off + c->pend_len;
  if (c->head >= c->f.size) c->head = 0;
  c->next_seq++;
  c->pend_len = 0;
  return walk(c, kill_same, &s) < 0 ? XIP_ERR_IO : XIP_OK;
}

void xip_abandon(XipCache *c) { c->pend_len = 0; }

int xip_ref(XipCache *c, uint32_t off) {
  XipHdr h;
  int i, slot = -1;
  for (i = 0; i < XIP_INUSE_MAX; i++) {
    if (c->use[i].refs && c->use[i].off == off) { c->use[i].refs++; return XIP_OK; }
    if (!c->use[i].refs && slot < 0) slot = i;
  }
  if (slot < 0) return XIP_ERR_BUSY;
  if (c->f.read(c->f.ctx, off, &h, sizeof h) != 0 || !committed(c, &h, off))
    return XIP_ERR_STATE;
  c->use[slot].off = off;
  c->use[slot].len = h.sectors * XIP_SECTOR;
  c->use[slot].refs = 1;
  return XIP_OK;
}

void xip_unref(XipCache *c, uint32_t off) {
  int i;
  for (i = 0; i < XIP_INUSE_MAX; i++)
    if (c->use[i].refs && c->use[i].off == off) { c->use[i].refs--; return; }
}

static int kill_hash(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  if (h->live != LIVE || h->key.path_hash != *(const uint32_t *)ctx) return 0;
  return kill(c, off);
}

int xip_forget(XipCache *c, uint32_t path_hash) {
  return walk(c, kill_hash, &path_hash) < 0 ? XIP_ERR_IO : XIP_OK;
}

int xip_wipe(XipCache *c) {
  int i;
  for (i = 0; i < XIP_INUSE_MAX; i++) if (c->use[i].refs) return XIP_ERR_BUSY;
  if (c->f.erase(c->f.ctx, 0, c->f.size) != 0) return XIP_ERR_IO;
  c->head = 0;
  c->next_seq = 1;
  c->pend_len = 0;
  return XIP_OK;
}

struct each { void (*fn)(const XipHdr *, uint32_t, int, void *); void *ctx; };
static int each_one(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  struct each *e = (struct each *)ctx;
  e->fn(h, off, in_use(c, off), e->ctx);
  return 0;
}

int xip_each(XipCache *c, void (*fn)(const XipHdr *h, uint32_t off, int in_use, void *ctx),
             void *ctx) {
  struct each e;
  e.fn = fn;
  e.ctx = ctx;
  return walk(c, each_one, &e) < 0 ? XIP_ERR_IO : XIP_OK;
}

/* App data overlays. See ovl.h. */

#include "kernel/app/ovl.h"

#include <stdio.h>
#include <string.h>

void ovl_path(char *out, size_t cap, uint32_t load, int n) {
  snprintf(out, cap, OVL_DIR "/%lu-%d.bin", (unsigned long)load, n);
}

void ovl_init(Ovl *o, uint8_t *region, const uint32_t size[CAPP_OVL_MAX], int n, uint32_t load) {
  int k;
  memset(o, 0, sizeof *o);
  o->region = region;
  o->n = n < 0 ? 0 : n > CAPP_OVL_MAX ? CAPP_OVL_MAX : n;
  for (k = 0; k < o->n; k++) o->size[k] = size ? size[k] : 0;
  o->live = o->n > 0 ? 0 : -1;
  o->load = load;
}

int ovl_live(const Ovl *o) { return o ? o->live : -1; }

int ovl_switch(Ovl *o, const OvlIo *io, int n, int flags) {
  char path[OVL_PATH_MAX];
  unsigned bit;
  if (!o || !io || o->n == 0 || n < 0 || n >= o->n) return -1;
  if (n == o->live) return 0;
  /* The outgoing one first, at its own size, not the region's. If it cannot
   * be kept, nothing has changed, and the app is told. */
  if (o->live >= 0) {
    bit = 1u << o->live;
    ovl_path(path, sizeof path, o->load, o->live);
    if (flags & CAPP_OVL_DISCARD) {
      if (o->saved & bit) io->remove(io->ctx, path);
      o->saved &= ~bit;
    } else if (o->size[o->live]) {
      if (io->save(io->ctx, path, o->region, o->size[o->live]) != 0) return -1;
      o->saved |= bit;
    }
  }
  /* Then the incoming one: back from its file, or zeros on a first visit.
   * A file that will not read is zeros as well, and -1: the mode is empty,
   * and the app hears so rather than finding out. */
  o->live = n;
  bit = 1u << n;
  if (o->size[n] == 0) return 0;
  if (o->saved & bit) {
    ovl_path(path, sizeof path, o->load, n);
    if (io->load(io->ctx, path, o->region, o->size[n]) == 0) return 0;
    memset(o->region, 0, o->size[n]);
    return -1;
  }
  memset(o->region, 0, o->size[n]);
  return 0;
}

void ovl_drop(Ovl *o, const OvlIo *io) {
  char path[OVL_PATH_MAX];
  int k;
  if (!o) return;
  for (k = 0; k < CAPP_OVL_MAX; k++) {
    if (!(o->saved & (1u << k))) continue;
    ovl_path(path, sizeof path, o->load, k);
    if (io) io->remove(io->ctx, path);
  }
  o->saved = 0;
  o->live = -1;
}

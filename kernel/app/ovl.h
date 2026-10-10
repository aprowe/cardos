/* App data overlays: which one is live, and where the others are kept.
 * Portable so the host suite can reach it; capprun.c binds it to the card
 * and to the app running. See
 * docs/superpowers/specs/2026-10-10-app-data-overlays-design.md.
 *
 * An overlay is zeroed state only, so there is no starting image to load:
 * what is kept is what the app left there, and a first visit is zeros.
 * `saved` remembers which overlays have a file, so a first visit and a
 * DISCARD touch the card not at all -- which is also why DISCARD works
 * with no card. */
#ifndef CARDOS_OVL_H
#define CARDOS_OVL_H

#include <stddef.h>
#include <stdint.h>
#include "kernel/app/capp.h"

#define OVL_DIR      "/cache/ovl"
#define OVL_PATH_MAX 40

typedef struct {
  /* n bytes to path, replacing it: 0, or -1 (no card, card full). */
  int  (*save)(void *ctx, const char *path, const void *buf, uint32_t n);
  /* exactly n bytes from path: 0, or -1 (missing, another size, unreadable). */
  int  (*load)(void *ctx, const char *path, void *buf, uint32_t n);
  void (*remove)(void *ctx, const char *path);
  void *ctx;
} OvlIo;

typedef struct {
  uint8_t  *region;                 /* every overlay lives here while live */
  uint32_t  size[CAPP_OVL_MAX];     /* each one's own size; 0 if not declared */
  int       n;                      /* highest declared + 1; 0 for none */
  int       live;                   /* -1 for none */
  uint32_t  load;                   /* this load's id, in the file names */
  unsigned  saved;                  /* bit k: overlay k has a file */
} Ovl;

/* Overlay 0 live -- the loader zeroed the block, so it is zeroed -- or none
 * when n is 0. */
void ovl_init(Ovl *o, uint8_t *region, const uint32_t size[CAPP_OVL_MAX], int n, uint32_t load);
/* See CardApi.overlay: 0; -1 with nothing changed when the save failed;
 * -1 with n live and zeroed when the read failed; -1 for a bad n. */
int  ovl_switch(Ovl *o, const OvlIo *io, int n, int flags);
int  ovl_live(const Ovl *o);
/* The app is going: its files go too, and nothing is live. */
void ovl_drop(Ovl *o, const OvlIo *io);
/* "/cache/ovl/<load>-<n>.bin": the dash keeps load 1, overlay 12 apart from
 * load 11, overlay 2. */
void ovl_path(char *out, size_t cap, uint32_t load, int n);

#endif /* CARDOS_OVL_H */

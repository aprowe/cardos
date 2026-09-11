/* The contract between CardOS and a loadable app (.capp).
 *
 * This header is shared: CardOS includes it, and so does every app, which is
 * why it must not depend on anything else in the tree.
 *
 * An app links against nothing. Everything it may do arrives as a table of
 * function pointers handed to its entry point, so there are no undefined
 * symbols to resolve and the interface versions cleanly -- CardOS refuses a
 * binary built against an API version it no longer provides.
 */
#ifndef CARDOS_CAPP_H
#define CARDOS_CAPP_H

#include <stdint.h>
#include <stddef.h>

#define CAPP_API_VERSION 5
#define CAPP_ICON_W 16
#define CAPP_ICON_H 16
#define CAPP_ICON_BYTES ((CAPP_ICON_W / 8) * CAPP_ICON_H)   /* 1bpp, 32 bytes */

typedef struct { int16_t x, y, w, h; } CRect;

/* Colours are RGB565 already byte-swapped for the panel; an app should use
 * CAPP_RGB rather than composing them by hand. */
#define CAPP_RGB(r, g, b)                                                   \
  ((uint16_t)(((((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))) >> 8) | \
              ((((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))) << 8)))

#define CAPP_BLACK  CAPP_RGB(0, 0, 0)
#define CAPP_WHITE  CAPP_RGB(255, 255, 255)
#define CAPP_GREY   CAPP_RGB(128, 128, 128)
#define CAPP_FACE   CAPP_RGB(192, 192, 192)
#define CAPP_NAVY   CAPP_RGB(0, 0, 128)
#define CAPP_RED    CAPP_RGB(200, 0, 0)
#define CAPP_GREEN  CAPP_RGB(0, 140, 0)

/* Mouse buttons, as they reach click(). */
#define CAPP_BTN_LEFT  0x01
#define CAPP_BTN_RIGHT 0x02

/* Keys an app may see beyond ordinary ASCII. */
#define CAPP_KEY_UP    0x80
#define CAPP_KEY_DOWN  0x81
#define CAPP_KEY_LEFT  0x82
#define CAPP_KEY_RIGHT 0x83
#define CAPP_KEY_ENTER 0x0D
#define CAPP_KEY_BACK  0x08

/* File open flags, matching the kernel's. */
#define CAPP_O_READ   0x01
#define CAPP_O_WRITE  0x02
#define CAPP_O_CREATE 0x04
#define CAPP_O_TRUNC  0x10

/* Everything an app is allowed to do. Grows only by appending, with
 * api_version bumped -- never by reordering. */
typedef struct {
  uint16_t version;

  /* Drawing. All coordinates are content-relative; the clip is already set to
   * whatever part of the window is actually visible. */
  void (*fill)(CRect r, uint16_t colour);
  void (*frame)(CRect r, uint16_t colour);
  void (*bevel)(CRect r, uint16_t face, uint16_t tl, uint16_t br);
  void (*text)(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg);
  void (*pixels)(CRect r, const uint16_t *rgb565);   /* raw blit */

  /* Files. */
  int  (*open)(const char *path, int flags);
  int  (*read)(int fd, void *buf, size_t n);
  int  (*write)(int fd, const void *buf, size_t n);
  int  (*seek)(int fd, int32_t off, int whence);
  void (*close)(int fd);
  int  (*list)(const char *dir, char *out, int max_entries, int name_len);

  /* Odds and ends an app cannot get from a libc it is not linked against. */
  void *(*mem_set)(void *d, int c, size_t n);
  void *(*mem_cpy)(void *d, const void *s, size_t n);
  void *(*mem_move)(void *d, const void *s, size_t n);
  size_t (*str_len)(const char *s);
  int  (*fmt)(char *buf, size_t n, const char *fmt, ...);
  uint32_t (*ticks_ms)(void);
  void (*log)(const char *msg);

  /* --- version 2 ---------------------------------------------------------
   * The network. One-shot and blocking: an app here has a few kilobytes to
   * spare and no business holding a connection open across paint calls.
   * Returns bytes written, or negative (-1 means there is no network). */
  int (*http_get)(const char *url, char *buf, size_t size, int timeout_ms);
  int (*net_ready)(void);

  /* Bring up the network the user last joined. Blocking, up to timeout_ms;
   * returns 0 on success. An app that needs the web calls this rather than
   * telling the user to go to Settings and come back. */
  int (*net_connect)(int timeout_ms);
} CardApi;

/* What an app hands back. The callbacks mirror the built-in AppDef, so a
 * loaded app and a compiled-in one are the same thing to the window system. */
typedef struct {
  uint16_t api_version;                  /* must equal CAPP_API_VERSION */
  char     name[16];
  uint8_t  icon[CAPP_ICON_BYTES];        /* 16x16, 1bpp, bit 7 = leftmost */
  int      fullscreen;                   /* 1 to take the whole screen */

  void (*paint)(void *state, CRect content);
  int  (*key)(void *state, uint8_t k);
  int  (*click)(void *state, int16_t x, int16_t y, int button);
  void (*open)(void *state);

  /* Optional: the arguments the app was started with, as one string --
   * "run edit /notes.txt" reaches Edit as "/notes.txt". Called *after* open(),
   * because open() resets the app and would otherwise throw the arguments
   * away. Not called at all when there are none, so an app can tell "started
   * bare" from "started with an empty string".
   *
   * One string rather than argv: an app that wants words can split them, and
   * every app that has wanted arguments so far wanted exactly one path. */
  void (*set_args)(void *state, const char *args);

  /* Natural height at this width, for contents that do not fit the window.
   * NULL means it always fits. */
  int16_t (*height)(void *state, int16_t width);

  /* Preferred content size; 0 for whatever the desktop hands out. */
  int16_t pref_w, pref_h;

  /* --- version 3 ---------------------------------------------------------
   * Does this app want typed characters right now? When it does not, the
   * shell turns ; . , / into arrows so moving a selection needs no Fn key.
   * NULL means it never takes text. */
  int (*wants_text)(void *state);

  /* --- version 4 ---------------------------------------------------------
   * The app's keys, one per line as "key<tab>meaning", shown by ctrl-h. NULL
   * if it has none worth listing. */
  const char *help;

  void *state;
} CappApp;

/* The single exported symbol. The loader finds it by name, calls it with the
 * API table, and gets everything else back. */
const CappApp *capp_register(const CardApi *api);

#endif /* CARDOS_CAPP_H */

/* The contract between CardOS and a loadable app (.capp).
 *
 * This header is shared: CardOS includes it, and so does every app, which is
 * why it must not depend on anything else in the tree.
 *
 * A .capp is a program, in the ordinary sense. It exports two things:
 *
 *   capp_info   a const descriptor -- name, icon, flags -- that the loader
 *               reads *without running anything*. A launcher needs an app's
 *               name to draw its icon, and executing a program to find out
 *               what it is called is the wrong way round.
 *
 *   capp_main   the program: argc, argv, an API table, and an exit status.
 *
 * A command-line tool does its work in capp_main, reads with api->in_line,
 * writes with api->out, and returns. Nothing takes the screen and the prompt
 * comes straight back, which is what typing a command should do.
 *
 * A graphical app calls api->ui() from capp_main to install its event
 * handlers, and then returns. The shell keeps the handlers and calls them; the
 * app's globals stay alive because its image stays loaded. It is a callback
 * table rather than an event loop of its own because the desktop hosts four
 * windows at once, and four programs cannot each be blocked in their own loop
 * on a machine with one stack.
 *
 * That split is the whole design: one entry point, argc/argv and stdio like
 * any other program, and drawing is something a program asks for rather than a
 * different kind of program.
 *
 * An app links against nothing. Everything it may do arrives in the API table,
 * so there are no undefined symbols to resolve and the interface versions
 * cleanly -- CardOS refuses a binary built against a version it no longer
 * provides.
 */
#ifndef CARDOS_CAPP_H
#define CARDOS_CAPP_H

#include <stdint.h>
#include <stddef.h>

#define CAPP_API_VERSION 16

#define CAPP_ICON_W 16
#define CAPP_ICON_H 16
#define CAPP_ICON_BYTES ((CAPP_ICON_W / 8) * CAPP_ICON_H)   /* 1bpp, 32 bytes */

#define CAPP_MAX_ARGS 8

/* capp_info flags. */
#define CAPP_CLI        0x0001   /* a command: no icon, runs and returns */
#define CAPP_FULLSCREEN 0x0002   /* opens filling the screen */

/* What the app needs to be useful. Declaring nothing means standalone, and
 * standalone is the default on purpose: Mines and Pinball work on a device
 * that has never seen a network, and should keep working when the laptop is
 * off.
 *
 * The OS acts on these before capp_main runs -- joining WiFi, checking the
 * proxy answers -- so an app is not left rendering a half-state while a radio
 * negotiates. It does not refuse to start an app whose needs are unmet: Web
 * still has its cached page and Claude still has its log. api->caps_ok() says
 * what was actually found, so an app can explain itself in its own words. */
#define CAPP_NEEDS_NET   0x0004  /* the internet, over WiFi */
#define CAPP_NEEDS_PROXY 0x0008  /* tools/webproxy.py on a PC; implies NET */

/* What caps_ok() returns: the same bits, set when that need was met. */
#define CAPP_CAP_NET     CAPP_NEEDS_NET
#define CAPP_CAP_PROXY   CAPP_NEEDS_PROXY

typedef struct { int16_t x, y, w, h; } CRect;

#define CAPP_NAME_MAX 63

/* One directory entry, as list_ex hands it over. Fixed-width on purpose: an
 * app has no allocator, so the caller holds the array. */
typedef struct {
  char     name[CAPP_NAME_MAX + 1];
  uint32_t size;
  int      is_dir;
} CappEntry;

typedef struct {
  uint32_t size;
  int      is_dir;
} CappStat;

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

/* What a graphical app installs. Everything is optional except paint. */
typedef struct {
  void (*paint)(void *state, CRect content);
  int  (*key)(void *state, uint8_t k);
  int  (*click)(void *state, int16_t x, int16_t y, int button);

  /* Natural height at this width, for contents taller than the window. The
   * shell scrolls the difference and draws a scrollbar. */
  int16_t (*height)(void *state, int16_t width);

  /* Does the app want typed characters right now? When it does not, the shell
   * turns ; . , / into arrows so moving a selection needs no Fn key. */
  int (*wants_text)(void *state);

  /* Preferred content size; 0 for whatever the shell hands out. */
  int16_t pref_w, pref_h;

  void *state;

  /* The pointer moved, a button is down, or the wheel turned. x and y are
   * content-relative and are valid even when no button is pressed, which is
   * the difference between this and click(): a click is an event, and this is
   * where the mouse *is*.
   *
   * buttons is a mask of CAPP_BTN_*, held rather than edge-triggered. wheel is
   * notches since the last call, positive away from the user.
   *
   * Return 1 to ask for a repaint. An app that only wants clicks can leave
   * this NULL and lose nothing; the shell keeps drawing the pointer either
   * way, so every app has a mouse whether or not it reads one.
   *
   * The wheel reaches a windowed app through here only if the app takes it --
   * returning 0 lets the window scroll instead, which is what an app with
   * contents taller than its window wants. */
  int (*mouse)(void *state, int16_t x, int16_t y, int buttons, int wheel);

  /* Called every pass of the shell's loop -- about every 5 ms -- with the
   * clock in milliseconds. Return 1 to ask for a repaint.
   *
   * This is what lets a program move without being pushed. Everything else
   * here answers an event; a game, a clock, anything with its own time needs
   * a moment that belongs to nobody. It is not a thread: it runs on the
   * shell's stack, between keypresses, and a tick that takes 20 ms makes the
   * whole machine take 20 ms. Do a frame's worth of work and return.
   *
   * NULL for the overwhelming majority of apps, which change only when
   * something is pressed. */
  int (*tick)(void *state, uint32_t now_ms);
} CappUi;

/* Everything an app is allowed to do. Grows only by appending, with
 * CAPP_API_VERSION bumped -- never by reordering. */
typedef struct {
  uint16_t version;

  /* ---- drawing. Only meaningful from a ui callback; coordinates are
   * content-relative and the clip is already the visible part. ---- */
  void (*fill)(CRect r, uint16_t colour);
  void (*frame)(CRect r, uint16_t colour);
  void (*bevel)(CRect r, uint16_t face, uint16_t tl, uint16_t br);
  void (*text)(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg);
  void (*pixels)(CRect r, const uint16_t *rgb565);   /* raw blit */

  /* ---- files ---- */
  int  (*open)(const char *path, int flags);
  int  (*read)(int fd, void *buf, size_t n);
  int  (*write)(int fd, const void *buf, size_t n);
  int  (*seek)(int fd, int32_t off, int whence);
  void (*close)(int fd);
  int  (*list)(const char *dir, char *out, int max_entries, int name_len);

  /* What `list` cannot say: which entries are directories and how big the
   * files are. Added when the file manager needed to draw a folder
   * differently from a file, which is the first question anyone asks of a
   * listing. Returns the count, or negative. */
  int  (*list_ex)(const char *dir, CappEntry *out, int max_entries);
  int  (*stat)(const char *path, CappStat *out);

  /* Making, unmaking and moving. rename moves too -- on FAT a rename across
   * directories is a move, and there is no copy here because a copy of a file
   * larger than the heap is a loop the caller would have to write anyway. */
  int  (*mkdir)(const char *path);
  int  (*remove)(const char *path);
  int  (*rename)(const char *from, const char *to);

  /* Start another program, by the name the launcher knows it by, with one
   * string of arguments -- `run("edit", "/notes.txt")`. Returns 0 if it
   * started. This is how a file manager opens a file in an editor without
   * containing an editor. */
  int  (*run)(const char *name, const char *args);

  /* ---- odds and ends an app cannot get from a libc it is not linked
   * against ---- */
  void *(*mem_set)(void *d, int c, size_t n);
  void *(*mem_cpy)(void *d, const void *s, size_t n);
  void *(*mem_move)(void *d, const void *s, size_t n);
  size_t (*str_len)(const char *s);
  int  (*fmt)(char *buf, size_t n, const char *fmt, ...);
  uint32_t (*ticks_ms)(void);
  void (*log)(const char *msg);

  /* ---- standard output and input ----
   *
   * Where these go is the shell's business, not the app's: the console, a
   * file, or the next program in a pipeline. */
  void (*out)(const char *s);
  void (*out_line)(const char *s);              /* adds the newline */
  int  (*in_line)(char *buf, size_t n);         /* -1 at end of input */
  int  (*has_input)(void);

  /* ---- the network. One-shot and blocking: an app here has a few kilobytes
   * to spare and no business holding a connection open. ---- */
  int (*http_get)(const char *url, char *buf, size_t size, int timeout_ms);
  int (*net_ready)(void);
  int (*net_connect)(int timeout_ms);

  /* Why the network is or is not up, as a sentence. An app that reports
   * "error -1" has told the user nothing they can act on. */
  const char *(*net_status)(void);

  /* Straight to a file, for a body too large to hold -- a rendered page is
   * 200 KB and the heap is 150. Returns bytes written, or negative. */
  int (*http_download)(const char *url, const char *path, int timeout_ms);

  /* The general form. method is "GET", "POST", "PATCH", "PUT" or "DELETE";
   * body and content_type are NULL for a request without one; bearer is an
   * OAuth access token, or NULL. The reply body is returned whatever the
   * status, because an API's error is a document explaining itself. */
  int (*http)(const char *method, const char *url,
              const char *body, const char *content_type,
              const char *bearer,
              char *out, size_t out_size, int timeout_ms);

  /* A Google access token, refreshed if needed, or NULL if the device has not
   * been signed in. The sign-in itself happens once on a PC -- see
   * kernel/net/gauth.h for why it cannot happen here. */
  const char *(*google_token)(void);
  const char *(*google_status)(void);

  /* Which of this app's declared needs were met when it started: a mask of
   * CAPP_CAP_*. An app that declared nothing gets 0 and has nothing to ask
   * about. Checked once at launch rather than continuously -- a network that
   * drops later shows up as a failed request, which is where it belongs. */
  int (*caps_ok)(void);

  /* ---- becoming a graphical app ----
   *
   * Called from capp_main. The CappUi must outlive the call -- a static, not
   * a local -- because the shell keeps calling into it long after capp_main
   * has returned. */
  void (*ui)(const CappUi *ui);

  /* ---- updates from the PC. See kernel/net/update.h. ----
   *
   * update_check writes what is stale into `out` as a short list --
   * "pinball, claude, firmware" -- and returns how many things that is, 0 for
   * nothing, negative with the reason in `out` if the proxy could not be
   * asked. update_apply installs the apps and then, if `os` is set and the
   * firmware is stale, the firmware -- which restarts the machine and does
   * not return. Otherwise returns how many apps it installed, with the last
   * message in `out`. Both block for seconds; say so on screen first. */
  int (*update_check)(char *out, size_t n);
  int (*update_apply)(int os, char *out, size_t n);
} CardApi;

/* The descriptor, read by the loader without executing anything. Must be a
 * const object named exactly `capp_info`. */
typedef struct {
  uint16_t api_version;                  /* must equal CAPP_API_VERSION */
  uint16_t flags;
  char     name[16];
  uint8_t  icon[CAPP_ICON_BYTES];        /* 16x16, 1bpp, bit 7 = leftmost */
  const char *help;                      /* "key<tab>meaning" per line */
} CappInfo;

/* The program. argv[0] is the name it was invoked as. */
int capp_main(const CardApi *api, int argc, char **argv);

#endif /* CARDOS_CAPP_H */

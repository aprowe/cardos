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

#define CAPP_API_VERSION 23

/* Local time, broken down, as api->now fills it in. */
typedef struct {
  uint16_t year;                /* 2026, not 126 */
  uint8_t  month;               /* 1-12 */
  uint8_t  day;                 /* 1-31 */
  uint8_t  hour;                /* 0-23 */
  uint8_t  min;
  uint8_t  sec;
  uint8_t  wday;                /* 0 = Sunday */
  uint8_t  synced;              /* 0 none, 1 restored and approximate, 2 from
                                 * the network. Truthy means there is a usable
                                 * date; 2 means the minute can be trusted. */
} CappTime;

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

/* Where the PC helper lives when nothing says otherwise.
 *
 * In the contract header because both sides need it: the kernel reads it when
 * an app declares CAPP_NEEDS_PROXY and when the updater looks for a manifest,
 * and Web and Claude compile it in as their own default. It was written out
 * five separate times, and the failure mode of that is a laptop whose address
 * changed and two of five places still pointing at the old one.
 *
 * The kernel prefers `env PROXY` over this. An app cannot read the environment
 * -- there is no API for it -- so an app that wants to move has its own way
 * of being told: `/url` in Claude, an argument to Web. */
#define CAPP_PROXY_DEFAULT "http://192.168.1.74:8080"

typedef struct { int16_t x, y, w, h; } CRect;

/* One thing an app can be asked to do. See CappUi.actions. */
typedef struct {
  const char *id;      /* stable, machine-readable: "save", "run.vm" */
  const char *label;   /* shown to a person: "Save" */
  const char *menu;    /* the menu it hangs under, or NULL for no menu */
  uint8_t     key;     /* the ctrl chord that runs it, or 0 for none */
  uint8_t     action;  /* the app's own enum value, passed back to `action` */
} CappAction;

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
/* http_poll while the request is still running. */
#define CAPP_HTTP_PENDING (-1000)

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

  /* ---- what this app can be asked to do ----
   *
   * One table, and four things come out of it that used to be written
   * separately and drift apart: the ctrl chords, the menu bar, the help
   * panel, and a list of verbs something other than a finger can invoke.
   *
   * Before this an app said the same facts three times -- capp_info.help as a
   * string, a switch in `key`, and the toolbar's own menu tables -- and
   * nothing checked they agreed.
   *
   * The shell matches a ctrl chord against this BEFORE offering the key to
   * `key`, so an app no longer writes a switch for its own shortcuts. Ctrl
   * belongs entirely to the app now (see the convention in
   * kernel/drv/keyboard.h), so every chord here is safe to claim.
   *
   * `id` is the stable machine-readable name -- "save", "run.vm" -- and is
   * what the console, the voice verbs in kernel/sys/rpc.c, or a model would
   * use. kernel/sys/rpc.c already argues the point: an LLM choosing between a
   * few named verbs is useful, one handed a string to run is not. This is
   * that list of verbs, per app, written once.
   *
   * NULL/0 for an app that has none; it keeps working exactly as before. */
  const CappAction *actions;
  uint8_t           nactions;
  int (*action)(void *state, int action);
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

  /* A response read as it arrives, for a body that does not end.
   *
   * `on_data` is called with each chunk off the socket; return non-zero to
   * stop. Nothing is buffered beyond a chunk, so the stream can be longer
   * than the heap -- which for a screen share is the point.
   *
   * It blocks for as long as the stream runs, which means the shell does too:
   * an app using this owns the machine until it returns. Return from
   * `on_data` promptly, and use key_pending() to notice when the user wants
   * out. */
  int (*http_stream)(const char *url,
                     int (*on_data)(void *ctx, const uint8_t *d, int n),
                     void *ctx, int timeout_ms);

  /* Is a key down at this instant? A peek that consumes nothing -- the press
   * is still delivered to key() afterwards. For deciding to stop something
   * long, since nothing else runs while it does. */
  int (*key_pending)(void);

  /* ---- saying what changed ------------------------------------------
   *
   * The shell repaints an app by calling paint with a clip. Until now that
   * clip was always the app's whole rectangle, so every keypress redrew
   * everything -- and three apps grew their own dirty-tracking to avoid it,
   * each with a slightly different `expect_paint` heuristic for telling
   * "my own repaint" from "the shell repainting me".
   *
   * damage() replaces all of that. An app marks the rectangles it actually
   * changed; the shell unions them and clips the next paint to that. An app
   * that marks nothing gets its whole rectangle, exactly as before, so this
   * costs existing apps nothing.
   *
   * Coordinates are the same ones paint is given -- content-relative, in the
   * rect handed to the last paint. Mark before returning 1 from a handler:
   * the shell reads the accumulated damage when it decides what to clip. */
  void (*damage)(CRect r);

  /* What this paint is actually being asked to repair, in the same
   * coordinates. An app that draws expensive things -- a photograph, a page
   * of text -- can skip whatever falls outside it.
   *
   * Comparing this with the rect paint was given also answers the question
   * the `expect_paint` hacks existed for: if it covers everything, this is a
   * repaint the app did not ask for (a help overlay closing, a window moving)
   * and everything has to be drawn. If it is smaller, it is the app's own
   * damage coming back. */
  CRect (*paint_area)(void);

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

  /* ---- what day it is ----
   *
   * ticks_ms is uptime, which answers "how long since" and nothing else. An
   * app that needs a date has no other way to get one: there is no RTC on
   * this board, and an app links against no libc, so it has neither
   * localtime nor the zone rules to feed it.
   *
   * So the kernel does the conversion. `now` fills in local time per env TZ;
   * `epoch` is UTC seconds, for arithmetic and for talking to a server.
   *
   * Both can be honestly ignorant: the clock comes from NTP and is lost at
   * every power cut. `synced` is 0 and `epoch` returns 0 until it has been
   * set, and an app should say so rather than draw 1 Jan 1970. */
  void (*now)(CappTime *t);
  uint32_t (*epoch)(void);

  /* ---- memory an app can execute ----
   *
   * For a program that writes machine code at runtime. Ordinary allocations
   * cannot be executed: on this chip the same SRAM is reached through an
   * instruction window that only permits aligned 32-bit fetches and a data
   * window that permits byte access, so code has to be WRITTEN through one
   * and RUN through the other. exec_alloc returns the instruction-window
   * pointer -- the one to call -- and exec_writable turns it into the
   * byte-addressable alias of the same memory, which is the one to write
   * through. Writing through the executable pointer faults.
   *
   * This is exactly what the app loader does with a .capp; see
   * kernel/app/elfload.c, and apps/capp.ld for why the split exists at all.
   * There is no cache to flush: internal SRAM is not cached on this part.
   *
   * exec_alloc returns NULL when there is not that much executable RAM left,
   * which is a real possibility -- it is the scarcest memory on the board. */
  void *(*exec_alloc)(size_t n);
  void *(*exec_writable)(void *exec);
  void  (*exec_free)(void *exec);

  /* ---- a request that does not freeze the machine ----
   *
   * `http` above blocks. The shell is one cooperative loop, so for as long as
   * it blocks nothing repaints and no key is read -- twenty seconds of dead
   * machine for one sync. These two do the same work off a task of their own.
   *
   * Start it, return from your handler, and poll from `tick` until the answer
   * arrives. The shell keeps drawing and keeps reading keys throughout, and
   * shows a spinner of its own while a request is in flight, so an app need
   * not draw one.
   *
   *   if (api->http_start("GET", url, 0, 0, tok, 20000) == 0) waiting = 1;
   *   ...
   *   int n = api->http_poll(buf, sizeof buf);
   *   if (n != CAPP_HTTP_PENDING) { waiting = 0; ... }
   *
   * http_start returns 0 if accepted, -1 if a request is already in flight --
   * there is only one, because two TLS sessions do not fit in this heap -- and
   * -2 if there was no memory for the reply. Every string is copied, so none
   * of them need outlive the call.
   *
   * http_poll returns CAPP_HTTP_PENDING while it runs, and otherwise exactly
   * what `http` would have: BYTES on success, negative on failure, an HTTP
   * status negated into it so -403 is a 403. Collecting the answer frees it,
   * so poll until it is not pending and then stop asking. */
  int (*http_start)(const char *method, const char *url, const char *body,
                    const char *content_type, const char *bearer,
                    int timeout_ms);
  int (*http_poll)(char *out, size_t out_size);

  /* ---- the card as a network drive ----
   *
   * WebDAV on port 80 while it is on; see kernel/net/share.h. share_start
   * returns 0 or -1, and share_status says either the URL to type on the PC
   * or why it could not start. The share stops by itself when the app that
   * started it is closed. share_take_log hands back one line per call --
   * "PUT /desktop/x.capp" -- or NULL, for an app that shows what is going
   * on; poll it from tick. */
  int         (*share_start)(void);
  void        (*share_stop)(void);
  const char *(*share_status)(void);
  const char *(*share_take_log)(void);
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

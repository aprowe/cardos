/* Running loaded programs. See capprun.h. */

#include "kernel/app/capprun.h"
#include "kernel/sys/input.h"
#include "kernel/app/elfload.h"
#include "kernel/net/wifi.h"
#include "kernel/net/http.h"
#include "kernel/net/httpq.h"
#include "kernel/net/share.h"
#include "kernel/net/update.h"
#include "kernel/sys/env.h"
#include "kernel/app/cmdline.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

typedef struct {
  LoadedApp la;
  int       used;

  /* What the app said changed since its last paint, in the coordinates its
   * last paint was given. Unioned as it is declared; consumed by the shell,
   * which clips the next paint to it. Invalid means "everything", which is
   * both the default and what an app that never calls damage() always gets. */
  Rect      damage;
  int       has_damage;

  char      name[16];     /* copied out: CappInfo.name need not be terminated */
  char      help[160];

  /* The rest of the descriptor, copied for the same reason the name is: the
   * image it came from is not resident. Twelve apps' icons are 384 bytes;
   * twelve apps' code was 48 KB of executable RAM, which is most of the
   * pool. */
  uint8_t   icon[CAPP_ICON_BYTES];
  uint16_t  flags;
  char      path[80];
  int       loaded;       /* is the image in memory right now */
  /* Set when the icon scan re-ran while a shell still held this app. The
   * scan loaded a fresh copy into another slot; this one lives on only until
   * the shell lets go, and must not be handed out as an app again. */
  int       stale;
  /* A release asked for while one of this slot's own handlers was on the
   * stack -- Files calling run("edit") from its click handler, and the
   * launcher letting go of Files. Freeing the code then would return into
   * it; the trampoline does the release on its way out instead. */
  int       release_pending;

  CappUi    ui;           /* what the program installed, if anything */
  int       has_ui;

  AppDef    def;          /* the same thing, wearing a built-in app's clothes */
} Slot;

static const char *TAG = "capp";


static Slot s_slot[CAPPRUN_MAX];

/* Which slot is running. The api->ui callback has no argument saying who is
 * calling, and cannot: it is called from inside the program, which has no idea
 * it lives in a slot. */
static Slot *s_running;

/* Which slot's callback is executing right now, for the same reason: damage()
 * is called from inside a handler and the program cannot name itself. Set
 * around every trampoline below, cleared after -- a damage() from anywhere
 * else has no owner and is dropped rather than credited to whoever ran last. */
static Slot *s_active;

static CRect to_crect(Rect r) {
  CRect o;
  o.x = r.x; o.y = r.y; o.w = r.w; o.h = r.h;
  return o;
}

/* One set of trampolines for every slot, with the slot as the AppDef's state.
 * Generated thunks per slot would be the alternative, and there is no need. */
/* Every trampoline checks that the image is still there before calling into
 * it. It should be -- a shell releases an app only when it closes it -- but
 * the window that outlives its twin (open the same app twice, close one) and
 * an update that reloads the icons under a running app both used to jump
 * into freed executable RAM, and a check costs nothing. */
#define IMAGE_GONE(s) (!(s)->loaded || !(s)->has_ui)

static void release_slot(Slot *s);

/* The tail of every trampoline: the handler has returned, so a release that
 * was asked for while it ran can happen now.
 *
 * `prev` is what s_active was on the way in, and it goes back, because
 * trampolines nest. A tick that asks for a Google token blocks in
 * http_request, whose busy badge repaints the shell on its way out -- and
 * that repaint is a paint trampoline, inside the tick. Clearing s_active
 * there left the rest of the tick running as nobody: its damage() marks
 * were dropped, and the request it then started had no owner, so the slot
 * could not be disowned when the app closed. That reply sat uncollected and
 * every sync after it was refused until reboot. See httpq.h. */
static void handler_done(Slot *s, Slot *prev) {
  s_active = prev;
  if (s->release_pending && s_active != s) {
    s->release_pending = 0;
    release_slot(s);
  }
}

static void tr_paint(void *state, Rect c) {
  Slot *s = (Slot *)state, *prev;
  if (IMAGE_GONE(s)) return;
  prev = s_active;
  s_active = s;
  /* Painting settles the account: whatever was damaged is being repaired
   * now, and anything the app marks from here on belongs to the next frame. */
  s->has_damage = 0;
  if (s->ui.paint) s->ui.paint(s->ui.state, to_crect(c));
  handler_done(s, prev);
}

/* The action table gets the key before the app's own handler does.
 *
 * That is the point of the table: an app declares "ctrl-s is save" once, and
 * the shortcut, the menu item and the help line all come from the same place.
 * An app with no table is unaffected -- the loop runs zero times and the key
 * goes straight through, which is how every app that has not been converted
 * keeps working. */
static int tr_key(void *state, uint8_t k) {
  Slot *s = (Slot *)state, *prev;
  int r, i;

  if (IMAGE_GONE(s)) return 0;
  /* An app that asked for no auto-repeat never sees one. Consumed rather
   * than declined: a declined key goes on to the shell, and a repeat the
   * app did not want is not the shell's either. */
  if ((s->flags & CAPP_NO_REPEAT) && input_is_repeat()) return 1;
  prev = s_active;
  s_active = s;
  for (i = 0; i < (int)s->ui.nactions; i++) {
    if (!s->ui.actions[i].key || s->ui.actions[i].key != k) continue;
    r = s->ui.action ? s->ui.action(s->ui.state, s->ui.actions[i].action) : 0;
    handler_done(s, prev);
    return r;
  }
  r = s->ui.key ? s->ui.key(s->ui.state, k) : 0;
  handler_done(s, prev);
  return r;
}

/* Run one by name, for anything that is not a finger: the console, a script,
 * a model choosing between the verbs an app actually offers. */
int capprun_action_invoke(const AppDef *a, const char *id) {
  int i, j;
  Slot *prev;
  if (!a || !id) return -1;
  for (i = 0; i < CAPPRUN_MAX; i++) {
    Slot *s = &s_slot[i];
    if (!s->used || (const void *)s != a->state) continue;
    if (IMAGE_GONE(s)) return -1;
    for (j = 0; j < (int)s->ui.nactions; j++) {
      const char *p = s->ui.actions[j].id, *q = id;
      while (*p && *p == *q) { p++; q++; }
      if (*p || *q) continue;
      if (!s->ui.action) return -1;
      prev = s_active;
  s_active = s;
      s->ui.action(s->ui.state, s->ui.actions[j].action);
      handler_done(s, prev);
      return 0;
    }
    return -1;
  }
  return -1;
}

/* The table, for a shell that wants to list it. */
const CappAction *capprun_actions(const AppDef *a, int *n) {
  int i;
  if (n) *n = 0;
  if (!a) return 0;
  for (i = 0; i < CAPPRUN_MAX; i++) {
    Slot *s = &s_slot[i];
    if (!s->used || (const void *)s != a->state) continue;
    if (n) *n = (int)s->ui.nactions;
    return s->ui.actions;
  }
  return 0;
}

static int tr_click(void *state, int16_t x, int16_t y, int button) {
  Slot *s = (Slot *)state, *prev;
  int r;
  if (IMAGE_GONE(s)) return 0;
  prev = s_active;
  s_active = s;
  r = s->ui.click ? s->ui.click(s->ui.state, x, y, button) : 0;
  handler_done(s, prev);
  return r;
}

static int tr_mouse(void *state, int16_t x, int16_t y, int buttons, int wheel) {
  Slot *s = (Slot *)state, *prev;
  int r;
  if (IMAGE_GONE(s)) return 0;
  prev = s_active;
  s_active = s;
  r = s->ui.mouse ? s->ui.mouse(s->ui.state, x, y, buttons, wheel) : 0;
  handler_done(s, prev);
  return r;
}

static int tr_tick(void *state, uint32_t now_ms) {
  Slot *s = (Slot *)state, *prev;
  int r;
  if (IMAGE_GONE(s)) return 0;
  prev = s_active;
  s_active = s;
  r = s->ui.tick ? s->ui.tick(s->ui.state, now_ms) : 0;
  handler_done(s, prev);
  return r;
}

static int16_t tr_height(void *state, int16_t w) {
  Slot *s = (Slot *)state;
  if (IMAGE_GONE(s)) return 0;
  return s->ui.height ? s->ui.height(s->ui.state, w) : 0;
}

static int tr_wants_text(void *state) {
  Slot *s = (Slot *)state;
  if (IMAGE_GONE(s)) return 0;
  return s->ui.wants_text ? s->ui.wants_text(s->ui.state) : 0;
}

/* An app marking what it changed. Unioned, because two marks between paints
 * are one repair. */
void capprun_damage(CRect r) {
  Slot *s = s_active;
  Rect n;

  if (!s || r.w <= 0 || r.h <= 0) return;
  n.x = r.x; n.y = r.y; n.w = r.w; n.h = r.h;

  if (!s->has_damage) { s->damage = n; s->has_damage = 1; return; }
  s->damage = rect_union(s->damage, n);
}

/* What the shell should clip the next paint to, or 0 for "all of it". */
static int tr_take_damage(void *state, Rect *out) {
  Slot *s = (Slot *)state;
  if (!s->has_damage) return 0;
  *out = s->damage;
  return 1;
}

/* Called by the program, through the API table, from inside capp_main. */
void capprun_install_ui(const CappUi *ui) {
  Slot *s = s_running;
  if (!s || !ui) return;

  s->ui = *ui;               /* copied: the program may pass a local */
  s->has_ui = 1;

  s->def.name       = s->name;
  s->def.paint      = ui->paint ? tr_paint : NULL;
  s->def.key        = ui->key ? tr_key : NULL;
  s->def.click      = ui->click ? tr_click : NULL;
  s->def.tick       = ui->tick ? tr_tick : NULL;
  s->def.mouse      = ui->mouse ? tr_mouse : NULL;
  s->def.take_damage = tr_take_damage;
  s->def.open       = NULL;      /* capp_main was the open */
  s->def.state      = s;
  s->def.height     = ui->height ? tr_height : NULL;
  s->def.pref_w     = ui->pref_w;
  s->def.pref_h     = ui->pref_h;
  s->def.wants_text = ui->wants_text ? tr_wants_text : NULL;
  /* The help panel, written from the action table rather than by hand.
   *
   * capp_info.help is a string an app maintains separately from its key
   * switch, which is exactly the kind of pair that drifts -- Edit's said
   * "ctrl-p markdown preview" while the desktop was quietly taking ctrl-P.
   * With a table there is one description, and this renders it. An app with
   * no table keeps its written help. */
  if (ui->actions && ui->nactions) {
    size_t n = 0;
    int i;
    for (i = 0; i < (int)ui->nactions && n < sizeof s->help - 1; i++) {
      const CappAction *a = &ui->actions[i];
      int w;
      if (a->key >= 1 && a->key <= 26)
        w = snprintf(s->help + n, sizeof s->help - n, "ctrl-%c\t%s\n",
                     'a' + a->key - 1, a->label);
      else if (a->key >= 0xE0 && a->key <= 0xF9)         /* fn-p: print */
        w = snprintf(s->help + n, sizeof s->help - n, "fn-%c\t%s\n",
                     'a' + a->key - 0xE0, a->label);
      else continue;                                          /* menu-only */
      /* snprintf says how long the line would have been, not how much it
       * wrote; a label that did not fit is dropped whole rather than
       * counted, so n never runs past the buffer into the icon. */
      if (w < 0 || (size_t)w >= sizeof s->help - n) { s->help[n] = 0; break; }
      n += (size_t)w;
    }
    s->help[n] = 0;
  }
  s->def.help       = s->help[0] ? s->help : NULL;
  s->def.set_args   = NULL;      /* argv was the arguments */
}

/* The command catalog, /cache/commands.txt: one line per command of every
 * app, written during the icon scan while each image is loaded anyway -- the
 * descriptor's pointers are only good then. On the card, not in RAM. See
 * docs/superpowers/specs/2026-09-23-app-commands-design.md. */
static int s_catalog_fd = -1;

void capprun_catalog_begin(void) {
  if (s_catalog_fd >= 0) fs_close(s_catalog_fd);
  s_catalog_fd = fs_open(CAPPRUN_CATALOG, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
}

void capprun_catalog_end(void) {
  if (s_catalog_fd >= 0) fs_close(s_catalog_fd);
  s_catalog_fd = -1;
}

/* The app's name in the catalog is its file's, "todo" for .../todo.capp --
 * what `do todo` types and what the server's catalog, keyed by the same
 * files, calls it. */
static void catalog_add(const char *path, const CappInfo *info) {
  char app[24], line[200];
  const char *base = strrchr(path, '/');
  size_t n;
  int i;
  if (s_catalog_fd < 0 || !info->commands || !info->ncommands) return;
  base = base ? base + 1 : path;
  n = strcspn(base, ".");
  snprintf(app, sizeof app, "%.*s", (int)n, base);
  for (i = 0; i < info->ncommands; i++) {
    if (!cmdline_is_command(&info->commands[i])) continue;
    cmdline_catalog_line(app, &info->commands[i], line, sizeof line - 1);
    n = strlen(line);
    line[n++] = '\n';
    fs_write(s_catalog_fd, line, n);
  }
}

int capprun_load(const char *path) {
  int i;
  Slot *s;

  for (i = 0; i < CAPPRUN_MAX; i++) if (!s_slot[i].used) break;
  if (i == CAPPRUN_MAX) {
    /* Loudly, because the symptom is an app that quietly does not appear. */
    ESP_LOGE(TAG, "no slot for %s: all %d in use, raise CAPPRUN_MAX",
             path, CAPPRUN_MAX);
    return -1;
  }
  s = &s_slot[i];

  memset(s, 0, sizeof *s);
  if (capp_load(path, &s->la) != CAPP_OK) return -1;

  memcpy(s->name, s->la.info->name, sizeof s->name - 1);
  s->name[sizeof s->name - 1] = 0;
  if (s->la.info->help)
    snprintf(s->help, sizeof s->help, "%s", s->la.info->help);
  memcpy(s->icon, s->la.info->icon, sizeof s->icon);
  s->flags = s->la.info->flags;
  snprintf(s->path, sizeof s->path, "%s", path);
  catalog_add(path, s->la.info);

  /* And now give it back.
   *
   * This is called once per .capp by the icon scan, purely to find out what
   * an app is called and what its icon looks like. Keeping the image for that
   * cost about 4 KB of executable RAM each, and with twelve apps on the card
   * the pool was down to 28 KB before anything had been run. The descriptor
   * is copied above; the code is not needed again until someone starts it. */
  capp_unload(&s->la);
  s->loaded = 0;
  s->used = 1;
  return i;
}

/* Bring the image back for a slot that is about to run. */
static int ensure_loaded(Slot *s) {
  CappResult r;
  if (s->loaded) return 0;
  r = capp_load(s->path, &s->la);
  if (r != CAPP_OK) {
    ESP_LOGE(TAG, "%s: %s", s->path, capp_strerror(r));
    return -1;
  }
  s->loaded = 1;
  return 0;
}

/* Let go of one, if nothing is still calling into it. */
static void release_slot(Slot *s) {
  /* A request it started and will never collect goes with it. Left in the
   * queue, that reply refused every sync on the device until the next
   * reboot -- see httpq.h. */
  httpq_abandon(s);
  if (s->loaded) capp_unload(&s->la);
  s->loaded = 0;
  s->has_ui = 0;
  if (s->stale) { s->stale = 0; s->used = 0; }
  share_app_closed(s);
}

const void *capprun_caller(void) {
  return s_active ? s_active : s_running;
}

/* Is a shell still calling into this one? An app that installed an interface
 * and is still in memory is being hosted: capprun_start releases anything
 * that installed nothing, and a shell releases what it hosts when it closes
 * it. The one executing right now counts too, whether or not it has a UI --
 * a command may be the thing asking for the reload. */
static int slot_busy(const Slot *s) {
  return (s->loaded && s->has_ui) || s == s_running || s == s_active;
}

/* A shell saying it has finished with an app -- the window closed, or escape
 * left it. Matched by the AppDef's state pointer, which is the slot; a
 * built-in's AppDef matches nothing here and is left alone. */
void capprun_release(const AppDef *a) {
  int i;
  if (!a) return;
  for (i = 0; i < CAPPRUN_MAX; i++) {
    Slot *s = &s_slot[i];
    if (!s->used || (const void *)s != a->state) continue;
    if (s == s_active || s == s_running) s->release_pending = 1;
    else release_slot(s);
    return;
  }
}

/* Forget every slot, so the icon scan can start again. Except the ones a
 * shell is still hosting, and the one running right now: `update apps` is
 * asked for from inside Build and from the Claude terminal, and the reload
 * that follows used to free the caller's own code and return into it. Those
 * keep their image, marked stale, and go when the shell lets go of them; the
 * scan loads a fresh copy of the same file into another slot, so the icon
 * grid shows the new version while the old one is still on screen. */
void capprun_unload_all(void) {
  int i;
  for (i = 0; i < CAPPRUN_MAX; i++) {
    Slot *s = &s_slot[i];
    if (!s->used) continue;
    if (slot_busy(s)) { s->stale = 1; continue; }
    release_slot(s);
    s->used = 0;
  }
}

/* Split a command line into argv. In place, into a buffer of our own, because
 * the caller's string is not ours to write on. Quotes are honoured so a path
 * with a space in it survives; nothing else is interpreted. */
static int split_args(const char *name, const char *args,
                      char *buf, size_t bufsize, char **argv, int max) {
  int argc = 0;
  size_t n = 0;

  /* argv[0] is the name it was invoked as, as it is everywhere else. */
  argv[argc++] = buf;
  while (name[n] && n < 15 && n + 2 < bufsize) { buf[n] = name[n]; n++; }
  buf[n++] = 0;

  if (!args) return argc;

  while (*args && argc < max && n + 1 < bufsize) {
    char quote = 0;
    while (*args == ' ') args++;
    if (!*args) break;

    if (*args == '"' || *args == '\'') quote = *args++;
    argv[argc++] = buf + n;
    while (*args && n + 1 < bufsize) {
      if (quote ? (*args == quote) : (*args == ' ')) { args++; break; }
      buf[n++] = *args++;
    }
    buf[n++] = 0;
  }
  return argc;
}

/* What the last-started app got. One value, not per-slot: only one program is
 * ever starting at a time, and it is read from inside that program's own
 * capp_main through the API table. */
static int s_caps_ok;

/* Bring up whatever the flags ask for, and report what was achieved. */
static int meet_needs(uint16_t flags) {
  int got = 0;

  if (!(flags & (CAPP_NEEDS_NET | CAPP_NEEDS_PROXY))) return 0;

  if (wifi_is_connected() || wifi_connect_saved(20000) == 0) {
    got |= CAPP_CAP_NET;
  } else {
    ESP_LOGW(TAG, "app needs the network: %s", wifi_status());
    return 0;                       /* no net, so certainly no proxy */
  }

  if (flags & CAPP_NEEDS_PROXY) {
    /* One short request, to the endpoint that exists to answer it. A proxy
     * that is not running is the common case when the laptop is shut, and it
     * is worth knowing at startup rather than halfway through a render. */
    char buf[96];
    char url[160];
    const char *base = env_get("PROXY");
    snprintf(url, sizeof url, "%s/status", base && base[0] ? base : CAPP_PROXY_DEFAULT);
    /* With the shared secret the device already keeps for update, or a
     * remote proxy answers 403 and every app that needs it is refused. */
    const char *t = update_token();
    if (http_request("GET", url, NULL, NULL, *t ? t : NULL, buf, sizeof buf, 4000) >= 0)
      got |= CAPP_CAP_PROXY;
    else ESP_LOGW(TAG, "app needs the proxy, and %s did not answer", url);
  }
  return got;
}

int capprun_start(int slot, const char *name, const char *args) {
  static char argbuf[192];
  char *argv[CAPP_MAX_ARGS];
  int argc, rc;
  Slot *s;
  extern const CardApi *cardos_api(void);

  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return -1;
  s = &s_slot[slot];

  /* Already on screen somewhere? Its globals hold that run's state, and
   * calling capp_main over them is not a second copy of the app, it is the
   * first one with its memory scribbled on. Start from a fresh image. A shell
   * that would rather keep the running one (the desktop raises the existing
   * window) checks before it gets here. The app asking to restart itself
   * from one of its own handlers is refused: its code is on the stack. */
  if (s == s_active || s == s_running || s->stale) return -1;
  if (s->loaded && s->has_ui) release_slot(s);
  s->has_ui = 0;

  argc = split_args(name ? name : s->name, args, argbuf, sizeof argbuf,
                    argv, CAPP_MAX_ARGS);

  /* Meet what the app said it needs, before it runs.
   *
   * Doing it here rather than inside the app's first request is what makes
   * the twenty seconds of joining a network belong to "starting Web" instead
   * of to "Web is broken". Failure is not fatal: caps_ok() reports what was
   * actually found and the app decides what to say about it. */
  if (ensure_loaded(s) != 0) return -1;
  s_caps_ok = meet_needs(s->flags);

  s_running = s;
  rc = s->la.main(cardos_api(), argc, argv);
  s_running = NULL;

  /* A command -- grep, cat -- does its work in capp_main and returns having
   * installed nothing. Nothing will call into it again, so it goes straight
   * back to the pool. A graphical app installed handlers and has to stay. */
  if (!s->has_ui) release_slot(s);
  return rc;
}

int capprun_caps_ok(void) { return s_caps_ok; }

/* The app whose code is on the stack right now: inside capp_main it is the
 * one being started, inside a handler the one the trampoline entered, and a
 * capp_main entered from another app's handler is the innermost. Used as an
 * identity, never dereferenced -- it names the owner of a request, so
 * release_slot can disown it. NULL between handlers. */
const void *capprun_executing(void) {
  return s_running ? (const void *)s_running : (const void *)s_active;
}

/* What to call it in a log line. "app" when no app is on the stack, which
 * means the kernel logged through the same path. */
const char *capprun_executing_name(void) {
  Slot *s = s_running ? s_running : s_active;
  return (s && s->name[0]) ? s->name : "app";
}

int capprun_is_app(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return 0;
  return s_slot[slot].has_ui;
}

const char *capprun_name(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return "";
  return s_slot[slot].name;
}

const uint8_t *capprun_icon(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return NULL;
  return s_slot[slot].icon;
}

int capprun_is_cli(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return 0;
  return (s_slot[slot].flags & CAPP_CLI) != 0;
}

int capprun_fullscreen(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return 0;
  return (s_slot[slot].flags & CAPP_FULLSCREEN) != 0;
}

const AppDef *capprun_def(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return NULL;
  if (!s_slot[slot].has_ui) return NULL;
  return &s_slot[slot].def;
}

uint32_t capprun_exec_free(void) { return capp_exec_free(); }

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
#include "kernel/ui/fontres.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Two tables, because an app on the card and an app in memory are different
 * things with different counts.
 *
 * An Entry is what the icon scan learns about each .capp: its name, icon,
 * flags and path. Every app on the card has one for as long as the scan's
 * list stands, and it is small. A Run is an app actually in memory -- its
 * image, the interface it installed, what it marked damaged -- and there are
 * only ever a few: the one on screen, the desktop's windows, a command.
 *
 * They were one struct, a Slot, and every app on the card paid for a running
 * app's worth of state whether or not it ever ran. The table was sized for
 * the apps that existed, and each time an app arrived past it the last one
 * the scan found silently vanished from the launcher: Pinball at 9, Share at
 * 17, Share again at 21 (2026-09-23). A Slot was 456 bytes; an Entry is
 * about 140 and a Run about 360, so 64 of one and 10 of the other cost less
 * than the 32 Slots they replace. */
typedef struct Run Run;

typedef struct {
  int       used;
  /* Loaded for one run, not by the scan -- launchui_run_path, a command for
   * an app the scan never saw. It goes when its run does, instead of every
   * `./prog` leaving an entry behind until the next reload. */
  int       transient;
  char      name[16];     /* copied out: CappInfo.name need not be terminated */
  /* The rest of the descriptor, copied for the same reason the name is: the
   * image it came from is not resident. Twelve apps' icons are 384 bytes;
   * twelve apps' code was 48 KB of executable RAM, which is most of the
   * pool. */
  uint8_t   icon[CAPP_ICON_BYTES];
  uint16_t  flags;
  char      path[80];
  Run      *run;          /* in memory right now, or NULL */
} Entry;

struct Run {
  int       used;
  /* The entry it was started from. NULL once the icon scan re-ran while a
   * shell still held this app: the scan made a fresh entry for the same
   * file, and this run lives on only until the shell lets go -- it must not
   * be found as that app again. */
  Entry    *entry;
  LoadedApp la;
  int       loaded;       /* is the image in memory right now */

  /* What the app said changed since its last paint, in the coordinates its
   * last paint was given. Unioned as it is declared; consumed by the shell,
   * which clips the next paint to it. Invalid means "everything", which is
   * both the default and what an app that never calls damage() always gets. */
  Rect      damage;
  int       has_damage;

  /* Copied from the entry, which a reload can take away while this runs. */
  char      name[16];
  uint16_t  flags;
  /* Written from the action table, or copied from capp_info.help, when the
   * app installs its interface -- the one moment the image and the table
   * are both certainly there. Only a running app has a help panel to show. */
  char      help[160];

  /* A release asked for while one of this run's own handlers was on the
   * stack -- Files calling run("edit") from its click handler, and the
   * launcher letting go of Files. Freeing the code then would return into
   * it; the trampoline does the release on its way out instead. */
  int       release_pending;

  CappUi    ui;           /* what the program installed, if anything */
  int       has_ui;

  AppDef    def;          /* the same thing, wearing a built-in app's clothes */
};

static const char *TAG = "capp";

static Entry s_entry[CAPPRUN_APPS];
static Run   s_run[CAPPRUN_RUNS];

/* Where the next free-run search starts. Round the pool rather than lowest
 * first, so a run just released is the last to be handed to a different app:
 * a shell that let go of an AppDef should not be holding it any more, but if
 * one were, it finds an empty run (IMAGE_GONE) rather than someone else. */
static int s_run_next;

/* Which run is starting. The api->ui callback has no argument saying who is
 * calling, and cannot: it is called from inside the program, which has no idea
 * it lives in a run. */
static Run *s_running;

/* Which run's callback is executing right now, for the same reason: damage()
 * is called from inside a handler and the program cannot name itself. Set
 * around every trampoline below, cleared after -- a damage() from anywhere
 * else has no owner and is dropped rather than credited to whoever ran last. */
static Run *s_active;

static CRect to_crect(Rect r) {
  CRect o;
  o.x = r.x; o.y = r.y; o.w = r.w; o.h = r.h;
  return o;
}

/* One set of trampolines for every run, with the run as the AppDef's state.
 * Generated thunks per run would be the alternative, and there is no need. */
/* Every trampoline checks that the image is still there before calling into
 * it. It should be -- a shell releases an app only when it closes it -- but
 * the window that outlives its twin (open the same app twice, close one) and
 * an update that reloads the icons under a running app both used to jump
 * into freed executable RAM, and a check costs nothing. */
#define IMAGE_GONE(s) (!(s)->loaded || !(s)->has_ui)

static void release_run(Run *s);

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
static void handler_done(Run *s, Run *prev) {
  s_active = prev;
  if (s->release_pending && s_active != s) {
    s->release_pending = 0;
    release_run(s);
  }
}

static void tr_paint(void *state, Rect c) {
  Run *s = (Run *)state, *prev;
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
  Run *s = (Run *)state, *prev;
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
static Run *run_of(const AppDef *a);

int capprun_action_invoke(const AppDef *a, const char *id) {
  int j;
  Run *s = run_of(a), *prev;
  if (!s || !id || IMAGE_GONE(s)) return -1;
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

/* The table, for a shell that wants to list it. */
const CappAction *capprun_actions(const AppDef *a, int *n) {
  Run *s = run_of(a);
  if (n) *n = 0;
  if (!s) return 0;
  if (n) *n = (int)s->ui.nactions;
  return s->ui.actions;
}

static int tr_click(void *state, int16_t x, int16_t y, int button) {
  Run *s = (Run *)state, *prev;
  int r;
  if (IMAGE_GONE(s)) return 0;
  prev = s_active;
  s_active = s;
  r = s->ui.click ? s->ui.click(s->ui.state, x, y, button) : 0;
  handler_done(s, prev);
  return r;
}

static int tr_mouse(void *state, int16_t x, int16_t y, int buttons, int wheel) {
  Run *s = (Run *)state, *prev;
  int r;
  if (IMAGE_GONE(s)) return 0;
  prev = s_active;
  s_active = s;
  r = s->ui.mouse ? s->ui.mouse(s->ui.state, x, y, buttons, wheel) : 0;
  handler_done(s, prev);
  return r;
}

static int tr_tick(void *state, uint32_t now_ms) {
  Run *s = (Run *)state, *prev;
  int r;
  if (IMAGE_GONE(s)) return 0;
  prev = s_active;
  s_active = s;
  r = s->ui.tick ? s->ui.tick(s->ui.state, now_ms) : 0;
  handler_done(s, prev);
  return r;
}

static int16_t tr_height(void *state, int16_t w) {
  Run *s = (Run *)state;
  if (IMAGE_GONE(s)) return 0;
  return s->ui.height ? s->ui.height(s->ui.state, w) : 0;
}

static int tr_wants_text(void *state) {
  Run *s = (Run *)state;
  if (IMAGE_GONE(s)) return 0;
  return s->ui.wants_text ? s->ui.wants_text(s->ui.state) : 0;
}

/* An app marking what it changed. Unioned, because two marks between paints
 * are one repair. */
void capprun_damage(CRect r) {
  Run *s = s_active;
  Rect n;

  if (!s || r.w <= 0 || r.h <= 0) return;
  n.x = r.x; n.y = r.y; n.w = r.w; n.h = r.h;

  if (!s->has_damage) { s->damage = n; s->has_damage = 1; return; }
  s->damage = rect_union(s->damage, n);
}

/* What the shell should clip the next paint to, or 0 for "all of it". */
static int tr_take_damage(void *state, Rect *out) {
  Run *s = (Run *)state;
  if (!s->has_damage) return 0;
  *out = s->damage;
  return 1;
}

/* Called by the program, through the API table, from inside capp_main. */
void capprun_install_ui(const CappUi *ui) {
  Run *s = s_running;
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
  s->help[0] = 0;
  if (!(ui->actions && ui->nactions) && s->la.info && s->la.info->help)
    snprintf(s->help, sizeof s->help, "%s", s->la.info->help);
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

/* A run for this entry, from the pool. NULL when every run is taken, which
 * means ten apps in memory at once -- said loudly, since the symptom is an
 * app that will not start. */
static Run *run_new(Entry *e) {
  int k;
  for (k = 0; k < CAPPRUN_RUNS; k++) {
    Run *r = &s_run[(s_run_next + k) % CAPPRUN_RUNS];
    if (r->used) continue;
    s_run_next = (int)(r - s_run + 1) % CAPPRUN_RUNS;
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->entry = e;
    memcpy(r->name, e->name, sizeof r->name);
    r->flags = e->flags;
    e->run = r;
    return r;
  }
  ESP_LOGE(TAG, "cannot start %s: all %d runs in use, raise CAPPRUN_RUNS",
           e->name, CAPPRUN_RUNS);
  return NULL;
}

/* The run behind an AppDef a shell holds. Matched by the state pointer, so a
 * built-in's AppDef matches nothing here. */
static Run *run_of(const AppDef *a) {
  int k;
  if (!a) return NULL;
  for (k = 0; k < CAPPRUN_RUNS; k++)
    if (s_run[k].used && (const void *)&s_run[k] == a->state) return &s_run[k];
  return NULL;
}

/* Load, read the descriptor into a new entry, and -- unless `keep` -- give
 * the image back. `keep` is for something about to run the app: loading it,
 * freeing it and loading it again left a gap in the executable pool each
 * time, until a command could not load Calendar with 44 KB free and no single
 * block of 13.9 KB (2026-09-23). A kept image goes straight into a run. */
static int load_entry(const char *path, int transient, int keep) {
  LoadedApp la;
  Entry *e;
  Run *r;
  int i;

  for (i = 0; i < CAPPRUN_APPS; i++) if (!s_entry[i].used) break;
  if (i == CAPPRUN_APPS) {
    /* Loudly, because the symptom is an app that quietly does not appear. */
    ESP_LOGE(TAG, "no entry for %s: all %d in use, raise CAPPRUN_APPS",
             path, CAPPRUN_APPS);
    return -1;
  }
  e = &s_entry[i];

  memset(e, 0, sizeof *e);
  if (capp_load(path, &la) != CAPP_OK) return -1;

  memcpy(e->name, la.info->name, sizeof e->name - 1);
  e->name[sizeof e->name - 1] = 0;
  memcpy(e->icon, la.info->icon, sizeof e->icon);
  e->flags = la.info->flags;
  e->transient = transient;
  snprintf(e->path, sizeof e->path, "%s", path);
  catalog_add(path, la.info);

  /* And now give it back.
   *
   * This is called once per .capp by the icon scan, purely to find out what
   * an app is called and what its icon looks like. Keeping the image for that
   * cost about 4 KB of executable RAM each, and with twelve apps on the card
   * the pool was down to 28 KB before anything had been run. The descriptor
   * is copied above; the code is not needed again until someone starts it. */
  if (keep && (r = run_new(e)) != NULL) {
    r->la = la;
    r->loaded = 1;
  } else {
    capp_unload(&la);
    if (keep) return -1;                 /* no run to keep it in */
  }
  e->used = 1;
  return i;
}

int capprun_load(const char *path) { return load_entry(path, 0, 0); }

/* For one run of a program the scan did not list: loaded now, kept for the
 * start that follows, and gone with that run. */
int capprun_load_once(const char *path) { return load_entry(path, 1, 1); }

/* Bring the image back for a run that is about to start. */
static int ensure_loaded(Run *s) {
  CappResult r;
  if (s->loaded) return 0;
  r = capp_load(s->entry->path, &s->la);
  if (r != CAPP_OK) {
    ESP_LOGE(TAG, "%s: %s", s->entry->path, capp_strerror(r));
    return -1;
  }
  s->loaded = 1;
  return 0;
}

/* Take the program out of memory and forget what it installed, keeping the
 * run: for a restart, where the same AppDef should come back as the same
 * app, as it did when there was only one table. */
static void release_image(Run *s) {
  /* A request it started and will never collect goes with it. Left in the
   * queue, that reply refused every sync on the device until the next
   * reboot -- see httpq.h. */
  httpq_abandon(s);
  fontres_release_owner(s);          /* and the fonts it asked for */
  if (s->loaded) capp_unload(&s->la);
  s->loaded = 0;
  s->has_ui = 0;
  share_app_closed(s);
}

/* Let go of one completely: the image, and the run back to the pool. An entry
 * that existed only for this run goes too. */
static void release_run(Run *s) {
  release_image(s);
  s->release_pending = 0;
  if (s->entry) {
    s->entry->run = NULL;
    if (s->entry->transient) s->entry->used = 0;
  }
  s->entry = NULL;
  s->used = 0;
}

const void *capprun_caller(void) {
  return s_active ? s_active : s_running;
}

/* Is a shell still calling into this one? An app that installed an interface
 * and is still in memory is being hosted: capprun_start releases anything
 * that installed nothing, and a shell releases what it hosts when it closes
 * it. The one executing right now counts too, whether or not it has a UI --
 * a command may be the thing asking for the reload. */
static int run_busy(const Run *s) {
  return (s->loaded && s->has_ui) || s == s_running || s == s_active;
}

/* A shell saying it has finished with an app -- the window closed, or escape
 * left it. */
void capprun_release(const AppDef *a) {
  Run *s = run_of(a);
  if (!s) return;
  if (s == s_active || s == s_running) s->release_pending = 1;
  else release_run(s);
}

/* Forget every entry, so the icon scan can start again. The runs a shell is
 * still hosting, and the one running right now, stay: `update apps` is asked
 * for from inside Build and from the Claude terminal, and the reload that
 * follows used to free the caller's own code and return into it. Those keep
 * their image, detached from any entry, and go when the shell lets go of
 * them; the scan makes a fresh entry for the same file, so the icon grid
 * shows the new version while the old one is still on screen. */
void capprun_unload_all(void) {
  int i;
  for (i = 0; i < CAPPRUN_RUNS; i++) {
    Run *s = &s_run[i];
    if (!s->used) continue;
    if (run_busy(s)) { s->entry = NULL; continue; }
    release_run(s);
  }
  for (i = 0; i < CAPPRUN_APPS; i++) s_entry[i].used = 0;
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

static Entry *entry_at(int slot) {
  if (slot < 0 || slot >= CAPPRUN_APPS || !s_entry[slot].used) return NULL;
  return &s_entry[slot];
}

int capprun_start(int slot, const char *name, const char *args) {
  static char argbuf[192];
  char *argv[CAPP_MAX_ARGS];
  int argc, rc;
  Entry *e = entry_at(slot);
  Run *s;
  extern const CardApi *cardos_api(void);

  if (!e) return -1;
  s = e->run;

  /* Already on screen somewhere? Its globals hold that run's state, and
   * calling capp_main over them is not a second copy of the app, it is the
   * first one with its memory scribbled on. Start from a fresh image, in the
   * same run, so a shell still holding its AppDef finds the new instance. A
   * shell that would rather keep the running one (the desktop raises the
   * existing window) checks before it gets here. The app asking to restart
   * itself from one of its own handlers is refused: its code is on the
   * stack. */
  if (s) {
    if (s == s_active || s == s_running) return -1;
    if (s->loaded && s->has_ui) release_image(s);
  } else if ((s = run_new(e)) == NULL) {
    return -1;
  }
  s->has_ui = 0;

  argc = split_args(name ? name : e->name, args, argbuf, sizeof argbuf,
                    argv, CAPP_MAX_ARGS);

  /* Meet what the app said it needs, before it runs.
   *
   * Doing it here rather than inside the app's first request is what makes
   * the twenty seconds of joining a network belong to "starting Web" instead
   * of to "Web is broken". Failure is not fatal: caps_ok() reports what was
   * actually found and the app decides what to say about it. */
  if (ensure_loaded(s) != 0) { release_run(s); return -1; }
  s_caps_ok = meet_needs(s->flags);

  s_running = s;
  rc = s->la.main(cardos_api(), argc, argv);
  s_running = NULL;

  /* A command -- grep, cat -- does its work in capp_main and returns having
   * installed nothing. Nothing will call into it again, so it goes straight
   * back to the pool. A graphical app installed handlers and has to stay. */
  if (!s->has_ui) release_run(s);
  return rc;
}

/* ---- commands --------------------------------------------------------------
 *
 * One instance per app. If the app is open, the command goes to that
 * instance -- its login, its cache, its list already in memory, nothing
 * loaded twice. If not, it is started headless: the same capp_main, with
 * api->headless() true so it does the local part of starting and skips the
 * screen and the sync, then the command, then the slot goes back. See
 * docs/superpowers/specs/2026-09-23-app-commands-design.md. */

#define CMD_WAIT_MS 20000

static int   s_headless;             /* running an app only for a command */
static int   s_cmd_waiting;          /* a PENDING command has not finished */
static int   s_cmd_rc;
static char *s_cmd_out;
static size_t s_cmd_n;

int capprun_headless(void) { return s_headless; }

static int (*s_opener)(const char *app, const char *args);
void capprun_set_opener(int (*open)(const char *, const char *)) { s_opener = open; }

/* A CAPP_CMD_OPEN command: its arguments, quoted where they have spaces, as
 * the app's command line, and the app opened with it by the shell. */
static int open_with(const char *app, int argc, const char *const *argv,
                     char *out, size_t n) {
  char args[CAPPRUN_CMD_TEXT];
  size_t o = 0;
  int i;
  args[0] = 0;
  for (i = 0; i < argc && o + 4 < sizeof args; i++) {
    int space = strchr(argv[i], ' ') != NULL;
    o += (size_t)snprintf(args + o, sizeof args - o, "%s%s%s%s", o ? " " : "",
                          space ? "\"" : "", argv[i], space ? "\"" : "");
  }
  if (!s_opener || s_opener(app, args) < 0) {
    snprintf(out, n, "could not open %s", app);
    return -1;
  }
  snprintf(out, n, "opened %s%s%s", app, args[0] ? " " : "", args);
  return 0;
}

void capprun_command_done(int rc, const char *out) {
  if (!s_cmd_waiting) return;
  s_cmd_rc = rc;
  if (s_cmd_out && s_cmd_n) snprintf(s_cmd_out, s_cmd_n, "%s", out ? out : "");
  s_cmd_waiting = 0;
}

/* The entry whose file is APP.capp, in whichever folder. */
static Entry *entry_for_app(const char *app) {
  int i;
  size_t n = strlen(app);
  for (i = 0; i < CAPPRUN_APPS; i++) {
    Entry *e = &s_entry[i];
    const char *base;
    if (!e->used) continue;
    base = strrchr(e->path, '/');
    base = base ? base + 1 : e->path;
    if (!strncmp(base, app, n) && !strcmp(base + n, ".capp")) return e;
  }
  return NULL;
}

/* APP.capp on the card: the top of /apps, then each folder in it. The entries
 * only hold what the icon scan loaded, and a card changed since -- or a scan
 * that has not run -- leaves the app out, so a command cannot count on them.
 * 0 and the path, or -1. */
static int find_capp(const char *app, char *out, size_t n) {
  FsDir d;
  FsEntry e;
  FsStat st;
  snprintf(out, n, "%s/%s.capp", CAPP_APPS, app);
  if (fs_stat(out, &st) == 0 && !st.is_dir) return 0;
  if (fs_opendir(CAPP_APPS, &d) != 0) return -1;
  while (fs_readdir(&d, &e) == 1) {
    if (!e.is_dir || e.name[0] == '.') continue;
    snprintf(out, n, "%s/%s/%s.capp", CAPP_APPS, e.name, app);
    if (fs_stat(out, &st) == 0 && !st.is_dir) { fs_closedir(&d); return 0; }
  }
  fs_closedir(&d);
  return -1;
}

int capprun_command(const char *app, const char *cmd, int nwords,
                    const char *const *words, char *out, size_t n) {
  extern const CardApi *cardos_api(void);
  static char join[CAPPRUN_CMD_TEXT];
  const char *argv[CAPP_CMD_ARGS_MAX];
  const CappAction *a;
  Entry *e;
  Run *s, *prev;
  char why[96];
  int headless, argc, rc;

  out[0] = 0;
  e = entry_for_app(app);
  if (!e) {
    /* Not listed: find it on the card, with an entry that goes when the
     * command's run does. Loaded once, into that run. */
    char path[80];
    int i;
    if (find_capp(app, path, sizeof path) != 0 || (i = load_entry(path, 1, 1)) < 0) {
      snprintf(out, n, "no app called %s", app);
      return -1;
    }
    e = &s_entry[i];
  }
  s = e->run;
  if (s && (s == s_active || s == s_running)) {
    snprintf(out, n, "%s is busy", app);         /* its code is on the stack */
    return -1;
  }

  headless = !(s && s->loaded && s->has_ui);
  if (headless) {
    char *mainargv[1];
    if (!s && (s = run_new(e)) == NULL) {
      snprintf(out, n, "could not start %s: too many apps open", app);
      return -1;
    }
    mainargv[0] = s->name;
    s->has_ui = 0;
    if (ensure_loaded(s) != 0) {
      release_run(s);
      snprintf(out, n, "could not load %s: not enough memory for its code", app);
      return -1;
    }
    s_headless = 1;
    s_running = s;
    s->la.main(cardos_api(), 1, mainargv);
    s_running = NULL;
    if (!s->has_ui) {
      release_run(s);
      s_headless = 0;
      snprintf(out, n, "%s has no commands", app);
      return -1;
    }
  }

  a = cmdline_find(s->ui.actions, s->ui.nactions, cmd);
  /* An open-command has no handler -- the OS opens the app instead -- so
   * only the others need one. */
  if (!a || (!s->ui.command && !(a->cmd & CAPP_CMD_OPEN))) {
    snprintf(out, n, "%s has no command '%s'", app, cmd);
    rc = -1;
    goto finish;
  }
  argc = cmdline_bind(a, nwords, words, argv, join, sizeof join, why, sizeof why);
  if (argc < 0) { snprintf(out, n, "%s", why); rc = -1; goto finish; }
  if (a->cmd & CAPP_CMD_OPEN) {
    /* Not the handler: the app on screen with these arguments. The headless
     * copy that read the table goes first, so the one that opens is fresh. */
    if (headless) {
      release_run(s);
      s_headless = 0;
    }
    return open_with(app, argc, argv, out, n);
  }
  if (a->cmd & CAPP_CMD_NET) s_caps_ok = meet_needs(s->flags | CAPP_NEEDS_NET);

  prev = s_active;
  s_active = s;
  s_cmd_out = out;
  s_cmd_n = n;
  rc = s->ui.command(s->ui.state, a->action, argc, argv, out, n);
  if (rc == CAPP_CMD_PENDING) {
    /* It finishes from tick. Ticked here, on this task, as the shell would:
     * a headless app has no shell doing it. The loop is the wait -- a
     * command asked for from the console or by voice is waited on. */
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    s_cmd_waiting = 1;
    while (s_cmd_waiting) {
      uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
      if (now - start > CMD_WAIT_MS) break;
      if (s->ui.tick) s->ui.tick(s->ui.state, now);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_cmd_waiting) {
      s_cmd_waiting = 0;
      snprintf(out, n, "%s %s: no answer in %d s", app, cmd, CMD_WAIT_MS / 1000);
      rc = -1;
    } else {
      rc = s_cmd_rc;
    }
  }
  s_cmd_out = NULL;
  handler_done(s, prev);

finish:
  if (headless) {
    /* The run goes back, and an entry made only for this command with it.
     * Unless a handler above asked for it already: handler_done has done
     * the release, and the run may belong to nobody now. */
    if (s->used && s->entry == e) release_run(s);
    s_headless = 0;
  } else {
    /* The open instance changed under its own screen. No marks means the
     * whole rectangle at the next paint (see `damage` in Run). */
    s->has_damage = 0;
  }
  return rc;
}

int capprun_caps_ok(void) { return s_caps_ok; }

/* The app whose code is on the stack right now: inside capp_main it is the
 * one being started, inside a handler the one the trampoline entered, and a
 * capp_main entered from another app's handler is the innermost. Used as an
 * identity, never dereferenced -- it names the owner of a request, so
 * release_run can disown it. NULL between handlers. */
const void *capprun_executing(void) {
  return s_running ? (const void *)s_running : (const void *)s_active;
}

/* What to call it in a log line. "app" when no app is on the stack, which
 * means the kernel logged through the same path. */
const char *capprun_executing_name(void) {
  Run *s = s_running ? s_running : s_active;
  return (s && s->name[0]) ? s->name : "app";
}

int capprun_is_app(int slot) {
  Entry *e = entry_at(slot);
  return e && e->run && e->run->has_ui;
}

const char *capprun_name(int slot) {
  Entry *e = entry_at(slot);
  return e ? e->name : "";
}

const uint8_t *capprun_icon(int slot) {
  Entry *e = entry_at(slot);
  return e ? e->icon : NULL;
}

int capprun_is_cli(int slot) {
  Entry *e = entry_at(slot);
  return e && (e->flags & CAPP_CLI) != 0;
}

int capprun_fullscreen(int slot) {
  Entry *e = entry_at(slot);
  return e && (e->flags & CAPP_FULLSCREEN) != 0;
}

const AppDef *capprun_def(int slot) {
  Entry *e = entry_at(slot);
  if (!e || !e->run || !e->run->has_ui) return NULL;
  return &e->run->def;
}

uint32_t capprun_exec_free(void) { return capp_exec_free(); }

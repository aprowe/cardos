/* Running loaded programs. See capprun.h. */

#include "kernel/app/capprun.h"
#include "kernel/app/elfload.h"
#include "kernel/net/wifi.h"
#include "kernel/net/http.h"
#include "kernel/sys/env.h"

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
static void tr_paint(void *state, Rect c) {
  Slot *s = (Slot *)state;
  s_active = s;
  /* Painting settles the account: whatever was damaged is being repaired
   * now, and anything the app marks from here on belongs to the next frame. */
  s->has_damage = 0;
  if (s->ui.paint) s->ui.paint(s->ui.state, to_crect(c));
  s_active = NULL;
}

static int tr_key(void *state, uint8_t k) {
  Slot *s = (Slot *)state;
  int r;
  s_active = s;
  r = s->ui.key ? s->ui.key(s->ui.state, k) : 0;
  s_active = NULL;
  return r;
}

static int tr_click(void *state, int16_t x, int16_t y, int button) {
  Slot *s = (Slot *)state;
  int r;
  s_active = s;
  r = s->ui.click ? s->ui.click(s->ui.state, x, y, button) : 0;
  s_active = NULL;
  return r;
}

static int tr_mouse(void *state, int16_t x, int16_t y, int buttons, int wheel) {
  Slot *s = (Slot *)state;
  int r;
  s_active = s;
  r = s->ui.mouse ? s->ui.mouse(s->ui.state, x, y, buttons, wheel) : 0;
  s_active = NULL;
  return r;
}

static int tr_tick(void *state, uint32_t now_ms) {
  Slot *s = (Slot *)state;
  int r;
  s_active = s;
  r = s->ui.tick ? s->ui.tick(s->ui.state, now_ms) : 0;
  s_active = NULL;
  return r;
}

static int16_t tr_height(void *state, int16_t w) {
  Slot *s = (Slot *)state;
  return s->ui.height ? s->ui.height(s->ui.state, w) : 0;
}

static int tr_wants_text(void *state) {
  Slot *s = (Slot *)state;
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
  s->def.help       = s->help[0] ? s->help : NULL;
  s->def.set_args   = NULL;      /* argv was the arguments */
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

  s->used = 1;
  return i;
}

void capprun_unload_all(void) {
  int i;
  for (i = 0; i < CAPPRUN_MAX; i++) {
    if (!s_slot[i].used) continue;
    capp_unload(&s_slot[i].la);
    s_slot[i].used = 0;
    s_slot[i].has_ui = 0;
  }
  s_running = NULL;
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
    if (http_get(url, buf, sizeof buf, 4000) >= 0) got |= CAPP_CAP_PROXY;
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
  s->has_ui = 0;

  argc = split_args(name ? name : s->name, args, argbuf, sizeof argbuf,
                    argv, CAPP_MAX_ARGS);

  /* Meet what the app said it needs, before it runs.
   *
   * Doing it here rather than inside the app's first request is what makes
   * the twenty seconds of joining a network belong to "starting Web" instead
   * of to "Web is broken". Failure is not fatal: caps_ok() reports what was
   * actually found and the app decides what to say about it. */
  s_caps_ok = meet_needs(s->la.info ? s->la.info->flags : 0);

  s_running = s;
  rc = s->la.main(cardos_api(), argc, argv);
  s_running = NULL;
  return rc;
}

int capprun_caps_ok(void) { return s_caps_ok; }

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
  return s_slot[slot].la.info->icon;
}

int capprun_is_cli(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return 0;
  return (s_slot[slot].la.info->flags & CAPP_CLI) != 0;
}

int capprun_fullscreen(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return 0;
  return (s_slot[slot].la.info->flags & CAPP_FULLSCREEN) != 0;
}

const AppDef *capprun_def(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return NULL;
  if (!s_slot[slot].has_ui) return NULL;
  return &s_slot[slot].def;
}

uint32_t capprun_exec_free(void) { return capp_exec_free(); }

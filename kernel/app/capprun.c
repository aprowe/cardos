/* Loaded apps as AppDefs. See capprun.h. */

#include "kernel/app/capprun.h"
#include "kernel/app/elfload.h"

#include <string.h>

typedef struct {
  LoadedApp la;
  AppDef    def;
  char      name[16];
  int       used;
} Slot;

static Slot s_slot[CAPPRUN_MAX];

static CRect to_crect(Rect r) {
  CRect o;
  o.x = r.x; o.y = r.y; o.w = r.w; o.h = r.h;
  return o;
}

/* The slot is the AppDef's state, so one set of trampolines serves every
 * loaded app rather than needing generated thunks per slot. */
static void tr_paint(void *state, Rect c) {
  Slot *s = (Slot *)state;
  if (s->la.app->paint) s->la.app->paint(s->la.app->state, to_crect(c));
}

static int tr_key(void *state, uint8_t k) {
  Slot *s = (Slot *)state;
  return s->la.app->key ? s->la.app->key(s->la.app->state, k) : 0;
}

static int tr_click(void *state, int16_t x, int16_t y, int button) {
  Slot *s = (Slot *)state;
  return s->la.app->click ? s->la.app->click(s->la.app->state, x, y, button) : 0;
}

static int16_t tr_height(void *state, int16_t w) {
  Slot *s = (Slot *)state;
  return s->la.app->height ? s->la.app->height(s->la.app->state, w) : 0;
}

static void tr_set_args(void *state, const char *args) {
  Slot *s = (Slot *)state;
  if (s->la.app->set_args) s->la.app->set_args(s->la.app->state, args);
}

static int tr_wants_text(void *state) {
  Slot *s = (Slot *)state;
  return s->la.app->wants_text ? s->la.app->wants_text(s->la.app->state) : 0;
}

static void tr_open(void *state) {
  Slot *s = (Slot *)state;
  if (s->la.app->open) s->la.app->open(s->la.app->state);
}

int capprun_load(const char *path) {
  int i;
  for (i = 0; i < CAPPRUN_MAX; i++) if (!s_slot[i].used) break;
  if (i == CAPPRUN_MAX) return -1;

  if (capp_load(path, &s_slot[i].la) != CAPP_OK) return -1;

  /* The name is copied out because CappApp.name need not be NUL-terminated if
   * the app filled all 16 bytes, and AppDef.name is a C string. */
  memcpy(s_slot[i].name, s_slot[i].la.app->name, sizeof s_slot[i].name - 1);
  s_slot[i].name[sizeof s_slot[i].name - 1] = 0;

  s_slot[i].def.name  = s_slot[i].name;
  s_slot[i].def.paint = tr_paint;
  s_slot[i].def.key   = tr_key;
  s_slot[i].def.click = tr_click;
  s_slot[i].def.open  = tr_open;
  s_slot[i].def.height = s_slot[i].la.app->height ? tr_height : NULL;
  s_slot[i].def.pref_w = s_slot[i].la.app->pref_w;
  s_slot[i].def.pref_h = s_slot[i].la.app->pref_h;
  s_slot[i].def.wants_text = s_slot[i].la.app->wants_text ? tr_wants_text : NULL;
  /* The string lives in the app's own data allocation and was relocated with
   * everything else, so it can be handed straight over. */
  s_slot[i].def.help = s_slot[i].la.app->help;
  s_slot[i].def.set_args = s_slot[i].la.app->set_args ? tr_set_args : NULL;
  s_slot[i].def.cli = s_slot[i].la.app->cli;
  s_slot[i].def.state = &s_slot[i];
  s_slot[i].used = 1;
  return i;
}

void capprun_unload_all(void) {
  int i;
  for (i = 0; i < CAPPRUN_MAX; i++) {
    if (!s_slot[i].used) continue;
    capp_unload(&s_slot[i].la);
    s_slot[i].used = 0;
  }
}

const AppDef *capprun_def(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return NULL;
  return &s_slot[slot].def;
}

const uint8_t *capprun_icon(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return NULL;
  return s_slot[slot].la.app->icon;
}

int capprun_fullscreen(int slot) {
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return 0;
  return s_slot[slot].la.app->fullscreen ? 1 : 0;
}

void capprun_set_args(int slot, const char *args) {
  const CappApp *a;
  if (slot < 0 || slot >= CAPPRUN_MAX || !s_slot[slot].used) return;
  a = s_slot[slot].la.app;
  if (a->set_args) a->set_args(a->state, args);
}

uint32_t capprun_exec_free(void) { return capp_exec_free(); }

/* Running loaded programs. See capprun.h. */

#include "kernel/app/capprun.h"
#include "kernel/app/elfload.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  LoadedApp la;
  int       used;

  char      name[16];     /* copied out: CappInfo.name need not be terminated */
  char      help[160];

  CappUi    ui;           /* what the program installed, if anything */
  int       has_ui;

  AppDef    def;          /* the same thing, wearing a built-in app's clothes */
} Slot;

static Slot s_slot[CAPPRUN_MAX];

/* Which slot is running. The api->ui callback has no argument saying who is
 * calling, and cannot: it is called from inside the program, which has no idea
 * it lives in a slot. */
static Slot *s_running;

static CRect to_crect(Rect r) {
  CRect o;
  o.x = r.x; o.y = r.y; o.w = r.w; o.h = r.h;
  return o;
}

/* One set of trampolines for every slot, with the slot as the AppDef's state.
 * Generated thunks per slot would be the alternative, and there is no need. */
static void tr_paint(void *state, Rect c) {
  Slot *s = (Slot *)state;
  if (s->ui.paint) s->ui.paint(s->ui.state, to_crect(c));
}

static int tr_key(void *state, uint8_t k) {
  Slot *s = (Slot *)state;
  return s->ui.key ? s->ui.key(s->ui.state, k) : 0;
}

static int tr_click(void *state, int16_t x, int16_t y, int button) {
  Slot *s = (Slot *)state;
  return s->ui.click ? s->ui.click(s->ui.state, x, y, button) : 0;
}

static int16_t tr_height(void *state, int16_t w) {
  Slot *s = (Slot *)state;
  return s->ui.height ? s->ui.height(s->ui.state, w) : 0;
}

static int tr_wants_text(void *state) {
  Slot *s = (Slot *)state;
  return s->ui.wants_text ? s->ui.wants_text(s->ui.state) : 0;
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
  if (i == CAPPRUN_MAX) return -1;
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

  s_running = s;
  rc = s->la.main(cardos_api(), argc, argv);
  s_running = NULL;
  return rc;
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

/* Pet: a virtual pet, after the egg-shaped keyrings of 1996.
 *
 * It lives in wall-clock time (apps/petsim.h): closed, it still gets
 * hungry, bored, tired and messy, and opening the app catches up on the
 * hours you were away. Feed it, give it a snack, play the which-way game,
 * put the lights out when it is tired, clean up after it and give it
 * medicine when it is ill. It hatches, grows through four stages, and the
 * adult it becomes depends on how well it was kept. Left starving and ill
 * for a day, it flies home -- n starts a new egg.
 *
 * A reminder is set with the OS (notify_at) for when it will next be
 * hungry, so the banner says so with the app shut.
 *
 * Keys: left/right pick an action along the bottom, Enter does it; space is
 * the light; f feed, s snack, g game, c clean, m medicine, i info;
 * n new egg (asks). In the game, left or right guesses which way it looks.
 *
 * State is /var/pet/state.txt, key=value lines, saved through safefile.h.
 * Commands: status, feed, clean.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/safefile.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/petsim.h"

static const CardApi *api;

#define STATE_DIR   CAPP_VAR "/pet"
#define STATE_PATH  STATE_DIR "/state.txt"

#define SCALE   3
#define SPR     16
#define BOX_W   (SPR * SCALE + 8)      /* the sprite and a 4 px margin each side */
#define BOX_H   (SPR * SCALE + 4)
/* What a step repaints: the box and the 4 px it may have moved from, twice
 * over, so the room either side is in the same blit and nothing under it
 * is filled first. */
#define STEP_M  8
#define WIDE_W  (BOX_W + 2 * STEP_M)
#define BAR_H   12
#define ROOM_Y  BAR_H
#define ROOM_H  (135 - BAR_H - FOOT_H)
#define PET_Y   (ROOM_Y + 30)
#define STEP_MS 650
#define MSG_MS  2200

#define CLR_ROOM    CAPP_RGB(206, 222, 196)
#define CLR_ROOM_D  CAPP_RGB(30, 34, 58)       /* lights out */
#define CLR_FLOOR   CAPP_RGB(176, 196, 164)
#define CLR_FLOOR_D CAPP_RGB(22, 26, 44)
#define CLR_BAR     CAPP_RGB(24, 28, 38)
#define CLR_TEXT    CAPP_RGB(236, 240, 248)
#define CLR_DIM     CAPP_RGB(130, 138, 152)
#define CLR_INK     CAPP_RGB(28, 30, 36)
#define CLR_GOOD    CAPP_RGB(110, 196, 128)
#define CLR_MID     CAPP_RGB(255, 204, 82)
#define CLR_BAD     CAPP_RGB(228, 86, 76)
#define CLR_POOP    CAPP_RGB(132, 88, 48)
#define CLR_PANEL   CAPP_RGB(20, 23, 31)

/* ---- the art --------------------------------------------------------------
 * K outline, B body, L belly, e eye, p cheek, m mouth, W shell, y shell spot,
 * a antenna tip. */

static const char *const ART_EGG[SPR] = {
  "................", "................", "......KKKK......", ".....KWWWWK.....",
  "....KWWWWWWK....", "....KWWyWWWK....", "...KWWyyyWWWK...", "...KWWWyWWWWK...",
  "...KWWWWWWyWK...", "...KWyWWWyyyK...", "...KWyyWWWyWK...", "...KWWWWWWWWK...",
  "....KWWWWWWK....", ".....KKKKKK.....", "................", "................",
};
static const char *const ART_BABY[SPR] = {
  "................", "................", "................", "................",
  "................", "......KKKK......", "....KKBBBBKK....", "...KBBBBBBBBK...",
  "...KBeBBBBeBK...", "..KBBeBBBBeBBK..", "..KBpBBmmBBpBK..", "..KBBBBBBBBBBK..",
  "...KBBBBBBBBK...", "....KKKKKKKK....", "................", "................",
};
static const char *const ART_CHILD[SPR] = {
  "................", "................", "....K......K....", "...KBK....KBK...",
  "...KBBKKKKBBK...", "..KBBBBBBBBBBK..", "..KBBeBBBBeBBK..", "..KBBeBBBBeBBK..",
  ".KBBBBBBBBBBBBK.", ".KBpBBBmmBBBpBK.", ".KBBBBBBBBBBBBK.", ".KBBLLLLLLLLBBK.",
  "..KBLLLLLLLLBK..", "...KKBKKKKBKK...", "....KK....KK....", "................",
};
static const char *const ART_TEEN[SPR] = {
  "................", "...KK......KK...", "..KBBK....KBBK..", "..KBBBKKKKBBBK..",
  "..KBBBBBBBBBBK..", ".KBBBeBBBBeBBBK.", ".KBBBeBBBBeBBBK.", ".KBpBBBBBBBBpBK.",
  ".KBBBBBmmBBBBBK.", "KBKBBBBBBBBBBKBK", "KBKBBLLLLLLBBKBK", ".K.KBLLLLLLBK.K.",
  "...KBLLLLLLBK...", "...KBBBBBBBBK...", "...KBBK..KBBK...", "...KKK....KKK...",
};
static const char *const ART_ADULT[SPR] = {
  ".......aa.......", "...KK..KK...KK..", "..KBBK.KK.KBBK..", "..KBBBKKKKBBBK..",
  "..KBBBBBBBBBBK..", ".KBBBeBBBBeBBBK.", ".KBBBeBBBBeBBBK.", ".KBpBBBBBBBBpBK.",
  ".KBBBBBmmBBBBBK.", "KBKBBBBBBBBBBKBK", "KBKBBLLLLLLBBKBK", ".K.KBLLLLLLBK.K.",
  "...KBLLLLLLBK...", "...KBBBBBBBBK...", "...KBBK..KBBK...", "...KKK....KKK...",
};

typedef struct { uint16_t body, belly; } Colours;

static Colours colours(const Pet *p) {
  Colours c;
  switch (p->stage) {
  case PET_BABY:  c.body = CAPP_RGB(150, 214, 160); c.belly = CAPP_RGB(214, 240, 210); break;
  case PET_CHILD: c.body = CAPP_RGB(110, 200, 196); c.belly = CAPP_RGB(206, 238, 232); break;
  case PET_TEEN:  c.body = CAPP_RGB(232, 140, 196); c.belly = CAPP_RGB(250, 214, 232); break;
  default:
    if (p->form == PET_FORM_BRIGHT)      { c.body = CAPP_RGB(255, 196, 72);  c.belly = CAPP_RGB(255, 236, 188); }
    else if (p->form == PET_FORM_PLAIN)  { c.body = CAPP_RGB(110, 160, 232); c.belly = CAPP_RGB(206, 222, 250); }
    else                                 { c.body = CAPP_RGB(150, 130, 110); c.belly = CAPP_RGB(196, 184, 168); }
    break;
  }
  if (p->sick) { c.body = CAPP_RGB(170, 190, 120); }       /* a little green */
  return c;
}

static const char *const *art_of(const Pet *p) {
  switch (p->stage) {
  case PET_EGG:   return ART_EGG;
  case PET_BABY:  return ART_BABY;
  case PET_CHILD: return ART_CHILD;
  case PET_TEEN:  return ART_TEEN;
  default:        return ART_ADULT;
  }
}

/* ---- state ------------------------------------------------------------------ */

static const char *const NAMES[] = { "Mochi", "Pip", "Bean", "Tofu", "Nori", "Kiwi", "Bun", "Miso" };

enum { V_ROOM = 0, V_GAME, V_INFO, V_ASK_NEW };
enum { A_FEED = 0, A_SNACK, A_GAME, A_LIGHT, A_CLEAN, A_MED, A_INFO, A_COUNT };
static const char *const ACT_NAME[A_COUNT] = { "Feed", "Snack", "Game", "Light", "Clean", "Medicine", "Info" };

static struct {
  Pet      p;
  char     name[12];
  int      view, sel;
  int      x, dir, frame;          /* where it walks, and its bounce */
  uint32_t step_at, minute_at, open_ms;
  uint32_t open_epoch;             /* with no clock: time counted from open */
  char     msg[40];
  uint32_t msg_at;
  /* the game */
  int      round, wins, guess, look, reveal;
  uint32_t reveal_at;
  uint16_t box[WIDE_W * BOX_H];
} C;

static uint32_t num(const char *s) {
  uint32_t v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
  return v;
}

/* The fields, by name, for the file. */
typedef struct { const char *key; int *v; } Field;
static Field FIELDS[] = {
  { "age", &C.p.age_min }, { "food", &C.p.food }, { "fun", &C.p.fun },
  { "energy", &C.p.energy }, { "poop", &C.p.poop }, { "sick", &C.p.sick },
  { "asleep", &C.p.asleep }, { "weight", &C.p.weight }, { "misses", &C.p.misses },
  { "form", &C.p.form }, { "stage", &C.p.stage }, { "awake", &C.p.awake_min },
  { "mess", &C.p.mess_min }, { "starve", &C.p.starve_min }, { "dirty", &C.p.dirty_min },
  { "lowfood", &C.p.low_food }, { "lowfun", &C.p.low_fun },
};
#define NFIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

static void apply_line(const char *line) {
  int i;
  if (str_starts(line, "name=")) { api->fmt(C.name, sizeof C.name, "%s", line + 5); return; }
  if (str_starts(line, "born=")) { C.p.born = num(line + 5); return; }
  if (str_starts(line, "last=")) { C.p.last = num(line + 5); return; }
  if (str_starts(line, "seed=")) { C.p.seed = num(line + 5); return; }
  for (i = 0; i < NFIELDS; i++) {
    size_t n = api->str_len(FIELDS[i].key);
    if (str_starts(line, FIELDS[i].key) && line[n] == '=') { *FIELDS[i].v = (int)num(line + n + 1); return; }
  }
}

static int state_load(void) {
  char buf[128], line[48];
  int fd, n, i, len = 0, any = 0;
  fd = safe_open_read(api, STATE_PATH);
  if (fd < 0) return 0;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      if (buf[i] == '\r') continue;
      if (buf[i] != '\n') { if (len < (int)sizeof line - 1) line[len++] = buf[i]; continue; }
      line[len] = 0; len = 0;
      apply_line(line); any = 1;
    }
  }
  if (len) { line[len] = 0; apply_line(line); }
  api->close(fd);
  return any;
}

static void state_save(void) {
  SafeFile f;
  char line[48];
  int i;
  api->mkdir(CAPP_VAR);
  api->mkdir(STATE_DIR);
  if (safe_begin(&f, api, STATE_PATH) != 0) return;
  api->fmt(line, sizeof line, "name=%s\n", C.name); safe_line(&f, line);
  api->fmt(line, sizeof line, "born=%lu\n", (unsigned long)C.p.born); safe_line(&f, line);
  api->fmt(line, sizeof line, "last=%lu\n", (unsigned long)C.p.last); safe_line(&f, line);
  api->fmt(line, sizeof line, "seed=%lu\n", (unsigned long)C.p.seed); safe_line(&f, line);
  for (i = 0; i < NFIELDS; i++) {
    api->fmt(line, sizeof line, "%s=%d\n", FIELDS[i].key, *FIELDS[i].v);
    safe_line(&f, line);
  }
  safe_commit(&f);
}

/* The wall clock, or -- with none -- seconds counted from when the app opened
 * on top of where the pet's time stood, so it still lives while it is open. */
static uint32_t now_s(void) {
  uint32_t e = api->epoch();
  if (e) return e;
  return C.open_epoch + (api->ticks_ms() - C.open_ms) / 1000u;
}

static void new_egg(void) {
  uint32_t seed = api->ticks_ms() * 2654435761u + api->epoch();
  pet_new(&C.p, now_s(), seed);
  api->fmt(C.name, sizeof C.name, "%s", NAMES[pet_rand(&C.p) % (sizeof NAMES / sizeof NAMES[0])]);
  C.x = 92; C.dir = 1;
  state_save();
}

/* The OS reminds about food with the app shut. */
static void tell_os(void) {
  int m = pet_minutes_to_hungry(&C.p);
  char text[48];
  if (m < 0) { api->notify_cancel("hungry"); return; }
  api->fmt(text, sizeof text, "%s wants feeding", C.name);
  api->notify_at((uint32_t)m * 60u + 30u, "hungry", "hungry", text, 0);
}

static void say(const char *s) {
  api->fmt(C.msg, sizeof C.msg, "%s", s);
  C.msg_at = api->ticks_ms();
}

/* ---- painting ------------------------------------------------------------------ */

static uint16_t room_bg(void) { return C.p.asleep ? CLR_ROOM_D : CLR_ROOM; }
static uint16_t floor_bg(void) { return C.p.asleep ? CLR_FLOOR_D : CLR_FLOOR; }
#define FLOOR_Y (PET_Y + BOX_H - 6)

static void paint_bar_stat(int x, char label, int v) {
  uint16_t c = v > 50 ? CLR_GOOD : v > 25 ? CLR_MID : CLR_BAD;
  char s[2];
  int w = v * 30 / 100;
  s[0] = label; s[1] = 0;
  api->text((int16_t)x, 2, s, CLR_DIM, CLR_BAR);
  api->fill(capp_rect(x + 7, 3, 32, 6), CLR_INK);
  api->fill(capp_rect(x + 8, 4, w, 4), c);
  if (w < 30) api->fill(capp_rect(x + 8 + w, 4, 30 - w, 4), CLR_INK);
}

static void paint_bar(void) {
  char s[20];
  int w;
  api->fill(capp_rect(0, 0, 240, BAR_H), CLR_BAR);
  if (C.p.stage == PET_GONE || C.p.stage == PET_EGG) {
    api->text(4, 2, C.p.stage == PET_EGG ? "an egg" : "the nest is empty", CLR_DIM, CLR_BAR);
    return;
  }
  paint_bar_stat(2, 'F', C.p.food);
  paint_bar_stat(44, 'J', C.p.fun);
  paint_bar_stat(86, 'E', C.p.energy);
  api->fmt(s, sizeof s, "%s %s", C.name, pet_mood(&C.p));
  w = (int)api->str_len(s) * 6;
  api->text((int16_t)(238 - w), 2, s, C.p.sick ? CLR_BAD : CLR_TEXT, CLR_BAR);
}

/* The sprite into C.box with the room behind it and STEP_M of room either
 * side, then out in one blit -- which repaints where it just stepped from
 * as well, so a step needs nothing else drawn. */
static void paint_pet(void) {
  const char *const *art = art_of(&C.p);
  Colours col = colours(&C.p);
  uint16_t bg = room_bg(), fl = floor_bg();
  int bob = C.frame && !C.p.asleep ? 2 : 0;
  int blink = C.p.asleep || ((api->ticks_ms() / 300u) % 12u) == 0;
  int r, c, i, j;
  int egg_shift = C.p.stage == PET_EGG ? (C.frame ? 1 : -1) : 0;
  for (j = 0; j < BOX_H; j++) {
    uint16_t v = PET_Y + j >= FLOOR_Y ? fl : bg;
    for (i = 0; i < WIDE_W; i++) C.box[j * WIDE_W + i] = v;
  }
  for (r = 0; r < SPR; r++)
    for (c = 0; c < SPR; c++) {
      char ch = art[r][c];
      uint16_t v;
      if (ch == '.') continue;
      switch (ch) {
      case 'K': v = CLR_INK; break;
      case 'B': v = col.body; break;
      case 'L': v = col.belly; break;
      case 'p': v = CAPP_RGB(240, 140, 150); break;
      case 'm': v = CLR_INK; break;
      case 'W': v = CAPP_RGB(250, 248, 236); break;
      case 'y': v = CAPP_RGB(255, 196, 72); break;
      case 'a': v = CAPP_RGB(255, 236, 120); break;
      case 'e':
        /* eyes shut: the lower row of each eye becomes a line, the upper body */
        if (blink) v = (r + 1 < SPR && art[r + 1][c] == 'e') ? col.body : CLR_INK;
        else v = CLR_INK;
        break;
      default: v = col.body; break;
      }
      for (j = 0; j < SCALE; j++)
        for (i = 0; i < SCALE; i++) {
          int y = 2 + r * SCALE + j + bob - 2, x = 4 + c * SCALE + i + egg_shift;
          if (y >= 0 && y < BOX_H && x >= 0 && x < BOX_W) C.box[y * WIDE_W + STEP_M + x] = v;
        }
    }
  api->pixels(capp_rect(C.x - STEP_M, PET_Y, WIDE_W, BOX_H), C.box);
}

static void paint_poop(int n) {
  int k;
  for (k = 0; k < n; k++) {
    int x = 196 + (k % 2) * 20, y = FLOOR_Y - 14 - (k / 2) * 14;
    api->fill(capp_rect(x + 4, y, 4, 3), CLR_POOP);
    api->fill(capp_rect(x + 2, y + 3, 8, 4), CLR_POOP);
    api->fill(capp_rect(x, y + 7, 12, 5), CLR_POOP);
  }
}

static void paint_room(void) {
  uint16_t bg = room_bg(), fl = floor_bg();
  api->fill(capp_rect(0, ROOM_Y, 240, FLOOR_Y - ROOM_Y), bg);
  api->fill(capp_rect(0, FLOOR_Y, 240, ROOM_Y + ROOM_H - FLOOR_Y), fl);
  if (C.p.stage == PET_GONE) {
    api->text(60, 50, "the nest is empty", CLR_INK, bg);
    {
      char s[40];
      api->fmt(s, sizeof s, "%s flew home.", C.name);
      api->text((int16_t)(120 - (int)api->str_len(s) * 3), 66, s, CLR_INK, bg);
    }
    return;
  }
  paint_poop(C.p.poop);
  /* Both above the box's rows, so a step's blit never cuts into them. */
  if (C.p.asleep) api->text(C.x + BOX_W - 4 > 230 ? 200 : (int16_t)(C.x + BOX_W - 4), PET_Y - 8, "z Z", CLR_TEXT, bg);
  if (C.p.sick) api->text((int16_t)(C.x + 4), PET_Y - 8, "+ ill", CLR_BAD, bg);
  paint_pet();
}

static void paint_foot(CRect full) {
  char s[48];
  /* The tick lets a message go when its time is up, and repaints. Not
   * here: paint runs once per strip of a full repaint, and a message that
   * expired between two strips was half drawn and then forgotten. */
  if (C.msg[0]) { footer_paint(api, full, C.msg); return; }
  if (C.view == V_ASK_NEW) { footer_paint(api, full, "a new egg, and this pet goes? y/n"); return; }
  if (C.p.stage == PET_GONE) { footer_paint(api, full, "n a new egg"); return; }
  api->fmt(s, sizeof s, "< %s >  enter do  spc light", ACT_NAME[C.sel]);
  footer_paint(api, full, s);
}

static void paint_info(void) {
  char s[48];
  int y = ROOM_Y + 6;
  uint32_t d = (uint32_t)C.p.age_min / 1440u, h = ((uint32_t)C.p.age_min / 60u) % 24u;
  static const char *const STAGE[] = { "egg", "baby", "child", "teen", "adult", "gone" };
  static const char *const FORM[] = { "bright", "plain", "scruffy" };
  api->fill(capp_rect(0, ROOM_Y, 240, ROOM_H), CLR_PANEL);
  api->fmt(s, sizeof s, "%s, %s", C.name, C.p.stage == PET_ADULT ? FORM[C.p.form] : STAGE[C.p.stage]);
  api->text(10, (int16_t)y, s, CLR_TEXT, CLR_PANEL); y += 14;
  api->fmt(s, sizeof s, "age      %lud %luh", (unsigned long)d, (unsigned long)h);
  api->text(10, (int16_t)y, s, CLR_DIM, CLR_PANEL); y += 11;
  api->fmt(s, sizeof s, "weight   %dg", C.p.weight);
  api->text(10, (int16_t)y, s, CLR_DIM, CLR_PANEL); y += 11;
  api->fmt(s, sizeof s, "food %d  fun %d  energy %d", C.p.food, C.p.fun, C.p.energy);
  api->text(10, (int16_t)y, s, CLR_DIM, CLR_PANEL); y += 11;
  api->fmt(s, sizeof s, "let down %d time%s", C.p.misses, C.p.misses == 1 ? "" : "s");
  api->text(10, (int16_t)y, s, CLR_DIM, CLR_PANEL); y += 16;
  api->text(10, (int16_t)y, C.p.stage < PET_ADULT ? "grows by age; kept well, it grows bright"
                                                  : "all grown up", CLR_DIM, CLR_PANEL);
}

static void paint_game(void) {
  char s[40];
  uint16_t bg = CLR_PANEL;
  api->fill(capp_rect(0, ROOM_Y, 240, ROOM_H), bg);
  api->fmt(s, sizeof s, "round %d of 5   won %d", C.round + 1 > 5 ? 5 : C.round + 1, C.wins);
  api->text(10, ROOM_Y + 6, s, CLR_DIM, bg);
  if (C.round >= 5) {
    api->text(60, 56, C.wins >= 3 ? "it had a great time!" : "it had fun anyway", CLR_TEXT, bg);
    api->text(60, 70, "enter: back", CLR_DIM, bg);
    return;
  }
  api->fmt(s, sizeof s, "which way will %s look?", C.name);
  api->text((int16_t)(120 - (int)api->str_len(s) * 3), 40, s, CLR_TEXT, bg);
  if (C.reveal) {
    const char *face = C.look < 0 ? "<-- (o_o )" : "( o_o) -->";
    api->text(90, 64, face, C.guess == C.look ? CLR_GOOD : CLR_BAD, bg);
    api->text(96, 80, C.guess == C.look ? "right!" : "missed", C.guess == C.look ? CLR_GOOD : CLR_BAD, bg);
  } else {
    api->text(96, 64, "( o_o )", CLR_TEXT, bg);
    api->text(72, 80, "left or right?", CLR_DIM, bg);
  }
}

/* The rect a step marks (app_tick). */
static CRect step_rect(void) { return capp_rect(C.x - STEP_M, PET_Y - 10, WIDE_W, BOX_H + 10); }

static int inside(CRect a, CRect b) {
  return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
}

static void app_paint(void *st, CRect full) {
  (void)st;
  /* A step's own repaint: only the box, which carries the room it stood
   * on. Filling the room under it first, as a full paint does, showed the
   * pet as a patch of wall every 650 ms. The lettering above did not move
   * (the pet walks only awake and well, when there is none). */
  if (C.view == V_ROOM && C.p.stage != PET_GONE && inside(api->paint_area(), step_rect())) {
    paint_pet();
    return;
  }
  paint_bar();
  if (C.view == V_INFO) paint_info();
  else if (C.view == V_GAME) paint_game();
  else paint_room();
  paint_foot(full);
}

/* ---- doing things ------------------------------------------------------------- */

static void after(const char *why, const char *done) {
  if (why) { say(why); return; }
  if (done) say(done);
  state_save();
  tell_os();
}

static void do_act(int a) {
  char s[40];
  switch (a) {
  case A_FEED:  api->fmt(s, sizeof s, "%s eats", C.name); after(pet_feed(&C.p), s); break;
  case A_SNACK: after(pet_snack(&C.p), "a sweet: yum"); break;
  case A_GAME: {
    const char *why = pet_can_play(&C.p);
    if (why) { say(why); break; }
    C.view = V_GAME; C.round = 0; C.wins = 0; C.reveal = 0;
    break;
  }
  case A_LIGHT: after(pet_lights(&C.p), C.p.asleep ? "lights out" : "lights on"); break;
  case A_CLEAN: after(pet_clean(&C.p), "all clean"); break;
  case A_MED:   after(pet_medicine(&C.p), "medicine: yuck, but better"); break;
  case A_INFO:  C.view = V_INFO; break;
  }
}

static void game_key(uint8_t k) {
  if (C.round >= 5) {
    if (k == CAPP_KEY_ENTER || k == CAPP_KEY_ESC || k == ' ') {
      pet_played(&C.p, C.wins);
      state_save();
      C.view = V_ROOM;
    }
    return;
  }
  if (k == CAPP_KEY_ESC) { C.view = V_ROOM; return; }
  if (C.reveal) return;
  if (k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return;
  C.guess = k == CAPP_KEY_LEFT ? -1 : 1;
  C.look = (pet_rand(&C.p) & 1) ? 1 : -1;
  if (C.guess == C.look) C.wins++;
  C.reveal = 1;
  C.reveal_at = api->ticks_ms();
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (api->key_repeat() && k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 1;
  if (C.view == V_GAME) { game_key(k); return 1; }
  if (C.view == V_INFO) { if (k == CAPP_KEY_ESC || k == CAPP_KEY_ENTER || k == 'i') C.view = V_ROOM; return 1; }
  if (C.view == V_ASK_NEW) {
    int a = confirm_key(api, k);
    if (a != CONFIRM_WAIT) C.view = V_ROOM;
    if (a == CONFIRM_YES) { new_egg(); tell_os(); }
    return 1;
  }
  if (C.p.stage == PET_GONE) {
    if (k == 'n' || k == 'N' || k == CAPP_KEY_ENTER) { new_egg(); tell_os(); return 1; }
    return k == CAPP_KEY_ESC ? 0 : 1;
  }
  switch (k) {
  case CAPP_KEY_LEFT:  C.sel = (C.sel + A_COUNT - 1) % A_COUNT; return 1;
  case CAPP_KEY_RIGHT: C.sel = (C.sel + 1) % A_COUNT; return 1;
  case CAPP_KEY_ENTER: do_act(C.sel); return 1;
  case ' ':            do_act(A_LIGHT); return 1;
  case 'f': do_act(A_FEED); return 1;
  case 's': do_act(A_SNACK); return 1;
  case 'g': do_act(A_GAME); return 1;
  case 'c': do_act(A_CLEAN); return 1;
  case 'm': do_act(A_MED); return 1;
  case 'i': do_act(A_INFO); return 1;
  case 'n': C.view = V_ASK_NEW; return 1;
  default:  return 0;
  }
}

/* The walk, the bounce, a minute of life, and the game's next round. */
static int app_tick(void *st, uint32_t now) {
  int redraw = 0;
  (void)st;
  if (now - C.minute_at >= 60000u) {
    int before = C.p.stage;
    C.minute_at = now;
    if (pet_advance(&C.p, now_s())) {
      state_save();
      if (C.p.stage != before) {
        say(C.p.stage == PET_GONE ? "it flew home" : C.p.stage == PET_BABY ? "it hatched!" : "it grew!");
        tell_os();
      }
      redraw = 1;
    }
  }
  if (C.msg[0] && now - C.msg_at >= MSG_MS) { C.msg[0] = 0; redraw = 1; }
  if (C.view == V_GAME && C.reveal && now - C.reveal_at >= 900u) {
    C.reveal = 0;
    C.round++;
    redraw = 1;
  }
  if (C.view == V_ROOM && C.p.stage != PET_GONE && now - C.step_at >= STEP_MS) {
    C.step_at = now;
    C.frame ^= 1;
    if (!C.p.asleep && C.p.stage != PET_EGG && !C.p.sick) {
      if ((pet_rand(&C.p) % 5u) == 0) C.dir = -C.dir;
      C.x += C.dir * 4;
      if (C.x < 8) { C.x = 8; C.dir = 1; }
      if (C.x > 186 - BOX_W) { C.x = 186 - BOX_W; C.dir = -1; }
    }
    /* Only the box moves: the room around it stays as it was. Its STEP_M
     * of room either side covers the step (app_paint). */
    api->damage(step_rect());
    return 1;
  }
  return redraw;
}

static int app_wants_text(void *st) { (void)st; return 0; }

/* ---- commands ----------------------------------------------------------------- */

enum { ACT_STATUS = 1, ACT_FEED_CMD, ACT_CLEAN_CMD };

static const CappAction ACTIONS[] = {
  { "status", "Status", 0, 0, ACT_STATUS, "how the pet is doing", 0, 0, CAPP_CMD_YES },
  { "feed",   "Feed",   0, 0, ACT_FEED_CMD, "give the pet a meal", 0, 0, CAPP_CMD_YES },
  { "clean",  "Clean",  0, 0, ACT_CLEAN_CMD, "clean up after the pet", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  const char *why = 0;
  (void)st; (void)argc; (void)argv;
  if (action == ACT_FEED_CMD) why = pet_feed(&C.p);
  else if (action == ACT_CLEAN_CMD) why = pet_clean(&C.p);
  else if (action != ACT_STATUS) { api->fmt(out, n, "pet has no command %d", action); return -1; }
  if (why) { api->fmt(out, n, "%s: %s", C.name, why); return -1; }
  if (action != ACT_STATUS) { state_save(); tell_os(); }
  api->fmt(out, n, "%s is %s. food %d, fun %d, energy %d%s", C.name, pet_mood(&C.p),
           C.p.food, C.p.fun, C.p.energy, C.p.poop ? ", needs cleaning" : "");
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Pet",
  /* 16x16: a round little creature with two eyes and feet. */
  { 0x00, 0x00, 0x07, 0xE0, 0x18, 0x18, 0x20, 0x04,
    0x40, 0x02, 0x4C, 0x32, 0x8C, 0x31, 0x80, 0x01,
    0x83, 0xC1, 0x80, 0x01, 0x40, 0x02, 0x40, 0x02,
    0x30, 0x0C, 0x0F, 0xF0, 0x0C, 0x30, 0x00, 0x00 },
  "left/right\tchoose an action\nenter\tdo it\nspace\tlights out / on\n"
  "f\tfeed\ns\tsnack\ng\tthe which-way game\nc\tclean up\nm\tmedicine\ni\tinfo\n"
  "n\ta new egg (asks)\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  uint32_t e;
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&C, 0, sizeof C);
  C.open_ms = api->ticks_ms();
  e = api->epoch();
  if (!state_load()) {
    C.open_epoch = 1;                /* no clock and no pet: time starts here */
    new_egg();
  } else {
    /* A pet raised with no clock, now with one: its time joins the clock's
     * from here, rather than catching up from 1970. */
    if (e && C.p.last < 1000000000u) C.p.last = e;
    C.open_epoch = C.p.last;
    if (pet_advance(&C.p, now_s())) state_save();
  }
  C.x = 92; C.dir = 1;
  C.minute_at = C.step_at = api->ticks_ms();
  if (!api->headless()) tell_os();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}

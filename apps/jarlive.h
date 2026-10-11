/* The living jar's scene (apps/jar.c includes this after its drawing
 * helpers): what the sim's world events, chores, parts, scenery and big
 * things look like, and its sounds -- docs/superpowers/specs/
 * 2026-10-11-jar-living-world-design.md, stages D, E and F.
 *
 * DRAWING. Scenery (role background) is drawn after the sky and before the
 * ledges, at twice the size; big things at twice the size where they stand;
 * a part (the birdhouse's bird) wherever the sim has it. Ants march along
 * the soil, red, a berry on the back of one carrying; pellets are brown
 * crumbs; the leak's puddle is a blue spread on the soil by the works; mould
 * is grey-green on a bush; a visitor bird sits on the lid; `dark` darkens
 * the jar on top of the hour's light.
 *
 * CHORES. The sim stops what a chore stops; this says so -- a line in the
 * bar now and then -- and x does every chore there is (the snail cheered,
 * the puddle mopped, the mould cleared).
 *
 * SOUND. An item's `sound N` (and its voice, when poked) plays from a bank
 * of fourteen synthesised once into /cache/jar_*.wav (apps/synth.h). The
 * soundtrack is very minimal on purpose: a phrase of a few plucked notes now
 * and then -- every 10-25 s by day, less at night -- never over a sound,
 * never over anyone else's audio. b cycles: music and sounds, sounds only,
 * silence; kept in /var/jar/sound.txt. */

#include "apps/synth.h"

/* ---- sounds ------------------------------------------------------------------- */

#define LV_SOUNDS 14
#define LV_PHRASES 4

static const SyNote LS_POP[]    = { { 880, 40, 60, SY_TRI }, { 1320, 50, 50, SY_TRI } };
static const SyNote LS_CHIRP[]  = { { 2093, 50, 45, SY_TRI }, { 2637, 40, 45, SY_TRI }, { 2349, 60, 40, SY_TRI } };
static const SyNote LS_BOING[]  = { { 196, 60, 60, SY_TRI }, { 294, 60, 55, SY_TRI }, { 392, 60, 50, SY_TRI },
                                    { 330, 120, 45, SY_TRI } };
static const SyNote LS_DING[]   = { { 1568, 250, 55, SY_TRI } };
static const SyNote LS_HORN[]   = { { 98, 900, 70, SY_BUZZ } };
static const SyNote LS_WHOOSH[] = { { 0, 260, 35, SY_NOISE } };
static const SyNote LS_CRUNCH[] = { { 0, 30, 55, SY_NOISE }, { 0, 20, 0, SY_TRI }, { 0, 40, 50, SY_NOISE } };
static const SyNote LS_PLOP[]   = { { 330, 60, 55, SY_TRI }, { 196, 90, 45, SY_TRI } };
static const SyNote LS_COIN[]   = { { 1976, 60, 50, SY_SQR }, { 2637, 160, 45, SY_SQR } };
static const SyNote LS_FIZZ[]   = { { 0, 400, 25, SY_NOISE } };
static const SyNote LS_CLOCK[]  = { { 2400, 15, 45, SY_SQR }, { 0, 230, 0, SY_TRI }, { 1800, 15, 45, SY_SQR } };
static const SyNote LS_SPLASH[] = { { 0, 60, 60, SY_NOISE }, { 0, 200, 30, SY_NOISE } };
static const SyNote LS_BANG[]   = { { 0, 30, 90, SY_NOISE }, { 110, 140, 60, SY_SQR } };
static const SyNote LS_TUNE[]   = { { 523, 120, 45, SY_TRI }, { 659, 120, 45, SY_TRI }, { 784, 120, 45, SY_TRI },
                                    { 1047, 240, 45, SY_TRI } };
/* the soundtrack: four short phrases, pentatonic, soft */
static const SyNote LM_A[] = { { 392, 380, 22, SY_TRI }, { 440, 380, 20, SY_TRI }, { 523, 760, 22, SY_TRI } };
static const SyNote LM_B[] = { { 659, 380, 20, SY_TRI }, { 587, 380, 18, SY_TRI }, { 523, 380, 20, SY_TRI },
                               { 440, 760, 18, SY_TRI } };
static const SyNote LM_C[] = { { 330, 500, 20, SY_TRI }, { 392, 500, 18, SY_TRI }, { 294, 1000, 18, SY_TRI } };
static const SyNote LM_D[] = { { 523, 300, 18, SY_TRI }, { 0, 300, 0, SY_TRI }, { 659, 300, 18, SY_TRI },
                               { 784, 900, 20, SY_TRI } };

typedef struct { const SyNote *n; uint8_t count; } LvSnd;
#define LN(a) { a, (uint8_t)(sizeof(a) / sizeof((a)[0])) }
static const LvSnd LV_BANK[LV_SOUNDS + LV_PHRASES] = {
  LN(LS_POP), LN(LS_CHIRP), LN(LS_BOING), LN(LS_DING), LN(LS_HORN), LN(LS_WHOOSH), LN(LS_CRUNCH),
  LN(LS_PLOP), LN(LS_COIN), LN(LS_FIZZ), LN(LS_CLOCK), LN(LS_SPLASH), LN(LS_BANG), LN(LS_TUNE),
  LN(LM_A), LN(LM_B), LN(LM_C), LN(LM_D),
};

enum { LV_ALL = 0, LV_SOUNDS_ONLY, LV_SILENT };

static struct {
  int mode;                              /* LV_* */
  uint32_t have;                         /* a bit per bank entry on the card */
  uint32_t end;                          /* when ours that is playing ends */
  uint32_t next_music, next_chore;
  int made;
} LV;

static void lv_path(char *out, int n, int k) {
  api->fmt(out, (size_t)n, CAPP_CACHE "/jar_%d.wav", k);
}

/* The bank made, once; slow only the first time (a few KB of WAV each). */
static void lv_make(void) {
  int k;
  char p[40];
  if (LV.made) return;
  LV.made = 1;
  api->mkdir(CAPP_CACHE);
  for (k = 0; k < LV_SOUNDS + LV_PHRASES; k++) {
    lv_path(p, sizeof p, k);
    if (synth_ensure(api, p, LV_BANK[k].n, LV_BANK[k].count)) LV.have |= 1u << k;
  }
}

static const CappAudio *lv_audio(void) { return api->audio ? api->audio() : 0; }

/* Bank entry k: a sound (0..13) or a phrase (14..17). A sound cuts a phrase
 * of ours short; nothing cuts anyone else's audio. */
static void lv_play(int k) {
  const CappAudio *au = lv_audio();
  char p[40];
  uint32_t now = api->ticks_ms();
  int phrase = k >= LV_SOUNDS;
  if (!au || LV.mode == LV_SILENT || (phrase && LV.mode != LV_ALL) || !(LV.have & (1u << k))) return;
  if (au->state() != CAPP_AUDIO_IDLE) {
    if ((int32_t)(now - LV.end) >= 0 || phrase) return;   /* not ours, or music waits */
    au->stop();
  }
  lv_path(p, sizeof p, k);
  if (au->play(p) == 0) LV.end = now + (uint32_t)synth_ms(LV_BANK[k].n, LV_BANK[k].count) + 80u;
}

static void lv_load_mode(void) {
  char b[8];
  int fd = api->open(CAPP_VAR "/jar/sound.txt", CAPP_O_READ), n;
  if (fd < 0) return;
  n = api->read(fd, b, sizeof b - 1);
  api->close(fd);
  if (n > 0 && b[0] >= '0' && b[0] <= '2') LV.mode = b[0] - '0';
}

static void lv_save_mode(void) {
  char b[4];
  int fd = api->open(CAPP_VAR "/jar/sound.txt", CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  b[0] = (char)('0' + LV.mode);
  b[1] = '\n';
  api->write(fd, b, 2);
  api->close(fd);
}

/* ---- drawing ------------------------------------------------------------------ */

/* Where an item is drawn: its top left, and how many times its size. */
static int lv_at(int i, int *x, int *y) {
  const JPlaced *p = &J.placed[i];
  item_at(i, x, y);
  if (p->scale == 2) {                   /* twice the size, standing where it stood */
    *x -= 8;
    if (p->kind != JK_HANGING) *y -= 16;
  }
  return p->scale == 2 ? 2 : 1;
}

/* Scenery: behind the ledges, at its size. */
static void lv_scenery(void) {
  int i, x, y, s;
  for (i = 0; i < J.nplaced; i++) {
    const JPlaced *p = &J.placed[i];
    int f;
    if (p->role != JR_BACKGROUND) continue;
    f = p->slot[p->cur % (p->nframes ? p->nframes : 1)];
    s = lv_at(i, &x, &y);
    if (f < JS_POOL) pic(J.pool[f], p->pal, 1, 16, 16, x, y, p->flip, s);
  }
}

/* One item in front: its glow, its string, its picture, at its size. */
static void lv_item(int i) {
  const JPlaced *p = &J.placed[i];
  int x, y, s, f = p->slot[p->cur % (p->nframes ? p->nframes : 1)];
  if (p->role == JR_BACKGROUND) return;
  s = lv_at(i, &x, &y);
  if (p->kind == JK_HANGING) fill(x + 7 * s, 9, 1, y - 8, rgb(0xc8, 0xc0, 0xb0));
  if (p->glow) glow(x + 8 * s, y + 8 * s, 14 * s, 255, 220, 140, 110);
  if (f < JS_POOL) pic(J.pool[f], p->pal, 1, 16, 16, x, y, p->flip, s);
}

/* Parts out and about (and at home), over the items. */
static void lv_parts(void) {
  int i;
  for (i = 0; i < J.nplaced; i++) {
    const JPlaced *p = &J.placed[i];
    int f;
    if (!p->part) continue;
    f = p->slot[p->nframes];               /* the frame after the poses */
    if (f < JS_POOL)
      pic(J.pool[f], p->pal, 1, 16, 16, p->px / 16 - 8, p->py / 16 - 8 + p->pdy / 4,
          p->pst == 1 && p->ptx < p->px, 1);
  }
}

/* The world's doings on the soil and the lid. */
static void lv_world(void) {
  int k;
  for (k = 0; k < JS_PELLETS; k++)
    if (J.pel_t[k]) {
      fill(J.pel_x[k], JS_SOIL - 1, 2, 1, rgb(0x6a, 0x48, 0x2a));
      px(J.pel_x[k] + 2, JS_SOIL - 1, rgb(0x50, 0x36, 0x20));
    }
  if (J.puddle) {
    int w = 6 + J.puddle, x0 = 150 - w / 2;
    fill(x0 + 2, JS_SOIL - 1, w - 4, 1, rgb(0x5a, 0x90, 0xc8));
    fill(x0, JS_SOIL, w, 1, rgb(0x3a, 0x70, 0xb0));
    fill(x0 + 3, JS_SOIL - 1, w / 4, 1, rgb(0xb0, 0xd8, 0xf0));   /* a glint */
  }
  if (J.world == JWD_LEAK && (J.steps / 6) % 7 == 0)               /* a drip */
    fill(150, JS_LID + 2 + (int)((J.steps % 42) * 2), 1, 2, rgb(0x8a, 0xb8, 0xe0));
  for (k = 0; k < J.nbeds; k++)
    if (J.bed[k].mould) {
      int bx = JS_BED_X(k), d;
      for (d = 0; d < 7; d++)
        px(bx - 4 + (int)(hash2(k, d) % 9), JS_SOIL - 3 - (int)(hash2(d, k) % 9), rgb(0x8a, 0xa0, 0x70));
    }
  for (k = 0; k < JS_ANTS; k++)
    if (J.ant_st[k]) {
      int x = J.ant_x[k] / 16, leg = (int)((J.steps >> 2) + (uint32_t)k) & 1;
      fill(x, JS_SOIL - 2, 3, 1, rgb(0x90, 0x20, 0x18));
      px(x + (J.ant_st[k] == 1 ? 3 : -1), JS_SOIL - 2, rgb(0x60, 0x14, 0x10));
      px(x + leg, JS_SOIL - 1, rgb(0x60, 0x14, 0x10));
      if (J.ant_st[k] == 2) fill(x, JS_SOIL - 4, 2, 2, rgb(0xd0, 0x30, 0x50));   /* a berry */
    }
  if (J.world == JWD_VISITOR) {                                     /* a bird on the lid */
    int x = 118 + (int)((J.steps / 40) % 3), y = JS_LID - 6;
    fill(x, y + 1, 5, 3, rgb(0x6a, 0x50, 0x40));
    fill(x + 4, y, 2, 2, rgb(0x6a, 0x50, 0x40));
    px(x + 6, y + 1, rgb(0xe8, 0xb0, 0x40));
    px(x + 1, y + 4, rgb(0x40, 0x30, 0x28));
  }
}

/* ---- chores, keys, the clock --------------------------------------------------- */

static const char *lv_chore_line(void) {
  int c = js_chores(&J);
  if (c & JC_SULK) return "The snail is sulking: x cheers it up";
  if (c & JC_PUDDLE) return "A puddle stops the garden: x mops it";
  if (c & JC_MOULD) return "Mould on a bush: x clears it";
  return 0;
}

/* x: every chore there is; b: the sound. 1 if the key was ours. */
static int lv_key(int k) {
  if (k == 'x' || k == 'X') {
    if (js_fix(&J, JC_SULK | JC_PUDDLE | JC_MOULD) == 0) { say("All sorted"); lv_play(3); save(); }
    else say("Nothing needs doing");
    return 1;
  }
  if (k == 'b' || k == 'B') {
    LV.mode = (LV.mode + 1) % 3;
    lv_save_mode();
    say(LV.mode == LV_ALL ? "Music and sounds" : LV.mode == LV_SOUNDS_ONLY ? "Sounds only" : "Quiet");
    J.music = LV.mode == LV_ALL;
    if (LV.mode != LV_SILENT) lv_play(0);
    return 1;
  }
  return 0;
}

/* From the scene's tick: a sound an item asked for, the soundtrack, and the
 * chore line every half minute. */
static void lv_tick(uint32_t now) {
  if (J.snd) {
    lv_play(J.snd - 1);
    J.snd = 0;
  }
  if (LV.mode == LV_ALL && (int32_t)(now - LV.next_music) >= 0) {
    int night = J.phase == PH_NIGHT;
    lv_play(LV_SOUNDS + (int)(js_rnd(&J) % LV_PHRASES));
    LV.next_music = now + (uint32_t)js_rr(&J, night ? 25 : 10, night ? 50 : 25) * 1000u;
  }
  if ((int32_t)(now - LV.next_chore) >= 0) {
    const char *c = lv_chore_line();
    LV.next_chore = now + 30000u;
    if (c && !(G.msg[0] && (int32_t)(now - G.msg_until) < 0)) {
      api->fmt(G.msg, sizeof G.msg, "%s", c);
      G.msg_until = now + 8000;
      G.menu_dirty = 1;
    }
  }
}

/* The scene opens: the screen stays on while Jar Factory is open, the bank
 * is made, the mode read. */
static void lv_open(void) {
  if (api->keep_awake) api->keep_awake(1);
  lv_load_mode();
  J.music = LV.mode == LV_ALL;
  lv_make();
  LV.next_music = api->ticks_ms() + 6000u;
  LV.next_chore = api->ticks_ms() + 5000u;
}

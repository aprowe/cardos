/* Game Night: a scoreboard, dice, a round timer and a Taboo buzzer, for the
 * table. Four tools, each on its own: the menu opens one (Enter, or 1-4),
 * Esc comes back to the menu, and a timer that is running keeps running
 * while the others are used -- the buzzer shows what is left of it.
 *
 * SCORE (apps/gn_score.h): players or teams across, rounds down, totals
 * at the bottom, the leader in gold; any game scored in rounds, nothing
 * about any one game. Type a number in a cell -- minus too -- and Enter
 * goes on to the next player, then the next round. It does not follow
 * whose turn it is; f picks who goes first at random (a spin through the
 * names), g makes it the column the cursor is in. l: the lowest total wins
 * (Hearts). Up from the first round is the names' row, where e renames and
 * d removes a player. Kept in /var/gamenight/score.txt.
 *
 * DICE (apps/gn_dice.h): up/down how many (1-9), left/right how many sides
 * (2, 4, 6, 8, 10, 12, 20, 100), e types any -- "3d6+2", "d%". Space or
 * Enter rolls, and so does a shake on a Cardputer ADV (its motion sensor).
 * 1-9 hold a die through the next roll (Yahtzee). A d6 shows its pips, a d2
 * is a coin.
 *
 * TIMER: left/right a preset (30 s to 10 min), up/down 15 s more or less;
 * Space starts and pauses, Enter puts it back. The last ten seconds tick
 * and turn red; at zero it sounds until a key. The screen stays on while it
 * runs.
 *
 * BUZZER: Space, Enter, b -- or the button on top -- is the buzzer, as loud
 * as the speaker goes; c is a correct answer (a ding), s a skip. It counts
 * all three; n starts the counts again.
 *
 * Sounds are made once into /cache (as Iron's and Timer's are: the API
 * plays WAV files and has nothing that makes a tone), and m turns them off.
 * Settings -- the dice, the timer's preset, sound -- are in
 * /var/gamenight/prefs.txt.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/safefile.h"
#include "apps/gn_score.h"
#include "apps/gn_dice.h"

static const CardApi *api;
static const CappAudio *au;

#define DIR         CAPP_VAR "/gamenight"
#define SCORE_PATH  DIR "/score.txt"
#define PREFS_PATH  DIR "/prefs.txt"

#define C_BG     CAPP_RGB(16, 18, 26)
#define C_PANEL  CAPP_RGB(30, 34, 48)
#define C_LINE   CAPP_RGB(46, 52, 70)
#define C_TEXT   CAPP_RGB(236, 238, 244)
#define C_DIM    CAPP_RGB(128, 136, 156)
#define C_GOLD   CAPP_RGB(255, 204, 82)
#define C_TEAL   CAPP_RGB(96, 214, 198)
#define C_RED    CAPP_RGB(232, 76, 70)
#define C_GREEN  CAPP_RGB(110, 206, 120)
#define C_BLUE   CAPP_RGB(110, 160, 255)
#define C_DIE    CAPP_RGB(244, 240, 228)
#define C_PIP    CAPP_RGB(30, 32, 40)

enum { SC_HOME = 0, SC_SCORE, SC_PLAYERS, SC_DICE, SC_TIMER, SC_BUZZ };
enum { ASK_NONE = 0, ASK_NEW, ASK_DEL_ROUND, ASK_DEL_PLAYER };

/* ---- sounds ------------------------------------------------------------------ */

#define SND_RATE 8000u
enum { W_TRI, W_SQR, W_NOISE, W_BUZZ };
typedef struct { uint16_t hz, ms; uint8_t vol, wave; } Note;
enum { SN_BUZZ, SN_DING, SN_SKIP, SN_TICK, SN_ALARM, SN_RATTLE, SN_CLICK, SN_THINK, SN_COUNT };

static const Note N_BUZZ[]   = { { 110, 900, 100, W_BUZZ } };
static const Note N_DING[]   = { { 1047, 120, 70, W_TRI }, { 1568, 260, 70, W_TRI } };
static const Note N_SKIP[]   = { { 0, 220, 40, W_NOISE } };
static const Note N_TICK[]   = { { 1760, 30, 55, W_SQR } };
static const Note N_ALARM[]  = { { 988, 160, 85, W_SQR }, { 0, 70, 0, W_TRI }, { 988, 160, 85, W_SQR },
                                 { 0, 70, 0, W_TRI }, { 1319, 360, 85, W_SQR } };
static const Note N_RATTLE[] = { { 0, 40, 60, W_NOISE }, { 0, 50, 0, W_TRI }, { 0, 35, 55, W_NOISE },
                                 { 0, 60, 0, W_TRI }, { 0, 30, 50, W_NOISE }, { 0, 70, 0, W_TRI },
                                 { 0, 25, 45, W_NOISE } };
static const Note N_CLICK[]  = { { 660, 25, 45, W_TRI } };
/* Thinking music for a slow answer: an original tune, not any show's. */
static const Note N_THINK[] = {
  { 523, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 698, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 880, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 1047, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 698, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 587, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 523, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 698, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 880, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 1047, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 988, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 880, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 698, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 587, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 523, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 440, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 523, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 523, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 587, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 698, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 392, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 494, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 587, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 494, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 523, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 587, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 523, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 659, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 698, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 880, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 1047, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 988, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 880, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 1175, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 1047, 660, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 784, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI },
  { 1047, 310, 55, W_TRI },
  { 0, 40, 0, W_TRI }
};

typedef struct { const char *name; const Note *n; uint8_t count; } Snd;
#define NN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))
static const Snd SND[SN_COUNT] = {
  { "buzz", N_BUZZ, NN(N_BUZZ) }, { "ding", N_DING, NN(N_DING) },
  { "skip", N_SKIP, NN(N_SKIP) }, { "tick", N_TICK, NN(N_TICK) },
  { "alarm", N_ALARM, NN(N_ALARM) }, { "rattle", N_RATTLE, NN(N_RATTLE) },
  { "click", N_CLICK, NN(N_CLICK) }, { "think", N_THINK, NN(N_THINK) },
};

/* ---- state ---------------------------------------------------------------------- */

static const int16_t PRESETS[] = { 30, 45, 60, 90, 120, 180, 300, 600 };
#define NPRESET ((int)(sizeof PRESETS / sizeof PRESETS[0]))

static GScore G;
static GDice D;
static char TXT[4096];                  /* the scoreboard as text, to and from the card */

static struct {
  int screen, sel, sound, ask;
  int f_ui, f_uib, f_num, f_big;
  uint32_t snd_have, snd_end;
  /* score */
  int row, col, top, left;              /* the cursor (row -1: the names), the scroll */
  char edit[8];                         /* a number being typed into the cell */
  int editing;
  char input[GS_NAME + 1];              /* a name being typed */
  int renaming;                         /* the column being renamed, -1 adding */
  int spin;                             /* spinning for who goes first: steps left */
  uint32_t spin_at;
  int spin_col;
  /* dice */
  int rolling;
  uint32_t roll_at, roll_end;
  int32_t hist[6];
  int nhist;
  int typing;                           /* typing dice: "3d6+2" */
  char dtext[12];
  int shake_cd;
  uint32_t shake_at;
  int motion_ok;
  /* timer */
  int t_preset;                         /* seconds it is set to */
  int32_t t_left_ms;
  int t_running, t_done;
  uint32_t t_last, t_flash;
  int t_last_sec;
  /* buzzer */
  int b_correct, b_taboo, b_skip;
  int b_flash, b_kind;                  /* the screen flashes: 0 red buzz, 1 green, 2 blue */
  uint32_t b_until;
  int think;                            /* the thinking music is ours and playing */
  /* saving */
  int dirty;
  uint32_t dirty_at;
} S;

static uint32_t rnd32(void) { return gd_rnd(&D); }

/* ---- the card -------------------------------------------------------------------- */

static void save_score(void) {
  SafeFile f;
  int n = gs_save(&G, TXT, sizeof TXT);
  api->mkdir(DIR);
  if (safe_begin(&f, api, SCORE_PATH) != 0) return;
  safe_write(&f, TXT, (size_t)n);
  safe_commit(&f);
}

static void save_prefs(void) {
  SafeFile f;
  char line[48], name[16];
  api->mkdir(DIR);
  if (safe_begin(&f, api, PREFS_PATH) != 0) return;
  gd_name(&D, name, sizeof name);
  api->fmt(line, sizeof line, "dice %s\ntimer %d\nsound %d\n", name, S.t_preset, S.sound);
  safe_line(&f, line);
  safe_commit(&f);
}

static int read_all(const char *path, char *buf, int cap) {
  int fd = safe_open_read(api, path), n = 0, r;
  if (fd < 0) { buf[0] = 0; return -1; }
  while (n < cap - 1 && (r = api->read(fd, buf + n, (size_t)(cap - 1 - n))) > 0) n += r;
  api->close(fd);
  buf[n] = 0;
  return n;
}

static void load_all(void) {
  const char *p;
  gs_init(&G);
  if (read_all(SCORE_PATH, TXT, sizeof TXT) >= 0) gs_load(&G, TXT);
  if (read_all(PREFS_PATH, TXT, sizeof TXT) < 0) return;
  for (p = TXT; *p; p = tsv_next_line(p)) {
    if (str_starts(p, "dice ")) {
      char e[16];
      int k = 0;
      const char *q = p + 5;
      while (*q && *q != '\n' && *q != '\r' && k < 15) e[k++] = *q++;
      e[k] = 0;
      gd_parse(&D, e);
    } else if (str_starts(p, "timer ")) {
      const char *q = p + 6;
      uint32_t v = str_uint(&q);
      if (v >= 5 && v <= 5999) S.t_preset = (int)v;
    } else if (str_starts(p, "sound ")) {
      S.sound = p[6] != '0';
    }
  }
}

/* The board changed: saved soon, not on every keystroke. */
static void dirty(void) {
  S.dirty = 1;
  S.dirty_at = api->ticks_ms();
}

/* ---- sounds, made and played -------------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int snd_samples(int id) {
  int i, n = 0;
  for (i = 0; i < SND[id].count; i++) n += (int)(SND[id].n[i].ms * SND_RATE / 1000u);
  return n;
}

static void snd_path(char *out, size_t n, int id) {
  api->fmt(out, n, CAPP_CACHE "/gn_%s.wav", SND[id].name);
}

/* A sound as a WAV: each note a triangle, a square, a buzzer (two squares a
 * fifth apart, clipped hard) or noise, dying away, with a short fade at each
 * end so it does not click. */
static void make_snd(int id) {
  const Snd *s = &SND[id];
  uint8_t hdr[44];
  int16_t buf[128];
  char path[40];
  int fd, i, nb = 0;
  uint32_t seed = 0x2468ACEu, bytes = (uint32_t)snd_samples(id) * 2u;
  snd_path(path, sizeof path, id);
  fd = api->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  api->mem_set(hdr, 0, sizeof hdr);
  api->mem_cpy(hdr, "RIFF", 4);
  put_u32(hdr + 4, 36u + bytes);
  api->mem_cpy(hdr + 8, "WAVEfmt ", 8);
  put_u32(hdr + 16, 16);
  hdr[20] = 1; hdr[22] = 1;                          /* PCM, mono */
  put_u32(hdr + 24, SND_RATE);
  put_u32(hdr + 28, SND_RATE * 2u);
  hdr[32] = 2; hdr[34] = 16;
  api->mem_cpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, bytes);
  api->write(fd, hdr, sizeof hdr);
  for (i = 0; i < s->count; i++) {
    const Note *nt = &s->n[i];
    int n = (int)(nt->ms * SND_RATE / 1000u), j;
    uint32_t ph = 0, ph2 = 0, step = (uint32_t)nt->hz * 65536u / SND_RATE;
    for (j = 0; j < n; j++) {
      int amp = nt->vol * 300, v = 0;
      if (nt->wave != W_BUZZ) amp = amp * (n - j) / n;
      if (j < 24) amp = amp * j / 24;
      if (n - j < 24) amp = amp * (n - j) / 24;
      if (nt->wave == W_NOISE) {
        seed = seed * 1664525u + 1013904223u;
        v = ((int)((seed >> 16) & 0x7FFFu) - 16384) * amp / 16384;
      } else if (nt->hz) {
        uint32_t p = ph & 0xFFFFu, q = ph2 & 0xFFFFu;
        if (nt->wave == W_BUZZ) {
          v = (p < 32768u ? amp : -amp) + (q < 32768u ? amp : -amp) / 2;
          if (v > 30000) v = 30000;
          if (v < -30000) v = -30000;
          ph2 += step * 3u / 2u;
        } else if (nt->wave == W_SQR) {
          v = p < 32768u ? amp : -amp;
        } else {
          v = ((p < 32768u ? (int)p - 16384 : 49152 - (int)p) * amp) / 16384;
        }
        ph += step;
      }
      buf[nb++] = (int16_t)v;
      if (nb == 128) { api->write(fd, buf, sizeof buf); nb = 0; }
    }
  }
  if (nb) api->write(fd, buf, (size_t)nb * 2);
  api->close(fd);
}

static void ensure_sounds(void) {
  int i;
  char path[40];
  CappStat st;
  api->mkdir(CAPP_CACHE);
  for (i = 0; i < SN_COUNT; i++) {
    uint32_t want = 44u + (uint32_t)snd_samples(i) * 2u;
    snd_path(path, sizeof path, i);
    if (api->stat(path, &st) != 0 || st.is_dir || st.size != want) make_snd(i);
    if (api->stat(path, &st) == 0 && !st.is_dir && st.size == want) S.snd_have |= 1u << i;
  }
}

/* A sound, if sound is on and the speaker is free or playing one of ours:
 * music the player has on is never cut off. */
static void snd(int id) {
  char path[40];
  uint32_t now = api->ticks_ms();
  if (!S.sound || !au || !(S.snd_have & (1u << id))) return;
  if (au->state() != CAPP_AUDIO_IDLE) {
    if ((int32_t)(now - S.snd_end) >= 0) return;    /* not ours: leave it */
    au->stop();
  }
  snd_path(path, sizeof path, id);
  if (au->play(path) == 0) {
    int i, ms = 0;
    for (i = 0; i < SND[id].count; i++) ms += SND[id].n[i].ms;
    S.snd_end = now + (uint32_t)ms + 100u;
  }
}

/* ---- drawing helpers ------------------------------------------------------------- */

static CRect rc(int x, int y, int w, int h) { return capp_rect(x, y, w, h); }

static void disc(int cx, int cy, int r, uint16_t c) {
  int dy;
  for (dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
    api->fill(rc(cx - dx, cy + dy, 2 * dx + 1, 1), c);
  }
}

/* A rectangle with its corners rounded by r. */
static void rbox(int x, int y, int w, int h, int r, uint16_t c) {
  int k;
  api->fill(rc(x, y + r, w, h - 2 * r), c);
  for (k = 0; k < r; k++) {
    int in = r - k;
    int d = 0;
    while ((d + 1) * (d + 1) + in * in <= r * r) d++;
    api->fill(rc(x + r - d, y + k, w - 2 * (r - d), 1), c);
    api->fill(rc(x + r - d, y + h - 1 - k, w - 2 * (r - d), 1), c);
  }
}

static void text_c(int f, int cx, int y, const char *s, uint16_t fg, uint16_t bg) {
  int w = api->text_width(f, s);
  api->text_font(f, (int16_t)(cx - w / 2), (int16_t)y, s, fg, bg);
}

/* Cut to what fits in w pixels of the 6x8 font. */
static void fit6(char *out, const char *s, int w) {
  int n = w / 6, i;
  if (n < 1) n = 1;
  for (i = 0; s[i] && i < n && i < 15; i++) out[i] = s[i];
  out[i] = 0;
}

static void fmt_clock(char *out, int n, int32_t ms) {
  int s = (int)((ms + 999) / 1000);
  if (s < 0) s = 0;
  api->fmt(out, (size_t)n, "%d:%02d", s / 60, s % 60);
}

/* ---- the menu ---------------------------------------------------------------- */

static const char *const TILE[4] = { "Score", "Dice", "Timer", "Buzzer" };

static void tile_icon(int k, int cx, int cy, uint16_t bg) {
  switch (k) {
  case 0:                                              /* a grid with a total line */
    api->fill(rc(cx - 10, cy - 8, 20, 16), C_DIE);
    api->fill(rc(cx - 3, cy - 8, 1, 16), C_PIP);
    api->fill(rc(cx + 3, cy - 8, 1, 16), C_PIP);
    api->fill(rc(cx - 10, cy - 3, 20, 1), C_PIP);
    api->fill(rc(cx - 10, cy + 3, 20, 2), C_GOLD);
    break;
  case 1:                                              /* a die, five */
    rbox(cx - 9, cy - 9, 18, 18, 3, C_DIE);
    disc(cx - 4, cy - 4, 1, C_PIP); disc(cx + 4, cy - 4, 1, C_PIP);
    disc(cx, cy, 1, C_PIP);
    disc(cx - 4, cy + 4, 1, C_PIP); disc(cx + 4, cy + 4, 1, C_PIP);
    break;
  case 2:                                              /* a clock face */
    disc(cx, cy, 9, C_DIE);
    disc(cx, cy, 7, bg);
    api->fill(rc(cx, cy - 5, 1, 6), C_DIE);
    api->fill(rc(cx, cy, 4, 1), C_DIE);
    break;
  default:                                             /* a red button */
    disc(cx, cy + 2, 9, CAPP_RGB(120, 30, 30));
    disc(cx, cy, 9, C_RED);
    disc(cx - 3, cy - 3, 2, CAPP_RGB(255, 150, 140));
    break;
  }
}

static void tile_note(int k, char *out, int n) {
  out[0] = 0;
  switch (k) {
  case 0:
    if (G.np) api->fmt(out, (size_t)n, "%d players, round %d", G.np, G.nr + (G.nr < GS_MAXR));
    else api->fmt(out, (size_t)n, "add players");
    break;
  case 1: gd_name(&D, out, n); break;
  case 2:
    if (S.t_running || S.t_left_ms != S.t_preset * 1000) fmt_clock(out, n, S.t_left_ms);
    else fmt_clock(out, n, (int32_t)S.t_preset * 1000);
    if (S.t_running) {
      int at = (int)api->str_len(out);
      api->fmt(out + at, (size_t)(n - at), " running");
    }
    break;
  default: api->fmt(out, (size_t)n, "for Taboo"); break;
  }
}

static void paint_home(CRect c) {
  int k;
  api->fill(c, C_BG);
  text_c(S.f_uib, c.x + c.w / 2, c.y + 3, "Game Night", C_GOLD, C_BG);
  for (k = 0; k < 4; k++) {
    int x = c.x + 6 + (k % 2) * 116, y = c.y + 21 + (k / 2) * 50;
    int on = k == S.sel;
    uint16_t bg = on ? CAPP_RGB(44, 52, 78) : C_PANEL;
    char note[32];
    rbox(x, y, 112, 46, 5, on ? C_TEAL : C_LINE);
    rbox(x + 1, y + 1, 110, 44, 4, bg);
    tile_icon(k, x + 17, y + 23, bg);
    api->text_font(S.f_uib, (int16_t)(x + 34), (int16_t)(y + 7), TILE[k], C_TEXT, bg);
    tile_note(k, note, sizeof note);
    {
      char cut[20];
      fit6(cut, note, 74);
      api->text((int16_t)(x + 34), (int16_t)(y + 27), cut, k == 2 && S.t_running ? C_GOLD : C_DIM, bg);
    }
    {
      char num[4];
      api->fmt(num, sizeof num, "%d", k + 1);
      api->text((int16_t)(x + 102), (int16_t)(y + 4), num, C_DIM, bg);
    }
  }
  footer_paint(api, c, S.sound ? "enter open  1-4  m sound off" : "enter open  1-4  m sound on");
}

/* ---- the scoreboard ------------------------------------------------------------ */

#define SC_RW     20                    /* the round numbers' column */
#define SC_ROW_H  11
#define SC_HEAD_H 14

static int score_cw(void) {
  int w = (240 - SC_RW) / (G.np ? G.np : 1);
  return w < 36 ? 36 : w;
}

static int score_vis_cols(void) { return (240 - SC_RW) / score_cw(); }

static int score_rows(CRect c) { return (c.h - FOOT_H - SC_HEAD_H - SC_HEAD_H) / SC_ROW_H; }

static void score_scroll(CRect c) {
  int rows = score_rows(c), vc = score_vis_cols();
  if (S.row >= 0) {
    if (S.row < S.top) S.top = S.row;
    if (S.row >= S.top + rows) S.top = S.row - rows + 1;
  }
  if (S.top < 0) S.top = 0;
  if (S.col < S.left) S.left = S.col;
  if (S.col >= S.left + vc) S.left = S.col - vc + 1;
  if (S.left < 0) S.left = 0;
}

static void cell_text(char *out, int n, int r, int c) {
  out[0] = 0;
  if (S.editing && r == S.row && c == S.col) { api->fmt(out, (size_t)n, "%s_", S.edit); return; }
  if (r < G.nr && G.cell[r][c] != GS_EMPTY) api->fmt(out, (size_t)n, "%d", (int)G.cell[r][c]);
}

static const char *score_foot(void) {
  static char s[48];
  switch (S.ask) {
  case ASK_NEW:        return "new game, same players? y/n";
  case ASK_DEL_ROUND:  api->fmt(s, sizeof s, "delete round %d? y/n", S.row + 1); return s;
  case ASK_DEL_PLAYER: api->fmt(s, sizeof s, "remove %s? y/n", G.name[S.col]); return s;
  }
  if (S.renaming >= 0) { api->fmt(s, sizeof s, "name: %s_  enter ok", S.input); return s; }
  if (S.spin) return "who goes first...";
  if (S.editing) return "enter next  bksp  esc cancel";
  if (S.row < 0) return "e rename  d remove  p add  s shuffle";
  return "0-9 score  f first  n new  l low";
}

static void paint_score(CRect c) {
  int cw = score_cw(), vc = score_vis_cols(), rows, r, k, y;
  unsigned lead = gs_leaders(&G);
  char buf[16];
  score_scroll(c);
  rows = score_rows(c);
  api->fill(c, C_BG);
  /* the names */
  api->fill(rc(c.x, c.y, c.w, SC_HEAD_H), C_PANEL);
  api->text((int16_t)(c.x + 2), (int16_t)(c.y + 3), G.low ? "lo" : "#", C_DIM, C_PANEL);
  for (k = 0; k < vc && S.left + k < G.np; k++) {
    int col = S.left + k, x = c.x + SC_RW + k * cw;
    int sel = S.row < 0 && col == S.col;
    int spun = S.spin && col == S.spin_col;
    uint16_t bg = sel || spun ? CAPP_RGB(44, 52, 78) : C_PANEL;
    char nm[16];
    api->fill(rc(x, c.y, cw, SC_HEAD_H), bg);
    fit6(nm, G.name[col], cw - (col == G.first ? 8 : 2));
    if (col == G.first && !S.spin) {
      int ty = c.y + 3;                               /* a little triangle: goes first */
      api->fill(rc(x + 1, ty, 1, 7), C_GOLD);
      api->fill(rc(x + 2, ty + 1, 1, 5), C_GOLD);
      api->fill(rc(x + 3, ty + 2, 1, 3), C_GOLD);
      api->fill(rc(x + 4, ty + 3, 1, 1), C_GOLD);
    }
    if (S.renaming == col) api->fmt(nm, sizeof nm, "%s", "...");
    api->text((int16_t)(x + (col == G.first && !S.spin ? 7 : 2)), (int16_t)(c.y + 3), nm,
              spun ? C_GOLD : (lead >> col & 1) ? C_GOLD : C_TEXT, bg);
    api->fill(rc(x, c.y, 1, c.h - FOOT_H), C_LINE);
  }
  /* the rounds, and the row for the next one */
  y = c.y + SC_HEAD_H;
  for (r = S.top; r < S.top + rows && r <= G.nr && r < GS_MAXR; r++, y += SC_ROW_H) {
    uint16_t rowbg = (r & 1) ? CAPP_RGB(20, 23, 33) : C_BG;
    api->fill(rc(c.x, y, c.w, SC_ROW_H), rowbg);
    api->fmt(buf, sizeof buf, r < G.nr ? "%d" : "+", r + 1);
    api->text((int16_t)(c.x + 2), (int16_t)(y + 2), buf, C_DIM, rowbg);
    for (k = 0; k < vc && S.left + k < G.np; k++) {
      int col = S.left + k, x = c.x + SC_RW + k * cw;
      int sel = r == S.row && col == S.col;
      uint16_t bg = sel ? (S.editing ? CAPP_RGB(70, 60, 30) : CAPP_RGB(40, 70, 72)) : rowbg;
      api->fill(rc(x + 1, y, cw - 1, SC_ROW_H), bg);
      cell_text(buf, sizeof buf, r, col);
      if (buf[0]) {
        int w = (int)api->str_len(buf) * 6;
        api->text((int16_t)(x + cw - 3 - w), (int16_t)(y + 2), buf,
                  r < G.nr && G.cell[r][col] < 0 ? CAPP_RGB(255, 140, 120) : C_TEXT, bg);
      }
      api->fill(rc(x, y, 1, SC_ROW_H), C_LINE);
    }
  }
  /* the totals */
  y = c.y + c.h - FOOT_H - SC_HEAD_H;
  api->fill(rc(c.x, y, c.w, SC_HEAD_H), C_PANEL);
  api->fill(rc(c.x, y, c.w, 1), C_GOLD);
  api->text((int16_t)(c.x + 2), (int16_t)(y + 3), "=", C_GOLD, C_PANEL);
  for (k = 0; k < vc && S.left + k < G.np; k++) {
    int col = S.left + k, x = c.x + SC_RW + k * cw, w;
    int win = lead >> col & 1;
    uint16_t bg = win ? CAPP_RGB(70, 58, 20) : C_PANEL;
    api->fill(rc(x + 1, y + 1, cw - 1, SC_HEAD_H - 1), bg);
    api->fmt(buf, sizeof buf, "%d", (int)gs_total(&G, col));
    w = (int)api->str_len(buf) * 6;
    api->text((int16_t)(x + cw - 3 - w), (int16_t)(y + 4), buf, win ? C_GOLD : C_TEXT, bg);
  }
  if (G.np > vc) {                                     /* more to the right or left */
    if (S.left > 0) api->text((int16_t)(c.x + SC_RW - 6), (int16_t)(c.y + 3), "<", C_TEAL, C_PANEL);
    if (S.left + vc < G.np) api->text((int16_t)(c.x + c.w - 6), (int16_t)(c.y + 3), ">", C_TEAL, C_PANEL);
  }
  footer_paint(api, c, score_foot());
}

static void paint_players(CRect c) {
  int k, y;
  char line[40];
  api->fill(c, C_BG);
  text_c(S.f_uib, c.x + c.w / 2, c.y + 2, "Players", C_GOLD, C_BG);
  api->text((int16_t)(c.x + 8), (int16_t)(c.y + 20), "players or teams, in seating order",
            C_DIM, C_BG);
  for (k = 0, y = c.y + 32; k < G.np; k++, y += 10) {
    api->fmt(line, sizeof line, "%d  %s", k + 1, G.name[k]);
    api->text((int16_t)(c.x + 14 + (k >= 4 ? 112 : 0)), (int16_t)(y - (k >= 4 ? 40 : 0)), line,
              C_TEXT, C_BG);
  }
  y = c.y + 78;
  rbox(c.x + 8, y, c.w - 16, 18, 3, C_LINE);
  rbox(c.x + 9, y + 1, c.w - 18, 16, 3, C_PANEL);
  if (G.np < GS_MAXP) api->fmt(line, sizeof line, "+ %s_", S.input);
  else api->fmt(line, sizeof line, "that is the most: %d", GS_MAXP);
  api->text((int16_t)(c.x + 14), (int16_t)(y + 5), line, C_TEXT, C_PANEL);
  footer_paint(api, c, G.np ? "enter add, or done  bksp undo" : "type a name, enter adds it");
}

/* Commit the number being typed. */
static void score_commit(void) {
  const char *p = S.edit;
  int neg = 0, v;
  uint32_t u;
  if (!S.editing) return;
  S.editing = 0;
  if (*p == '-') { neg = 1; p++; }
  if (!*p) { if (!neg) gs_clear(&G, S.row, S.col); dirty(); return; }
  u = str_uint(&p);
  v = neg ? -(int)u : (int)u;
  if (gs_set(&G, S.row, S.col, v) == 0) dirty();
}

static void score_next(void) {
  if (++S.col >= G.np) {
    S.col = 0;
    if (S.row < G.nr && S.row + 1 < GS_MAXR) S.row++;
  }
}

static int key_players(uint8_t k) {
  int n = (int)api->str_len(S.input);
  if (k == CAPP_KEY_ESC) {
    S.input[0] = 0;
    S.screen = G.np ? SC_SCORE : SC_HOME;
    return 1;
  }
  if (k == CAPP_KEY_ENTER) {
    if (n) {
      if (gs_add_player(&G, S.input) >= 0) { dirty(); snd(SN_CLICK); }
      S.input[0] = 0;
    } else if (G.np) {
      S.screen = SC_SCORE;
      S.row = G.nr < GS_MAXR ? G.nr : GS_MAXR - 1;
      S.col = 0;
    }
    return 1;
  }
  if (k == CAPP_KEY_BACK) {
    if (n) S.input[n - 1] = 0;
    else if (G.np && !gs_any(&G)) { gs_del_player(&G, G.np - 1); dirty(); }
    return 1;
  }
  if (k >= 32 && k < 127 && n < GS_NAME && G.np < GS_MAXP) {
    S.input[n] = (char)k;
    S.input[n + 1] = 0;
  }
  return 1;
}

static int key_rename(uint8_t k) {
  int n = (int)api->str_len(S.input);
  if (k == CAPP_KEY_ESC) { S.renaming = -1; return 1; }
  if (k == CAPP_KEY_ENTER) {
    if (n) { gs_rename(&G, S.renaming, S.input); dirty(); }
    S.renaming = -1;
    return 1;
  }
  if (k == CAPP_KEY_BACK) { if (n) S.input[n - 1] = 0; return 1; }
  if (k >= 32 && k < 127 && n < GS_NAME) { S.input[n] = (char)k; S.input[n + 1] = 0; }
  return 1;
}

static int key_score(uint8_t k) {
  int n = S.editing ? (int)api->str_len(S.edit) : 0;
  if (S.renaming >= 0) return key_rename(k);
  if (S.ask) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) {
      if (S.ask == ASK_NEW) { gs_new_game(&G); S.row = 0; S.col = 0; S.top = 0; }
      else if (S.ask == ASK_DEL_ROUND) gs_del_round(&G, S.row);
      else { gs_del_player(&G, S.col); if (S.col >= G.np) S.col = G.np - 1; }
      dirty();
    }
    if (a != CONFIRM_WAIT) S.ask = ASK_NONE;
    if (!G.np) { S.screen = SC_PLAYERS; S.col = 0; }
    return 1;
  }
  if (S.spin) return 1;                                /* let the spin finish */
  if (api->key_repeat() && k != CAPP_KEY_BACK && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 1;
  if (((k >= '0' && k <= '9') || (k == '-' && !n)) && S.row >= 0) {
    if (!S.editing) { S.editing = 1; S.edit[0] = 0; n = 0; }
    if (n < 5) { S.edit[n] = (char)k; S.edit[n + 1] = 0; }
    return 1;
  }
  switch (k) {
  case CAPP_KEY_ENTER:
    if (S.row < 0) { S.row = 0; return 1; }
    score_commit();
    score_next();
    return 1;
  case CAPP_KEY_BACK:
    if (S.editing) { if (n) S.edit[n - 1] = 0; else S.editing = 0; }
    else if (S.row >= 0 && S.row < G.nr) { gs_clear(&G, S.row, S.col); dirty(); }
    return 1;
  case CAPP_KEY_ESC:
    if (S.editing) { S.editing = 0; return 1; }
    S.screen = SC_HOME;
    return 1;
  case CAPP_KEY_UP:    score_commit(); if (S.row > -1) S.row--; return 1;
  case CAPP_KEY_DOWN:  score_commit(); if (S.row < G.nr && S.row + 1 < GS_MAXR) S.row++; return 1;
  case CAPP_KEY_LEFT:  score_commit(); if (S.col > 0) S.col--; return 1;
  case CAPP_KEY_RIGHT: score_commit(); if (S.col + 1 < G.np) S.col++; return 1;
  case 0x7F:
  case 'd':
    score_commit();
    if (S.row < 0) S.ask = ASK_DEL_PLAYER;
    else if (S.row < G.nr) S.ask = ASK_DEL_ROUND;
    return 1;
  case 'e':
    score_commit();
    S.renaming = S.col;
    api->fmt(S.input, sizeof S.input, "%s", G.name[S.col]);
    return 1;
  case 'n': score_commit(); if (gs_any(&G) || G.first >= 0) S.ask = ASK_NEW; return 1;
  case 'f':
    score_commit();
    if (G.np < 2) return 1;
    S.spin = 14 + (int)(rnd32() % (uint32_t)G.np);   /* lands at random: a spin, not a pick */
    S.spin_col = (int)(rnd32() % (uint32_t)G.np);
    S.spin_at = api->ticks_ms();
    return 1;
  case 'g': G.first = G.first == S.col ? -1 : S.col; dirty(); return 1;
  case 'l': G.low = !G.low; dirty(); return 1;
  case 'p': score_commit(); S.screen = SC_PLAYERS; S.input[0] = 0; return 1;
  case 's': {
    int i;
    score_commit();
    if (gs_any(&G)) return 1;                        /* seats are set once the scores start */
    for (i = G.np - 1; i > 0; i--) gs_swap(&G, i, (int)(rnd32() % (uint32_t)(i + 1)));
    dirty();
    return 1;
  }
  default: return 0;
  }
}

/* The spin through the names, slowing, landing on who goes first. */
static int tick_spin(uint32_t now) {
  int gap;
  if (!S.spin) return 0;
  gap = 60 + (14 - (S.spin < 14 ? S.spin : 14)) * 18;
  if ((int32_t)(now - S.spin_at) < gap) return 0;
  S.spin_at = now;
  S.spin_col = (S.spin_col + 1) % G.np;
  snd(SN_CLICK);
  if (--S.spin == 0) {
    G.first = S.spin_col;
    snd(SN_DING);
    dirty();
  }
  return 1;
}

/* ---- dice ---------------------------------------------------------------------- */

#define ROLL_MS 650u

static void start_roll(void) {
  if (S.rolling) return;
  S.rolling = 1;
  S.roll_at = api->ticks_ms();
  S.roll_end = S.roll_at + ROLL_MS;
  snd(SN_RATTLE);
}

static void end_roll(void) {
  int i;
  S.rolling = 0;
  gd_roll(&D);
  for (i = 5; i > 0; i--) S.hist[i] = S.hist[i - 1];
  S.hist[0] = gd_total(&D);
  if (S.nhist < 6) S.nhist++;
  snd(SN_CLICK);
}

static const int8_t PIPS[7][7][2] = {
  { { 0 } },
  { { 0, 0 } },
  { { -1, -1 }, { 1, 1 } },
  { { -1, -1 }, { 0, 0 }, { 1, 1 } },
  { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } },
  { { -1, -1 }, { 1, -1 }, { 0, 0 }, { -1, 1 }, { 1, 1 } },
  { { -1, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 1, 1 } },
};
static const uint8_t NPIPS[7] = { 0, 1, 2, 3, 4, 5, 6 };

static void paint_die(int x, int y, int sz, int i) {
  int v = D.v[i], held = D.hold[i];
  uint16_t face = held ? CAPP_RGB(200, 236, 230) : C_DIE;
  char s[8];
  if (held) rbox(x - 2, y - 2, sz + 4, sz + 4, sz / 5 + 2, C_TEAL);
  if (D.sides == 2) {                                  /* a coin */
    disc(x + sz / 2, y + sz / 2, sz / 2, C_GOLD);
    disc(x + sz / 2, y + sz / 2, sz / 2 - 3, CAPP_RGB(232, 180, 60));
    if (v) text_c(S.f_uib, x + sz / 2, y + sz / 2 - 7, v == 1 ? "H" : "T", C_PIP,
                  CAPP_RGB(232, 180, 60));
    return;
  }
  rbox(x, y, sz, sz, sz / 5, face);
  if (!v) { text_c(S.f_uib, x + sz / 2, y + sz / 2 - 7, "?", C_DIM, face); return; }
  if (D.sides == 6) {
    int k, r = sz >= 34 ? 3 : 2, off = sz * 3 / 10;
    for (k = 0; k < NPIPS[v]; k++)
      disc(x + sz / 2 + PIPS[v][k][0] * off, y + sz / 2 + PIPS[v][k][1] * off, r, C_PIP);
  } else {
    api->fmt(s, sizeof s, "%d", v);
    if (sz >= 34 && S.f_num >= 0 && api->text_width(S.f_num, s) < sz - 4)
      text_c(S.f_num, x + sz / 2, y + (sz - api->font_height(S.f_num)) / 2, s, C_PIP, face);
    else
      text_c(S.f_uib, x + sz / 2, y + (sz - api->font_height(S.f_uib)) / 2, s, C_PIP, face);
  }
}

static void paint_dice(CRect c) {
  char name[16], s[48];
  int n = D.n, rowsn = n <= 3 ? 1 : 2, per = (n + rowsn - 1) / rowsn;
  int gap = 6, top = c.y + 18, area = 86, sz, i, y0;
  /* as big as fits beside the total: 146 px across, each with its number under it */
  sz = (146 - (per - 1) * gap) / per;
  if (sz > (area - rowsn * 9 - (rowsn - 1) * gap) / rowsn) sz = (area - rowsn * 9 - (rowsn - 1) * gap) / rowsn;
  if (sz > 40) sz = 40;
  api->fill(c, C_BG);
  gd_name(&D, name, sizeof name);
  api->text_font(S.f_uib, (int16_t)(c.x + 6), (int16_t)(c.y + 2), name, C_GOLD, C_BG);
  if (S.motion_ok) api->text((int16_t)(c.x + c.w - 6 * 11), (int16_t)(c.y + 5), "shake: roll", C_DIM, C_BG);
  y0 = top + (area - (rowsn * (sz + 9) + (rowsn - 1) * gap)) / 2;
  for (i = 0; i < n; i++) {
    int row = i / per, k = i % per, inrow = row ? n - per : per;
    int x = c.x + 3 + (146 - (inrow * sz + (inrow - 1) * gap)) / 2 + k * (sz + gap);
    int y = y0 + row * (sz + 9 + gap);
    paint_die(x, y, sz, i);
    if (D.v[i]) {                                      /* its number: 1-9 holds it */
      char num[4];
      api->fmt(num, sizeof num, "%d", i + 1);
      api->text((int16_t)(x + sz / 2 - 3), (int16_t)(y + sz + 2), num, D.hold[i] ? C_TEAL : C_LINE, C_BG);
    }
  }
  /* the total */
  api->fill(rc(c.x + 152, top, 1, area), C_LINE);
  if (gd_rolled(&D) && !S.rolling && D.sides == 2) {   /* coins: heads and tails, not a sum */
    int h = 0;
    for (i = 0; i < D.n; i++) h += D.v[i] == 1;
    api->fmt(s, sizeof s, "%d heads", h);
    text_c(S.f_uib, c.x + 196, top + 26, s, C_GOLD, C_BG);
    api->fmt(s, sizeof s, "%d tails", D.n - h);
    text_c(S.f_uib, c.x + 196, top + 44, s, C_TEXT, C_BG);
  } else if (gd_rolled(&D) && !S.rolling) {
    api->fmt(s, sizeof s, "%d", (int)gd_total(&D));
    text_c(S.f_num >= 0 && api->text_width(S.f_num, s) < 80 ? S.f_num : S.f_uib,
           c.x + 196, top + 22, s, C_TEXT, C_BG);
    if (D.mod) {
      api->fmt(s, sizeof s, "%+d", D.mod);
      text_c(-1, c.x + 196, top + 58, s, C_DIM, C_BG);
    }
  } else {
    text_c(S.f_uib, c.x + 196, top + 30, S.rolling ? "..." : "roll", C_DIM, C_BG);
  }
  /* the last few */
  if (S.nhist > 1) {
    int k, o = 0;
    o = api->fmt(s, sizeof s, "before:");
    for (k = 1; k < S.nhist && o < 40; k++) o += api->fmt(s + o, sizeof s - (size_t)o, " %d", (int)S.hist[k]);
    api->text((int16_t)(c.x + 6), (int16_t)(c.y + c.h - FOOT_H - 11), s, C_DIM, C_BG);
  }
  if (S.typing) {
    api->fmt(s, sizeof s, "dice: %s_  (2d6, d20, 3d6+2)", S.dtext);
    footer_paint(api, c, s);
  } else {
    footer_paint(api, c, "spc roll  ^v count  <> sides  1-9 hold");
  }
}

static int key_dice(uint8_t k) {
  if (S.typing) {
    int n = (int)api->str_len(S.dtext);
    if (k == CAPP_KEY_ESC) { S.typing = 0; return 1; }
    if (k == CAPP_KEY_ENTER) {
      if (gd_parse(&D, S.dtext) == 0) { S.typing = 0; save_prefs(); }
      else snd(SN_SKIP);
      return 1;
    }
    if (k == CAPP_KEY_BACK) { if (n) S.dtext[n - 1] = 0; return 1; }
    if (k > 32 && k < 127 && n < 10) { S.dtext[n] = (char)k; S.dtext[n + 1] = 0; }
    return 1;
  }
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 1;
  switch (k) {
  case ' ': case CAPP_KEY_ENTER: case 'r': start_roll(); return 1;
  case CAPP_KEY_UP:    gd_set_n(&D, D.n + 1); save_prefs(); return 1;
  case CAPP_KEY_DOWN:  gd_set_n(&D, D.n - 1); save_prefs(); return 1;
  case CAPP_KEY_RIGHT: gd_step_sides(&D, 1); save_prefs(); return 1;
  case CAPP_KEY_LEFT:  gd_step_sides(&D, -1); save_prefs(); return 1;
  case 'e': S.typing = 1; S.dtext[0] = 0; return 1;
  case 'u': gd_unhold(&D); return 1;
  case CAPP_KEY_ESC: S.screen = SC_HOME; return 1;
  default:
    if (k >= '1' && k <= '9' && k - '1' < D.n && D.v[k - '1']) {
      D.hold[k - '1'] ^= 1;
      return 1;
    }
    return 0;
  }
}

/* A shake: the acceleration well off 1 g, or a quick turn. */
static int shaken(void) {
  CappMotion m;
  int32_t a;
  if (!api->motion || api->motion(&m) != 0) return 0;
  S.motion_ok = 1;
  a = (int32_t)m.ax * m.ax + (int32_t)m.ay * m.ay + (int32_t)m.az * m.az;
  return a > 1900 * 1900 || a < 300 * 300 || m.gx > 4000 || m.gx < -4000 ||
         m.gy > 4000 || m.gy < -4000 || m.gz > 4000 || m.gz < -4000;
}

static int tick_dice(uint32_t now) {
  if (S.rolling) {
    if ((int32_t)(now - S.roll_end) >= 0) { end_roll(); return 1; }
    if ((int32_t)(now - S.roll_at) >= 70) { S.roll_at = now; gd_jiggle(&D); return 1; }
    return 0;
  }
  if (S.screen == SC_DICE && (int32_t)(now - S.shake_at) >= 40) {
    S.shake_at = now;
    if (S.shake_cd > 0) S.shake_cd--;
    else if (shaken()) { start_roll(); S.shake_cd = 25; return 1; }
  }
  return 0;
}

/* ---- the timer ----------------------------------------------------------------- */

static void timer_reset(void) {
  S.t_running = 0;
  S.t_done = 0;
  S.t_left_ms = (int32_t)S.t_preset * 1000;
  S.t_last_sec = -1;
  api->keep_awake(0);
}

static void timer_toggle(void) {
  if (S.t_done) { timer_reset(); return; }
  if (S.t_left_ms <= 0) timer_reset();
  S.t_running = !S.t_running;
  S.t_last = api->ticks_ms();
  api->keep_awake(S.t_running);
  snd(SN_CLICK);
}

static void paint_timer(CRect c) {
  char s[16];
  int32_t total = (int32_t)S.t_preset * 1000;
  int low = S.t_left_ms <= 10000 && S.t_running;
  uint16_t fg = S.t_done ? ((S.t_flash / 250u) & 1 ? C_RED : C_TEXT) : low ? C_RED : C_TEXT;
  int f = S.f_big >= 0 ? S.f_big : S.f_uib, w, barw;
  api->fill(c, S.t_done && (S.t_flash / 250u) & 1 ? CAPP_RGB(60, 16, 16) : C_BG);
  fmt_clock(s, sizeof s, S.t_left_ms);
  w = api->text_width(f, s);
  api->text_font(f, (int16_t)(c.x + (c.w - w) / 2), (int16_t)(c.y + 18), s, fg,
                 S.t_done && (S.t_flash / 250u) & 1 ? CAPP_RGB(60, 16, 16) : C_BG);
  barw = total > 0 ? (int)((int32_t)(c.w - 24) * (S.t_left_ms > 0 ? S.t_left_ms : 0) / total) : 0;
  api->fill(rc(c.x + 12, c.y + 88, c.w - 24, 6), C_PANEL);
  api->fill(rc(c.x + 12, c.y + 88, barw, 6), low || S.t_done ? C_RED : C_TEAL);
  fmt_clock(s, sizeof s, total);
  {
    char line[40];
    api->fmt(line, sizeof line, S.t_done ? "time's up!" : S.t_running ? "running  (set %s)" :
             S.t_left_ms != total ? "paused  (set %s)" : "set to %s", s);
    text_c(S.f_ui, c.x + c.w / 2, c.y + 100, line, S.t_done ? C_RED : C_DIM, C_BG);
  }
  footer_paint(api, c, S.t_done ? "any key: stop" : "spc start/pause  enter reset  <> ^v");
}

static int key_timer(uint8_t k) {
  if (S.t_done) { timer_reset(); return 1; }
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 1;
  switch (k) {
  case ' ': timer_toggle(); return 1;
  case CAPP_KEY_ENTER: case 'r': timer_reset(); return 1;
  case CAPP_KEY_LEFT: case CAPP_KEY_RIGHT: {
    int i, at = 0, dir = k == CAPP_KEY_RIGHT ? 1 : -1;
    if (S.t_running) return 1;
    for (i = 0; i < NPRESET; i++) if (PRESETS[i] <= S.t_preset) at = i;
    if (PRESETS[at] != S.t_preset && dir > 0) at--;
    at += dir;
    if (at < 0) at = 0;
    if (at >= NPRESET) at = NPRESET - 1;
    S.t_preset = PRESETS[at];
    timer_reset();
    save_prefs();
    return 1;
  }
  case CAPP_KEY_UP: case CAPP_KEY_DOWN:
    if (S.t_running) return 1;
    S.t_preset += k == CAPP_KEY_UP ? 15 : -15;
    if (S.t_preset < 15) S.t_preset = 15;
    if (S.t_preset > 5985) S.t_preset = 5985;
    timer_reset();
    save_prefs();
    return 1;
  case CAPP_KEY_ESC: S.screen = SC_HOME; return 1;
  default: return 0;
  }
}

/* The timer runs whatever screen is up; it repaints its own screen, the
 * buzzer's corner and the menu's tile. */
static int tick_timer(uint32_t now) {
  int sec, show = S.screen == SC_TIMER || S.screen == SC_BUZZ || S.screen == SC_HOME;
  if (S.t_done) {
    uint32_t was = S.t_flash / 250u;
    S.t_flash = now;
    if ((now / 250u) != was) {
      if ((now / 250u) % 8u == 0) snd(SN_ALARM);
      return S.screen == SC_TIMER;
    }
    return 0;
  }
  if (!S.t_running) return 0;
  S.t_left_ms -= (int32_t)(now - S.t_last);
  S.t_last = now;
  sec = (int)((S.t_left_ms + 999) / 1000);
  if (S.t_left_ms <= 0) {
    S.t_left_ms = 0;
    S.t_running = 0;
    S.t_done = 1;
    S.t_flash = now;
    snd(SN_ALARM);
    if (S.screen != SC_TIMER) S.screen = SC_TIMER;    /* time's up: show it */
    return 1;
  }
  if (sec != S.t_last_sec) {
    S.t_last_sec = sec;
    if (sec <= 10) snd(SN_TICK);
    return show;
  }
  return 0;
}

/* ---- the buzzer ----------------------------------------------------------------- */

static void buzz(int kind) {
  S.b_flash = 1;
  S.b_kind = kind;
  S.b_until = api->ticks_ms() + (kind ? 350u : 900u);
  if (kind == 0) { S.b_taboo++; snd(SN_BUZZ); }
  else if (kind == 1) { S.b_correct++; snd(SN_DING); }
  else { S.b_skip++; snd(SN_SKIP); }
}

static void paint_buzz(CRect c) {
  static const uint16_t FL[3] = { CAPP_RGB(150, 24, 24), CAPP_RGB(26, 110, 46), CAPP_RGB(34, 60, 130) };
  uint16_t bg = S.b_flash ? FL[S.b_kind] : C_BG;
  char s[40];
  int cx = c.x + 70, cy = c.y + 58, pressed = S.b_flash && S.b_kind == 0;
  api->fill(c, bg);
  /* the button: a red dome on a dark base, lower while pressed */
  disc(cx, cy + 10, 40, CAPP_RGB(40, 40, 48));
  disc(cx, cy + 6, 36, CAPP_RGB(110, 20, 20));
  disc(cx, cy + (pressed ? 6 : 0), 34, C_RED);
  disc(cx - 10, cy - 12 + (pressed ? 6 : 0), 8, CAPP_RGB(255, 140, 130));
  text_c(S.f_uib, cx, cy - 7 + (pressed ? 6 : 0), "BUZZ", C_TEXT, C_RED);
  /* the counts */
  api->fmt(s, sizeof s, "%d", S.b_correct);
  text_c(S.f_num >= 0 ? S.f_num : S.f_uib, c.x + 190, c.y + 4, s, C_GREEN, bg);
  text_c(-1, c.x + 190, c.y + 36, "correct", C_DIM, bg);
  api->fmt(s, sizeof s, "taboo %d   skip %d", S.b_taboo, S.b_skip);
  text_c(-1, c.x + 190, c.y + 52, s, C_TEXT, bg);
  if (S.t_running || S.t_done || S.t_left_ms != (int32_t)S.t_preset * 1000) {
    char t[12];
    fmt_clock(t, sizeof t, S.t_left_ms);
    text_c(S.f_uib, c.x + 190, c.y + 72, t, S.t_left_ms <= 10000 ? C_RED : C_GOLD, bg);
    text_c(-1, c.x + 190, c.y + 90, S.t_running ? "t: pause" : "t: go on", C_DIM, bg);
  } else {
    text_c(-1, c.x + 190, c.y + 80, "t: start the timer", C_DIM, bg);
  }
  footer_paint(api, c, "spc buzz  c correct  s skip  n reset");
}

static int key_buzz(uint8_t k) {
  if (api->key_repeat()) return 1;
  switch (k) {
  case ' ': case CAPP_KEY_ENTER: case 'b': buzz(0); return 1;
  case 'c': buzz(1); return 1;
  case 's': buzz(2); return 1;
  case 't': timer_toggle(); return 1;
  case 'n': S.b_correct = S.b_taboo = S.b_skip = 0; return 1;
  case CAPP_KEY_ESC: S.screen = SC_HOME; return 1;
  default: return 0;
  }
}

/* ---- the app ------------------------------------------------------------------- */

static void open_tool(int k) {
  S.sel = k;
  switch (k) {
  case 0:
    if (!G.np) { S.screen = SC_PLAYERS; S.input[0] = 0; }
    else {
      S.screen = SC_SCORE;
      S.row = G.nr < GS_MAXR ? G.nr : GS_MAXR - 1;
      S.col = 0;
    }
    break;
  case 1: S.screen = SC_DICE; break;
  case 2: S.screen = SC_TIMER; break;
  default: S.screen = SC_BUZZ; break;
  }
}

static int key_home(uint8_t k) {
  if (api->key_repeat()) return 1;
  switch (k) {
  case CAPP_KEY_LEFT:  if (S.sel % 2) S.sel--; return 1;
  case CAPP_KEY_RIGHT: if (!(S.sel % 2)) S.sel++; return 1;
  case CAPP_KEY_UP:    if (S.sel >= 2) S.sel -= 2; return 1;
  case CAPP_KEY_DOWN:  if (S.sel < 2) S.sel += 2; return 1;
  case CAPP_KEY_ENTER: case ' ': open_tool(S.sel); return 1;
  case '1': case '2': case '3': case '4': open_tool(k - '1'); return 1;
  case 'm':
    S.sound = !S.sound;
    save_prefs();
    snd(SN_CLICK);
    return 1;
  default: return 0;                                   /* Esc: the top level keeps it */
  }
}

static void app_paint(void *st, CRect c) {
  (void)st;
  switch (S.screen) {
  case SC_SCORE:   paint_score(c); break;
  case SC_PLAYERS: paint_players(c); break;
  case SC_DICE:    paint_dice(c); break;
  case SC_TIMER:   paint_timer(c); break;
  case SC_BUZZ:    paint_buzz(c); break;
  default:         paint_home(c); break;
  }
}

static int app_wants_text(void *st);

/* j, anywhere nothing is being typed: thinking music while someone takes
 * their time; j again stops it. */
static int thinking(void) {
  return S.think && au && au->state() == CAPP_AUDIO_PLAYING &&
         (int32_t)(api->ticks_ms() - S.snd_end) < 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if ((k == 'j' || k == 'J') && !app_wants_text(0) && !S.ask && !S.editing) {
    if (thinking()) { au->stop(); S.think = 0; }
    else { snd(SN_THINK); S.think = 1; }
    return 1;
  }
  switch (S.screen) {
  case SC_SCORE:   return key_score(k);
  case SC_PLAYERS: return key_players(k);
  case SC_DICE:    return key_dice(k);
  case SC_TIMER:   return key_timer(k);
  case SC_BUZZ:    return key_buzz(k);
  default:         return key_home(k);
  }
}

static int app_wants_text(void *st) {
  (void)st;
  return S.screen == SC_PLAYERS || (S.screen == SC_SCORE && S.renaming >= 0) ||
         (S.screen == SC_DICE && S.typing);
}

static int app_tick(void *st, uint32_t now) {
  int r = 0;
  (void)st;
  r |= tick_timer(now);
  r |= tick_dice(now) && S.screen == SC_DICE;
  r |= tick_spin(now) && S.screen == SC_SCORE;
  if (S.b_flash && (int32_t)(now - S.b_until) >= 0) { S.b_flash = 0; r |= S.screen == SC_BUZZ; }
  if (S.dirty && now - S.dirty_at > 800u) { S.dirty = 0; save_score(); }
  return r;
}

/* The button on top: the buzzer on the buzzer, a roll on the dice, start and
 * pause on the timer. */
static int app_button(void *st, int event, const char *text) {
  (void)st; (void)text;
  if (event == CAPP_G0_ASK)
    return S.screen == SC_BUZZ || S.screen == SC_DICE || S.screen == SC_TIMER ? CAPP_G0_PRESS
                                                                              : CAPP_G0_NONE;
  if (event == CAPP_G0_PRESSED) {
    if (S.screen == SC_BUZZ) buzz(0);
    else if (S.screen == SC_DICE) start_roll();
    else if (S.screen == SC_TIMER) timer_toggle();
    return 1;
  }
  return 0;
}

/* ---- commands: "roll 2d6", "scores" -------------------------------------------- */

enum { ACT_ROLL = 1, ACT_SCORES };
static const CappParam P_DICE[] = { { "dice", CAPP_ARG_TEXT, "what to roll: 2d6, d20, 3d6+2" } };
static const CappAction ACTIONS[] = {
  { "roll", "Roll", 0, 0, ACT_ROLL, "roll dice and say what came up", P_DICE, 1, CAPP_CMD_YES },
  { "scores", "Scores", 0, 0, ACT_SCORES, "the scoreboard's totals", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int app_action(void *st, int a) { (void)st; (void)a; return 0; }

static int app_command(void *st, int action, int argc, const char *const *argv, char *out,
                       size_t n) {
  size_t o = 0;
  int i;
  (void)st;
  if (action == ACT_ROLL) {
    GDice d;
    gd_init(&d, api->ticks_ms() ^ api->epoch() ^ 0x9E3779B9u);
    if (argc > 0 && argv[0][0] && gd_parse(&d, argv[0]) != 0) {
      api->fmt(out, n, "not dice: %s (try 2d6, d20, 3d6+2)", argv[0]);
      return -1;
    }
    gd_roll(&d);
    o = (size_t)api->fmt(out, n, "%d", (int)gd_total(&d));
    if (d.n > 1 || d.mod) {
      o += (size_t)api->fmt(out + o, n - o, " (");
      for (i = 0; i < d.n && o + 8 < n; i++) o += (size_t)api->fmt(out + o, n - o, i ? " %d" : "%d", d.v[i]);
      if (d.mod) o += (size_t)api->fmt(out + o, n - o, " %+d", d.mod);
      api->fmt(out + o, n - o, ")");
    }
    return 0;
  }
  if (action == ACT_SCORES) {
    unsigned lead = gs_leaders(&G);
    if (!G.np) { api->fmt(out, n, "no players yet"); return 0; }
    for (i = 0; i < G.np && o + 24 < n; i++)
      o += (size_t)api->fmt(out + o, n - o, "%s%s %d%s", i ? "\n" : "", G.name[i],
                            (int)gs_total(&G, i), (lead >> i & 1) ? " *" : "");
    return 0;
  }
  api->fmt(out, n, "gamenight has no command %d", action);
  return -1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Game Night",
  /* 16x16: a die beside a score card */
  { 0x00, 0x00, 0x7E, 0x00, 0x81, 0x00, 0xA5, 0x00,
    0x81, 0x7E, 0x99, 0x42, 0x81, 0x5A, 0xA5, 0x42,
    0x81, 0x5A, 0x7E, 0x42, 0x00, 0x5A, 0x00, 0x42,
    0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "1-4\tScore Dice Timer Buzzer; esc menu\n"
  "score\ttype a number, enter next; f first\n"
  "score\tg first here, l low wins, n new, d\n"
  "dice\tspc roll, ^v count, <> sides, e type\n"
  "dice\t1-9 hold a die; shake (ADV) rolls\n"
  "timer\tspc start, enter reset, <> ^v set\n"
  "buzzer\tspc buzz, c correct, s skip, t timer\n"
  "j\tthinking music (j again stops it)\n",
  ACTIONS,
  NACT,
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&S, 0, sizeof S);
  gd_init(&D, api->ticks_ms() * 2654435761u ^ api->epoch());
  S.sound = 1;
  S.t_preset = 60;
  S.renaming = -1;
  S.f_ui = S.f_uib = S.f_num = S.f_big = -1;
  load_all();
  S.t_left_ms = (int32_t)S.t_preset * 1000;
  S.t_last_sec = -1;
  if (!api->headless()) {
    S.f_ui = api->font_load("ui13");
    S.f_uib = api->font_load("ui13b");
    S.f_num = api->font_load("num30");
    S.f_big = api->font_load("clock56");
    au = api->audio ? api->audio() : 0;
    if (au) ensure_sounds();
  }
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.button = app_button;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  UI.pref_w = 240;
  UI.pref_h = 135;
  api->ui(&UI);
  return 0;
}

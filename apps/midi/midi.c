/* MIDI: a sequencer for a Grove-to-MIDI converter, its songs written by
 * Claude or by hand.
 *
 * A song is a text file in /songs (NAME.song), in the format apps/midi/midiseq.h
 * reads: notes at beats with lengths and velocities, chords, controller
 * changes and ramps, pitch bends, a tempo, a loop. `n` asks Claude for one
 * ("write the first section of Fur Elise") through the server
 * (apps/midi/server/__init__.py), which checks it before it comes back; `e` opens one in
 * Edit, and leaving Edit comes back to that song.
 *
 * Enter opens a song like a project: its piano roll, still, and every action
 * from there -- hear it, play it out, tempo, loop, ask Claude to change it,
 * undo, edit. Esc stops what plays, then goes back to the list.
 *
 * Playing is the kernel's (api->midi, API 38): the whole song goes over as
 * time-stamped messages and a task sends them on time, so a repaint here
 * cannot make a note late. While it plays, a piano roll: each channel its
 * own colour, a line where the song is.
 *
 * Space previews a song on the device's own speaker instead: every note,
 * up to eight at once, as soft triangle waves, rendered to
 * /cache/midi-preview.wav and played by the audio task. Rendered, not
 * played live, so the voices cost nothing while it sounds.
 *
 * `c` asks Claude to change the song on screen ("slower, and add the left
 * hand"); the old one is kept, and `u` puts it back.
 *
 * The converter listens on G1 or G2 depending on which it is; `g` swaps,
 * and the choice is kept in /config/midi.txt. `t` plays an arpeggio, to
 * find out which.
 *
 * @app folder Make
 * @app release extra
 */
/* Circuit (apps/midi/circuit.c) is this file built again with CIRCUIT defined:
 * a beat for a Novation Circuit's eight tracks, asked of Claude with the
 * instruments named (apps/midi/server/__init__.py, kit=circuit), its own folder and
 * colours, drums heard as drums, a lane per track, and a map from track to
 * the channel and note the Circuit listens on. Everything else is MIDI's. */
#ifdef CIRCUIT
#define MS_MAX_EV     1536          /* a beat is dense: a song in a big block, not the arena */
#define MS_MAX_NOTES  768
#else
#define MS_MAX_EV     768
#define MS_MAX_NOTES  384
#endif
#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/midi/midiseq.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/safefile.h"

static const CardApi *api;
static const CappMidi *mi;
static const CappAudio *au;

#ifdef CIRCUIT
#define PREVIEW     "/cache/circuit-preview.wav"
#define UNDO        "/cache/circuit-undo.song"
#define REOPEN      "/cache/circuit-open.txt"
#define DIR         "/circuit"
#define MAP_CONF    "/config/circuit.txt"
#define TEXT_MAX    16384
#define APP_NAME    "Circuit"
#define COMPOSE     "/midi/compose?kit=circuit"
#define EMPTY_LIST  "no beats: n asks Claude for one"
#else
#define PREVIEW     "/cache/midi-preview.wav"
#define UNDO        "/cache/midi-undo.song"
#define REOPEN      "/cache/midi-open.txt"   /* the song to open after Edit */
#define DIR         "/songs"
#define TEXT_MAX    8192
#define APP_NAME    "MIDI"
#define COMPOSE     "/midi/compose"
#define EMPTY_LIST  "no songs: n asks Claude for one"
#endif
#define RATE        16000
#define PV_BLOCK    256
#define FADE        48             /* samples of fade at a note's ends: no clicks */
#define VOICES      8

/* Each MIDI note's phase step a sample at 16 kHz, 2^32 a cycle: 440 Hz times
 * 2^((n-69)/12), worked out on the PC -- an app has no pow. */
static const uint32_t STEP[128] = {
  2194674u, 2325176u, 2463439u, 2609922u, 2765116u, 2929539u,
  3103738u, 3288296u, 3483828u, 3690988u, 3910465u, 4142993u,
  4389349u, 4650353u, 4926877u, 5219845u, 5530233u, 5859077u,
  6207476u, 6576592u, 6967657u, 7381975u, 7820930u, 8285987u,
  8778697u, 9300706u, 9853754u, 10439689u, 11060465u, 11718155u,
  12414953u, 13153184u, 13935313u, 14763950u, 15641860u, 16571974u,
  17557394u, 18601411u, 19707509u, 20879378u, 22120931u, 23436310u,
  24829905u, 26306368u, 27870626u, 29527900u, 31283720u, 33143947u,
  35114789u, 37202823u, 39415018u, 41758757u, 44241862u, 46872620u,
  49659811u, 52612737u, 55741253u, 59055800u, 62567441u, 66287895u,
  70229578u, 74405646u, 78830036u, 83517514u, 88483724u, 93745240u,
  99319622u, 105225474u, 111482506u, 118111601u, 125134882u, 132575789u,
  140459156u, 148811292u, 157660072u, 167035027u, 176967447u, 187490479u,
  198639243u, 210450947u, 222965012u, 236223201u, 250269764u, 265151578u,
  280918312u, 297622584u, 315320144u, 334070055u, 353934894u, 374980958u,
  397278486u, 420901894u, 445930023u, 472446403u, 500539528u, 530303157u,
  561836623u, 595245168u, 630640287u, 668140110u, 707869788u, 749961916u,
  794556973u, 841803789u, 891860047u, 944892805u, 1001079055u, 1060606313u,
  1123673247u, 1190490335u, 1261280574u, 1336280220u, 1415739577u, 1499923833u,
  1589113945u, 1683607578u, 1783720094u, 1889785610u, 2002158110u, 2121212627u,
  2247346494u, 2380980670u, 2522561148u, 2672560440u, 2831479154u, 2999847666u,
  3178227890u, 3367215155u,
};

#define CONF       "/config/midi.txt"       /* the pin: shared, it is one converter */
#define MAXS       32
#define NAMEL      40
#define TOP_H      14
#define ROW_H      12

#ifdef CIRCUIT
/* The Circuit's: a charcoal body, and each track the colour of its pads --
 * the drums warm, the synths purple and green, the MIDI tracks cool. */
#define CLR_BG     CAPP_RGB(24, 24, 26)
#define CLR_TEXT   CAPP_RGB(236, 236, 236)
#define CLR_DIM    CAPP_RGB(132, 132, 138)
#define CLR_SEL    CAPP_RGB(54, 54, 60)
#define CLR_ACC    CAPP_RGB(255, 120, 24)
#define CLR_BAD    CAPP_RGB(255, 80, 72)
#define CLR_ROLL   CAPP_RGB(32, 32, 35)
#define CLR_GRID   CAPP_RGB(46, 46, 50)
#define CLR_HEAD   CAPP_RGB(255, 255, 255)

static const uint16_t CHAN[8] = {
  CAPP_RGB(255, 96, 32),   CAPP_RGB(255, 168, 0),   CAPP_RGB(255, 228, 64),  CAPP_RGB(255, 64, 128),
  CAPP_RGB(168, 96, 255),  CAPP_RGB(64, 224, 96),   CAPP_RGB(0, 200, 255),   CAPP_RGB(72, 120, 255),
};
static const char *const LANE[8] = { "KCK", "SNR", "HAT", "FX", "KEY", "BAS", "MSC", "SMP" };
#define LABEL_W    22                       /* the lane names, left of the roll */
#else
#define CLR_BG     CAPP_RGB(15, 17, 23)
#define CLR_TEXT   CAPP_RGB(232, 236, 244)
#define CLR_DIM    CAPP_RGB(128, 136, 152)
#define CLR_SEL    CAPP_RGB(36, 44, 62)
#define CLR_ACC    CAPP_RGB(160, 140, 255)
#define CLR_BAD    CAPP_RGB(240, 110, 96)
#define CLR_ROLL   CAPP_RGB(22, 26, 36)
#define CLR_GRID   CAPP_RGB(34, 40, 54)
#define CLR_HEAD   CAPP_RGB(255, 255, 255)

static const uint16_t CHAN[8] = {
  CAPP_RGB(120, 180, 255), CAPP_RGB(255, 150, 110), CAPP_RGB(130, 220, 140), CAPP_RGB(250, 210, 90),
  CAPP_RGB(210, 140, 250), CAPP_RGB(100, 220, 220), CAPP_RGB(255, 120, 180), CAPP_RGB(200, 200, 200),
};
#define LABEL_W    0
#endif

enum { V_LIST, V_SONG, V_ASK };
enum { P_NONE, P_MIDI, P_SPEAKER };

static struct {
  char     file[MAXS][NAMEL];
  char     title[MAXS][NAMEL];
  int      n, sel, top;
  int      view;
  int      pin;                    /* 1 or 2 */
  int      loop_on;                /* -1 the song's own, 0 off, 1 on */
  int      tempo_add;
  int      ask_delete;
  char     status[48];
  int      bad;
  /* asking Claude */
  char     draft[120];
  char     job[16];
  uint32_t next_poll, asked_at;
  int      ask_back;               /* the view a prompt returns to */
  /* the song open */
  MsSong  *song;                   /* SG */
  uint32_t len_ms;
  int      lo, hi;                 /* pitch range on the roll */
  int      head_x;                 /* the playhead as drawn */
  int      playing;                /* P_NONE, P_MIDI or P_SPEAKER */
  char     change[NAMEL];          /* the song Claude is changing; "" for a new one */
  char     undo[NAMEL];            /* the song UNDO holds the last version of */
  CRect    c;
  int      have_c;
#ifdef CIRCUIT
  uint8_t  map_ch[8];              /* each track's channel, 1..16 */
  uint8_t  map_note[8];            /* every note of the track as this one; 0 as written */
  uint8_t  lane_lo[8], lane_hi[8]; /* each lane's pitch range */
#endif
  char     text[TEXT_MAX];
} M;

#define SG (*M.song)
#ifndef CIRCUIT
static MsSong s_song;
#endif

static void say(int bad, const char *s) { api->fmt(M.status, sizeof M.status, "%s", s); M.bad = bad; }
static void mark_all(void) { if (M.have_c) api->damage(M.c); }

static int ends_song(const char *s) {
  int n = (int)api->str_len(s);
  return n > 5 && s[n - 5] == '.' && s[n - 4] == 's' && s[n - 3] == 'o' && s[n - 2] == 'n' && s[n - 1] == 'g';
}

/* ---- files ----------------------------------------------------------------------- */

static int read_text(const char *path) {
  int fd = safe_open_read(api, path), n = 0, r;
  M.text[0] = 0;
  if (fd < 0) return -1;
  while (n < TEXT_MAX - 1 && (r = api->read(fd, M.text + n, (size_t)(TEXT_MAX - 1 - n))) > 0) n += r;
  api->close(fd);
  M.text[n] = 0;
  return n;
}

static void title_of(const char *path, char *out) {
  int fd = api->open(path, CAPP_O_READ), n, i = 0, k = 0;
  char buf[64];
  out[0] = 0;
  if (fd < 0) return;
  n = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  if (buf[0] != '#') return;
  i = 1;
  while (buf[i] == ' ') i++;
  while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && k < NAMEL - 1) out[k++] = buf[i++];
  out[k] = 0;
}

/* Listed straight into M.file and closed up in place -- a second copy of
 * the list to filter from was 1.3 KB of the data block (2026-10-09). */
static void rescan(void) {
  int n = api->list(DIR, &M.file[0][0], MAXS, NAMEL), i;
  M.n = 0;
  if (n < 0) { api->mkdir(DIR); n = 0; }
  for (i = 0; i < n && M.n < MAXS; i++) {
    char path[64];
    if (!ends_song(M.file[i])) continue;
    if (M.n != i) api->mem_cpy(M.file[M.n], M.file[i], NAMEL);
    api->fmt(path, sizeof path, DIR "/%s", M.file[M.n]);
    title_of(path, M.title[M.n]);
    if (!M.title[M.n][0]) api->fmt(M.title[M.n], NAMEL, "%s", M.file[M.n]);
    M.n++;
  }
  if (M.sel >= M.n) M.sel = M.n ? M.n - 1 : 0;
}

static void pin_load(void) {
  char b[4];
  int fd = api->open(CONF, CAPP_O_READ), n;
  M.pin = 2;
  if (fd < 0) return;
  n = api->read(fd, b, sizeof b);
  api->close(fd);
  if (n > 0 && b[0] == '1') M.pin = 1;
}

static void pin_save(void) {
  SafeFile f;
  if (safe_begin(&f, api, CONF) != 0) return;
  safe_write(&f, M.pin == 1 ? "1\n" : "2\n", 2);
  safe_commit(&f);
}

#ifdef CIRCUIT
/* ---- the track map ------------------------------------------------------------------- */

/* /config/circuit.txt: a line a track, "TRACK CHANNEL NOTE", NOTE 0 meaning
 * as written. Songs are always written to channels 1..8 in the order the
 * prompt names them; this is where they go on the owner's Circuit. */
static const char MAP_DEFAULT[] =
  "# Circuit: the MIDI channel each track plays on, and a note to play\n"
  "# every note of it as (0: as the song wrote it). For a Circuit Tracks\n"
  "# on its factory channels, the drums are all channel 10:\n"
  "#   1 10 60 / 2 10 62 / 3 10 64 / 4 10 65, and 5 1 0 / 6 2 0 / 7 3 0 / 8 4 0\n"
  "# track channel note\n"
  "1 1 0   kick\n2 2 0   snare\n3 3 0   hi-hat\n4 4 0   crash, special\n"
  "5 5 0   keys\n6 6 0   bass\n7 7 0   misc\n8 8 0   samples\n";

static int map_num(const char **p) {
  int v = -1;
  while (**p == ' ' || **p == '\t') (*p)++;
  if (**p < '0' || **p > '9') return -1;
  v = 0;
  while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
  return v;
}

static void map_parse(const char *t) {
  int i;
  for (i = 0; i < 8; i++) { M.map_ch[i] = (uint8_t)(i + 1); M.map_note[i] = 0; }
  while (*t) {
    const char *p = t;
    int tr = map_num(&p), ch = map_num(&p), note = map_num(&p);
    if (tr >= 1 && tr <= 8 && ch >= 1 && ch <= 16) {
      M.map_ch[tr - 1] = (uint8_t)ch;
      M.map_note[tr - 1] = (uint8_t)(note > 0 && note < 128 ? note : 0);
    }
    while (*t && *t != '\n') t++;
    if (*t) t++;
  }
}

static void map_load(void) {
  SafeFile f;
  if (read_text(MAP_CONF) > 0) { map_parse(M.text); return; }
  map_parse(MAP_DEFAULT);
  if (safe_begin(&f, api, MAP_CONF) != 0) return;           /* there to edit */
  safe_write(&f, MAP_DEFAULT, sizeof MAP_DEFAULT - 1);
  safe_commit(&f);
}

/* A channel message on track 1..8 goes to that track's channel, and a
 * note on a track with a note set becomes that note. */
static void map_event(uint8_t *b) {
  int st = b[0] & 0xF0, tr = b[0] & 0x0F;
  if (st < 0x80 || st > 0xE0 || tr > 7) return;
  b[0] = (uint8_t)(st | (M.map_ch[tr] - 1));
  if ((st == 0x80 || st == 0x90) && M.map_note[tr]) b[1] = M.map_note[tr];
}
#endif

/* ---- playing ----------------------------------------------------------------------- */

/* The song's events as the kernel takes them, written over the parsed ones:
 * a CappMidiEvent is smaller than an MsEvent, so event i is written to bytes
 * the parse has already been read past. Saves a second array of them. */
static int to_kernel(void) {
  CappMidiEvent *out = (CappMidiEvent *)(void *)SG.ev;
  int i;
  for (i = 0; i < SG.nev; i++) {
    uint32_t at = ms_ms(&SG, SG.ev[i].tick);
    uint8_t n = SG.ev[i].n, b0 = SG.ev[i].b[0], b1 = SG.ev[i].b[1], b2 = SG.ev[i].b[2];
    out[i].at_ms = at; out[i].n = n; out[i].b[0] = b0; out[i].b[1] = b1; out[i].b[2] = b2;
#ifdef CIRCUIT
    map_event(out[i].b);
#endif
  }
  return SG.nev;
}

static int looping(void) { return M.loop_on < 0 ? SG.loop : M.loop_on; }

/* What the song view says when nothing is playing. */
static void idle_status(void) {
  api->fmt(M.status, sizeof M.status, "%d bpm%s  G%d", SG.tempo, looping() ? "  loop" : "", M.pin);
  M.bad = 0;
}

/* The selected song read and parsed afresh, at the chosen tempo, with its
 * pitch range and length for the roll. Every play starts here, because
 * to_kernel() writes over the parsed events. */
static int load_song(void) {
  char path[64];
  int i;
  if (!M.n) return -1;
  if (!M.song) { say(1, "no room for a song: close something"); return -1; }
  api->fmt(path, sizeof path, DIR "/%s", M.file[M.sel]);
  if (read_text(path) < 0) { say(1, "cannot read it"); return -1; }
  if (ms_parse(&SG, M.text) != 0) {
    api->fmt(M.status, sizeof M.status, "line %d: %s", SG.err_line, SG.err);
    M.bad = 1;
    SG.nnotes = 0;
    return -1;
  }
  SG.tempo += M.tempo_add;
  if (SG.tempo < 20) SG.tempo = 20;
  if (looping() && !SG.loop_ticks) SG.loop_ticks = (SG.end_ticks + 4 * MS_PPQ - 1) / (4 * MS_PPQ) * (4 * MS_PPQ);
  M.len_ms = ms_ms(&SG, looping() ? SG.loop_ticks : SG.end_ticks);
  M.lo = 127; M.hi = 0;
  for (i = 0; i < SG.nnotes; i++) {
    if (SG.note[i].pitch < M.lo) M.lo = SG.note[i].pitch;
    if (SG.note[i].pitch > M.hi) M.hi = SG.note[i].pitch;
  }
  if (M.lo > M.hi) { M.lo = 48; M.hi = 72; }
#ifdef CIRCUIT
  for (i = 0; i < 8; i++) { M.lane_lo[i] = 127; M.lane_hi[i] = 0; }
  for (i = 0; i < SG.nnotes; i++) {
    int l = SG.note[i].ch & 7;
    if (SG.note[i].pitch < M.lane_lo[l]) M.lane_lo[l] = SG.note[i].pitch;
    if (SG.note[i].pitch > M.lane_hi[l]) M.lane_hi[l] = SG.note[i].pitch;
  }
#endif
  M.head_x = -1;
  return 0;
}

static void stop(void) {
  if (M.playing == P_SPEAKER) { if (au) au->stop(); }
  else if (M.playing == P_MIDI) { if (mi) mi->stop(); }
  M.playing = P_NONE;
  M.head_x = -1;
  if (M.view == V_SONG) idle_status();
}

/* The selected song, opened: the roll, still, and every action from there. */
static void open_sel(void) {
  if (!M.n) return;
  if (!M.song) { say(1, "no room for a song: close something"); return; }
  M.tempo_add = 0;
  M.loop_on = -1;
  M.view = V_SONG;
  if (load_song() == 0) idle_status();
}

static void play_midi(void) {
  int n, rc;
  if (!mi) return;
  stop();
  if (load_song() != 0) return;
  if (mi->open(M.pin == 1 ? 1 : 2) != 0) { say(1, "the Grove port would not open"); return; }
  n = to_kernel();
  rc = mi->play((const CappMidiEvent *)(void *)SG.ev, n, looping() ? M.len_ms : 0);
  if (rc != 0) { say(1, rc == -2 ? "not enough memory to play it" : "MIDI is not open"); return; }
  M.playing = P_MIDI;
  api->fmt(M.status, sizeof M.status, "to MIDI  %d bpm%s  G%d", SG.tempo, looping() ? "  loop" : "", M.pin);
  M.bad = 0;
}

/* ---- the preview -------------------------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* What is sounding at `tick`: up to VOICES pitches, the highest first, each
 * once (a unison across channels is one voice), with its velocity. */
static int sounding(uint32_t tick, int *pitch, int *vel) {
  int i, k, n = 0;
  for (i = 0; i < SG.nnotes; i++) {
    const MsNote *nt = &SG.note[i];
    if (tick < nt->start || tick >= nt->start + nt->len) continue;
#ifdef CIRCUIT
    if (nt->ch < 4) continue;                      /* a drum: drum_hits, not a tone */
#endif
    for (k = 0; k < n && pitch[k] != nt->pitch; k++) ;
    if (k < n) { if (nt->vel > vel[k]) vel[k] = nt->vel; continue; }
    if (n < VOICES) { pitch[n] = nt->pitch; vel[n] = nt->vel; n++; continue; }
    for (k = 0; k < n; k++)                      /* full: a higher note displaces the lowest */
      if (pitch[k] < nt->pitch) {
        int low = k, j;
        for (j = 0; j < n; j++) if (pitch[j] < pitch[low]) low = j;
        pitch[low] = nt->pitch; vel[low] = nt->vel;
        break;
      }
  }
  return n;
}

#ifdef CIRCUIT
/* The four drum tracks, heard as drums: a kick that falls in pitch, a snare
 * of noise over a tone, a hat of high noise, and a long wash of noise for
 * the crash track. Each is struck by a note starting and dies away. */
static struct { int32_t lvl, prev; uint32_t phase, step; } s_dr[4];
static uint32_t s_noise = 22222;

static int32_t noise(void) {
  s_noise = s_noise * 1664525u + 1013904223u;
  return (int32_t)(s_noise >> 16) - 32768;
}

static int32_t tri_of(uint32_t phase) {
  uint32_t ph = phase >> 16;
  return ph < 32768 ? (int32_t)ph * 2 - 32768 : (int32_t)(65535 - ph) * 2 - 32768;
}

#define KICK_LOW  13421773u                   /* 50 Hz at 16 kHz, 2^32 a cycle */
#define KICK_HIGH 42949673u                   /* 160 Hz */
#define SNARE_TONE 13421773u * 4              /* 200 Hz */

/* Strike the drums whose notes start in [t0, t1). */
static void drum_hits(uint32_t t0, uint32_t t1) {
  int i;
  for (i = 0; i < SG.nnotes; i++) {
    const MsNote *nt = &SG.note[i];
    if (nt->ch >= 4 || nt->start < t0 || nt->start >= t1) continue;
    s_dr[nt->ch].lvl = 4000 + nt->vel * 150;
    s_dr[nt->ch].phase = 0;
    if (nt->ch == 0) s_dr[0].step = KICK_HIGH;
  }
}

/* One sample of the four drums. */
static int32_t drum_sample(void) {
  int32_t sum = 0, n;
  if (s_dr[0].lvl > 0) {
    s_dr[0].phase += s_dr[0].step;
    s_dr[0].step -= (s_dr[0].step - KICK_LOW) >> 9;
    sum += tri_of(s_dr[0].phase) * s_dr[0].lvl / 32768;
    s_dr[0].lvl -= (s_dr[0].lvl >> 11) + 1;
  }
  if (s_dr[1].lvl > 0) {
    s_dr[1].phase += SNARE_TONE;
    n = noise() * 3 / 4 + tri_of(s_dr[1].phase) / 4;
    sum += n * s_dr[1].lvl / 32768;
    s_dr[1].lvl -= (s_dr[1].lvl >> 10) + 1;
  }
  if (s_dr[2].lvl > 0) {
    n = noise();
    sum += (n - s_dr[2].prev) / 3 * s_dr[2].lvl / 32768;
    s_dr[2].prev = n;
    s_dr[2].lvl -= (s_dr[2].lvl >> 8) + 1;
  }
  if (s_dr[3].lvl > 0) {
    sum += noise() / 2 * s_dr[3].lvl / 32768;
    s_dr[3].lvl -= (s_dr[3].lvl >> 13) + 1;
  }
  return sum;
}
#endif

/* The song as a WAV, every voice. A sample's tick is sample * tempo * 480 /
 * (60 * RATE) = sample * tempo / 2000; what sounds is looked up a block at a
 * time. Each voice keeps its phase while its note holds and fades in and
 * out over FADE samples, so nothing clicks; the sum is squeezed above 24000
 * rather than clipped, for the moments eight notes land at once. */
static int render_preview(void) {
  static int16_t block[PV_BLOCK];
  static struct { int pitch, level, target; uint32_t phase, step; } v[VOICES];
  uint8_t hdr[44];
  uint32_t samples, s, data;
  int fd, i;
  for (i = 0; i < VOICES; i++) { v[i].pitch = -1; v[i].level = v[i].target = 0; v[i].phase = 0; }
#ifdef CIRCUIT
  api->mem_set(s_dr, 0, sizeof s_dr);
#endif
  samples = SG.end_ticks / (uint32_t)SG.tempo * 2000 + (SG.end_ticks % (uint32_t)SG.tempo) * 2000 / (uint32_t)SG.tempo;
  samples += RATE / 4;                        /* a quarter second to ring out */
  if (samples > (uint32_t)RATE * 300) samples = (uint32_t)RATE * 300;
  data = samples * 2;
  fd = api->open(PREVIEW, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return -1;
  hdr[0] = 'R'; hdr[1] = 'I'; hdr[2] = 'F'; hdr[3] = 'F'; put32(hdr + 4, 36 + data);
  hdr[8] = 'W'; hdr[9] = 'A'; hdr[10] = 'V'; hdr[11] = 'E';
  hdr[12] = 'f'; hdr[13] = 'm'; hdr[14] = 't'; hdr[15] = ' '; put32(hdr + 16, 16);
  put16(hdr + 20, 1); put16(hdr + 22, 1); put32(hdr + 24, RATE); put32(hdr + 28, RATE * 2);
  put16(hdr + 32, 2); put16(hdr + 34, 16);
  hdr[36] = 'd'; hdr[37] = 'a'; hdr[38] = 't'; hdr[39] = 'a'; put32(hdr + 40, data);
  if (api->write(fd, hdr, 44) != 44) { api->close(fd); return -1; }
  for (s = 0; s < samples; s += PV_BLOCK) {
    uint32_t tick = s / 2000 * (uint32_t)SG.tempo + (s % 2000) * (uint32_t)SG.tempo / 2000;
    int pitch[VOICES], vel[VOICES], used[VOICES], np, k, n;
    np = sounding(tick, pitch, vel);
#ifdef CIRCUIT
    {
      uint32_t e = s + PV_BLOCK;
      drum_hits(tick, e / 2000 * (uint32_t)SG.tempo + (e % 2000) * (uint32_t)SG.tempo / 2000);
    }
#endif
    for (k = 0; k < np; k++) used[k] = 0;
    /* Voices already on a note that still sounds keep it; the rest fade. */
    for (i = 0; i < VOICES; i++) {
      v[i].target = 0;
      if (v[i].pitch < 0) continue;
      for (k = 0; k < np; k++)
        if (!used[k] && pitch[k] == v[i].pitch) { used[k] = 1; v[i].target = 2500 + vel[k] * 30; break; }
    }
    /* New notes take a voice that is free or has faded out. */
    for (k = 0; k < np; k++) {
      if (used[k]) continue;
      for (i = 0; i < VOICES; i++)
        if (v[i].pitch < 0 || (v[i].target == 0 && v[i].level == 0)) {
          v[i].pitch = pitch[k];
          v[i].step = STEP[pitch[k]];
          v[i].phase = 0;
          v[i].level = 0;
          v[i].target = 2500 + vel[k] * 30;
          break;
        }
    }
    n = samples - s < PV_BLOCK ? (int)(samples - s) : PV_BLOCK;
    for (k = 0; k < n; k++) {
      int32_t sum = 0;
      for (i = 0; i < VOICES; i++) {
        uint32_t ph;
        int32_t tri;
        if (v[i].pitch < 0) continue;
        if (v[i].level < v[i].target) {
          v[i].level += (v[i].target - v[i].level) / FADE + 1;
          if (v[i].level > v[i].target) v[i].level = v[i].target;
        } else if (v[i].level > v[i].target) {
          v[i].level -= (v[i].level - v[i].target) / FADE + 1;
          if (v[i].level < v[i].target) v[i].level = v[i].target;
        }
        if (!v[i].level && !v[i].target) { v[i].pitch = -1; continue; }
        v[i].phase += v[i].step;
        ph = v[i].phase >> 16;
        tri = ph < 32768 ? (int32_t)ph * 2 - 32768 : (int32_t)(65535 - ph) * 2 - 32768;
        sum += tri * v[i].level / 32768;
      }
#ifdef CIRCUIT
      sum += drum_sample();
#endif
      if (sum > 24000) sum = 24000 + (sum - 24000) / 4;
      else if (sum < -24000) sum = -24000 + (sum + 24000) / 4;
      if (sum > 32767) sum = 32767;
      if (sum < -32767) sum = -32767;
      block[k] = (int16_t)sum;
    }
    if (api->write(fd, block, (size_t)n * 2) != n * 2) { api->close(fd); return -1; }
  }
  api->close(fd);
  return 0;
}

static void play_speaker(void) {
  int i;
  if (!au) return;
  stop();
  if (load_song() != 0) return;
  if (!SG.nnotes) { say(1, "no notes to hear"); return; }
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  if (render_preview() != 0) { say(1, "cannot write the preview to the card"); return; }
  for (i = 0; i < 40 && au->state() != CAPP_AUDIO_IDLE; i++) {
    uint32_t t0 = api->ticks_ms() + 10;
    while ((int32_t)(api->ticks_ms() - t0) < 0) ;
  }
  if (au->play(PREVIEW) != 0) { say(1, "the speaker would not play it"); return; }
  M.len_ms = ms_ms(&SG, SG.end_ticks);   /* the preview does not loop */
  M.playing = P_SPEAKER;
  api->fmt(M.status, sizeof M.status, "speaker  %d bpm", SG.tempo);
  M.bad = 0;
}

/* + - and l while something plays: start it again as it was. */
static void replay(int what) {
  if (what == P_MIDI) play_midi();
  else if (what == P_SPEAKER) play_speaker();
  else if (load_song() == 0) idle_status();
}

/* A C-major arpeggio, now: to hear whether the converter is on this pin. */
static void test_tone(void) {
  static const int8_t NOTES[4] = { 60, 64, 67, 72 };
  CappMidiEvent ev[8];
  int i;
#ifdef CIRCUIT
  uint8_t ch = (uint8_t)(M.map_ch[4] - 1);         /* the keys track */
#else
  uint8_t ch = 0;
#endif
  if (!mi || mi->open(M.pin == 1 ? 1 : 2) != 0) { say(1, "the Grove port would not open"); return; }
  for (i = 0; i < 4; i++) {
    ev[2 * i].at_ms = (uint32_t)i * 250; ev[2 * i].n = 3;
    ev[2 * i].b[0] = (uint8_t)(0x90 | ch); ev[2 * i].b[1] = (uint8_t)NOTES[i]; ev[2 * i].b[2] = 100;
    ev[2 * i + 1].at_ms = (uint32_t)i * 250 + 230; ev[2 * i + 1].n = 3;
    ev[2 * i + 1].b[0] = (uint8_t)(0x80 | ch); ev[2 * i + 1].b[1] = (uint8_t)NOTES[i]; ev[2 * i + 1].b[2] = 0;
  }
  mi->play(ev, 8, 0);
  api->fmt(M.status, sizeof M.status, "a test arpeggio on G%d", M.pin);
  M.bad = 0;
}

/* ---- asking Claude ---------------------------------------------------------------------- */

static int save_as(const char *path, const char *text) {
  SafeFile f;
  if (safe_begin(&f, api, path) != 0) return -1;
  safe_write(&f, text, api->str_len(text));
  return safe_commit(&f);
}

static void ask_send(void) {
  char url[128], reply[24];
  const char *body = M.draft;
  int r, i;
  if (!M.draft[0]) { M.view = M.ask_back; return; }
  if (!api->net_ready() && api->net_connect(15000) != 0) { say(1, "offline: Claude needs the server"); M.view = M.ask_back; return; }
  /* A change: what to change, the server's mark, and the song as it is --
   * which is kept, so u can put it back. */
  if (M.change[0]) {
    static const char MARK[] = "\n---song---\n";
    char path[64];
    int pre = (int)api->str_len(M.draft) + (int)sizeof MARK - 1, fd, got = 0;
    api->fmt(path, sizeof path, DIR "/%s", M.change);
    if ((fd = safe_open_read(api, path)) < 0) { say(1, "cannot read it"); M.change[0] = 0; return; }
    while (pre + got < TEXT_MAX - 1 &&
           (r = api->read(fd, M.text + pre + got, (size_t)(TEXT_MAX - 1 - pre - got))) > 0) got += r;
    api->close(fd);
    M.text[pre + got] = 0;
    if (save_as(UNDO, M.text + pre) == 0) api->fmt(M.undo, sizeof M.undo, "%s", M.change);
    api->mem_cpy(M.text, M.draft, api->str_len(M.draft));
    api->mem_cpy(M.text + api->str_len(M.draft), MARK, sizeof MARK - 1);
    body = M.text;
  }
  api->fmt(url, sizeof url, "%s" COMPOSE, api->proxy());
  r = api->http("POST", url, body, "text/plain", "", reply, sizeof reply, 15000);
  if (r < 0 || reply[0] == 'e') { say(1, "the server did not take it"); M.view = M.ask_back; M.change[0] = 0; return; }
  for (i = 0; reply[i] && reply[i] != '\n' && i < (int)sizeof M.job - 1; i++) M.job[i] = reply[i];
  M.job[i] = 0;
  M.asked_at = api->ticks_ms();
  M.next_poll = M.asked_at + 3000;
  say(0, "Claude is writing it...");
}

/* A file name from the title: letters and digits, the rest as -. */
static void name_from(const char *title, char *out, int n) {
  int i = 0, k = 0, dash = 0;
  for (; title[i] && k < n - 7; i++) {
    char c = title[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[k++] = c; dash = 0; }
    else if (c >= 'A' && c <= 'Z') { out[k++] = (char)(c + 32); dash = 0; }
    else if (k && !dash) { out[k++] = '-'; dash = 1; }
  }
  while (k && out[k - 1] == '-') k--;
  if (!k) { out[0] = 's'; out[1] = 'o'; out[2] = 'n'; out[3] = 'g'; k = 4; }
  out[k] = 0;
}

static void ask_poll(void) {
  char url[128], name[NAMEL - 6], file[NAMEL], path[64];
  SafeFile f;
  int r, i;
  api->fmt(url, sizeof url, "%s/midi/compose?id=%s", api->proxy(), M.job);
  r = api->http("GET", url, 0, 0, "", M.text, TEXT_MAX, 15000);
  if (r < 0) { M.next_poll = api->ticks_ms() + 4000; return; }
  if (M.text[0] == 'p') {                       /* pending */
    M.next_poll = api->ticks_ms() + 3000;
    api->fmt(M.status, sizeof M.status, "Claude is writing it... %lus",
             (unsigned long)((api->ticks_ms() - M.asked_at) / 1000));
    if (M.have_c) api->damage(capp_rect(M.c.x, M.c.y, M.c.w, TOP_H));
    return;
  }
  M.job[0] = 0;
  if (!(M.text[0] == 'o' && M.text[1] == 'k' && M.text[2] == '\n')) {
    for (i = 0; M.text[i] && M.text[i] != '\n'; i++) ;
    M.text[i] = 0;
    say(1, M.text[0] == 'e' ? M.text + 6 : "Claude did not write one");
    mark_all();
    return;
  }
  /* "ok\n" and a change: over the song it changed. */
  if (M.change[0]) {
    api->fmt(path, sizeof path, DIR "/%s", M.change);
    M.change[0] = 0;
    if (save_as(path, M.text + 3) != 0) { say(1, "cannot write to the card"); return; }
    rescan();
    if (M.view != V_LIST && M.playing == P_NONE) load_song();
    say(0, "changed: space hears it, u undoes");
    mark_all();
    return;
  }
  /* "ok\n" and a new song: its title names the file. */
  {
    char title[NAMEL];
    const char *song = M.text + 3;
    i = 0;
    title[0] = 0;
    if (song[0] == '#') {
      int k = 0;
      i = 1;
      while (song[i] == ' ') i++;
      while (song[i] && song[i] != '\n' && k < NAMEL - 1) title[k++] = song[i++];
      title[k] = 0;
    }
    name_from(title[0] ? title : M.draft, name, sizeof name);
    api->fmt(file, sizeof file, "%s.song", name);
    api->fmt(path, sizeof path, DIR "/%s", file);
    api->mkdir(DIR);
    if (safe_begin(&f, api, path) != 0) { say(1, "cannot write to the card"); return; }
    safe_write(&f, song, api->str_len(song));
    if (safe_commit(&f) != 0) { say(1, "cannot write to the card"); return; }
    rescan();
    for (i = 0; i < M.n; i++) if (!(M.file[i][0] - file[0]) && api->str_len(M.file[i]) == api->str_len(file)) {
      int k;
      for (k = 0; file[k] && file[k] == M.file[i][k]; k++) ;
      if (!file[k]) M.sel = i;
    }
    /* The new song opens, whatever was on screen: it is what was asked for. */
    stop();
    if (M.view == V_ASK) { M.draft[0] = 0; M.change[0] = 0; }
    open_sel();
    say(0, "written: space hears it");
  }
  mark_all();
}

static void delete_sel(void) {
  char path[64];
  if (!M.n) return;
  api->fmt(path, sizeof path, DIR "/%s", M.file[M.sel]);
  api->remove(path);
  rescan();
  say(0, "deleted");
}

/* ---- the screen ----------------------------------------------------------------------------- */

static int in_song(void) { return M.view == V_SONG || (M.view == V_ASK && M.ask_back == V_SONG); }

static void paint_top(void) {
  CRect r = capp_rect(M.c.x, M.c.y, M.c.w, TOP_H);
  int w = (int)api->str_len(M.status) * 6, room, tw;
  char title[NAMEL];
  /* The song's title, cut to leave a space before the status. */
  room = (r.w - 24 - w) / 6;
  if (room < 0) room = 0;
  if (room > NAMEL - 1) room = NAMEL - 1;
  api->fmt(title, sizeof title, "%s", in_song() && M.n ? M.title[M.sel] : APP_NAME);
  title[room] = 0;
  tw = (int)api->str_len(title) * 6;
  api->text((int16_t)(r.x + 6), (int16_t)(r.y + 3), title, CLR_ACC, CLR_BG);
  api->fill(capp_rect(r.x, r.y, r.w, 3), CLR_BG);
  api->fill(capp_rect(r.x, r.y + 11, r.w, 3), CLR_BG);
  api->fill(capp_rect(r.x, r.y + 3, 6, 8), CLR_BG);
  api->fill(capp_rect(r.x + 6 + tw, r.y + 3, r.w - 12 - w - tw, 8), CLR_BG);
  api->text((int16_t)(r.x + r.w - 6 - w), (int16_t)(r.y + 3), M.status, M.bad ? CLR_BAD : CLR_DIM, CLR_BG);
  api->fill(capp_rect(r.x + r.w - 6, r.y + 3, 6, 8), CLR_BG);
}

static void paint_list(void) {
  int rows = (M.c.h - TOP_H - FOOT_H) / ROW_H, i, y = M.c.y + TOP_H;
  if (M.sel < M.top) M.top = M.sel;
  if (M.sel >= M.top + rows) M.top = M.sel - rows + 1;
  for (i = M.top; i < M.n && i - M.top < rows; i++, y += ROW_H) {
    char line[48];
    uint16_t bg = i == M.sel ? CLR_SEL : CLR_BG;
    api->fmt(line, sizeof line, " %-37.37s", M.title[i]);
    api->text((int16_t)M.c.x, (int16_t)(y + 2), line, CLR_TEXT, bg);
    api->fill(capp_rect(M.c.x, y, M.c.w, 2), bg);
    api->fill(capp_rect(M.c.x, y + 10, M.c.w, ROW_H - 10), bg);
    api->fill(capp_rect(M.c.x + 38 * 6, y + 2, M.c.w - 38 * 6, 8), bg);
  }
  if (!M.n) {
    api->text((int16_t)(M.c.x + 8), (int16_t)(y + 4), EMPTY_LIST, CLR_DIM, CLR_BG);
    api->fill(capp_rect(M.c.x, y, M.c.w, 4), CLR_BG);
    y += 12;
  }
  api->fill(capp_rect(M.c.x, y, M.c.w, M.c.y + M.c.h - FOOT_H - y), CLR_BG);
}

static CRect roll_rect(void) {
  return capp_rect(M.c.x + 4 + LABEL_W, M.c.y + TOP_H + 2, M.c.w - 8 - LABEL_W, M.c.h - TOP_H - FOOT_H - 4);
}

static int roll_x(uint32_t ms) {
  CRect r = roll_rect();
  return M.len_ms ? r.x + (int)(ms / 4 * (uint32_t)r.w / (M.len_ms / 4 + 1)) : r.x;
}

/* The roll within the clip: background, notes that reach it, the line. */
static void paint_roll(void) {
  CRect r = roll_rect(), a = api->paint_area();
  int span = M.hi - M.lo + 1, rowh = r.h / (span ? span : 1), i;
  if (rowh < 1) rowh = 1;
  if (rowh > 6) rowh = 6;
  api->fill(r, CLR_ROLL);
#ifdef CIRCUIT
  /* A lane a track: a drum hit fills its lane's height, a melodic note sits
   * at its pitch within its own track's range. */
  {
    int lh = r.h / 8;
    for (i = 1; i < 8; i++) api->fill(capp_rect(r.x, r.y + i * lh, r.w, 1), CLR_GRID);
    for (i = 0; i < SG.nnotes; i++) {
      const MsNote *nt = &SG.note[i];
      int l = nt->ch & 7, top = r.y + l * lh + 1, h = lh - 2, y = top;
      int x0 = roll_x(ms_ms(&SG, nt->start)), x1 = roll_x(ms_ms(&SG, nt->start + nt->len));
      if (x1 < a.x || x0 > a.x + a.w) continue;
      if (l >= 4) {
        int sp = M.lane_hi[l] - M.lane_lo[l] + 1, rh = h / (sp > 0 ? sp : 1);
        if (rh < 1) rh = 1;
        if (rh > 3) rh = 3;
        y = top + h - rh - (nt->pitch - M.lane_lo[l]) * (h - rh) / (sp > 1 ? sp - 1 : 1);
        h = rh;
      } else if (x1 - x0 > 3) x1 = x0 + 3;          /* a hit is a tick, whatever its length */
      if (x1 <= x0) x1 = x0 + 1;
      api->fill(capp_rect(x0, y, x1 - x0 - (x1 - x0 > 2), h), CHAN[l]);
    }
    if (M.head_x >= r.x) api->fill(capp_rect(M.head_x, r.y, 1, r.h), CLR_HEAD);
    return;
  }
#endif
  for (i = 0; i < SG.nnotes; i++) {
    const MsNote *nt = &SG.note[i];
    int x0 = roll_x(ms_ms(&SG, nt->start)), x1 = roll_x(ms_ms(&SG, nt->start + nt->len));
    int y = r.y + r.h - (nt->pitch - M.lo + 1) * r.h / (span ? span : 1);
    if (x1 < a.x || x0 > a.x + a.w) continue;
    if (x1 <= x0) x1 = x0 + 1;
    api->fill(capp_rect(x0, y, x1 - x0 - (x1 - x0 > 2), rowh), CHAN[nt->ch & 7]);
  }
  if (M.head_x >= r.x) api->fill(capp_rect(M.head_x, r.y, 1, r.h), CLR_HEAD);
}

static void app_paint(void *st, CRect c) {
  CRect a = api->paint_area();
  (void)st;
  M.c = c;
  M.have_c = 1;
  /* The playhead moving: its two columns and nothing else. */
  if (M.view == V_SONG && a.w <= 4 && a.y >= roll_rect().y) { paint_roll(); return; }
  paint_top();
  if (in_song()) {
    api->fill(capp_rect(c.x, c.y + TOP_H, c.w, 2), CLR_BG);
    api->fill(capp_rect(c.x, c.y + TOP_H + 2, 4 + LABEL_W, roll_rect().h), CLR_BG);
#ifdef CIRCUIT
    {
      CRect r = roll_rect();
      int lh = r.h / 8, l;
      for (l = 0; l < 8; l++)
        api->text((int16_t)(c.x + 3), (int16_t)(r.y + l * lh + (lh - 8) / 2), LANE[l], CHAN[l], CLR_BG);
    }
#endif
    api->fill(capp_rect(c.x + c.w - 4, c.y + TOP_H + 2, 4, roll_rect().h), CLR_BG);
    api->fill(capp_rect(c.x, c.y + c.h - FOOT_H - 2, c.w, 2), CLR_BG);
    paint_roll();
  } else paint_list();
  if (M.view == V_ASK) {
    char line[48];
    int n = (int)api->str_len(M.draft);
    api->fmt(line, sizeof line, "%s %s_%-38s", M.change[0] ? "change:" : "write:",
             n > 29 ? M.draft + n - 29 : M.draft, "");
    line[38] = 0;
    footer_paint(api, c, line);
  } else if (M.ask_delete) {
    char line[48];
    api->fmt(line, sizeof line, "delete %.22s? y yes  n no", M.title[M.sel]);
    footer_paint(api, c, line);
  } else if (M.view == V_LIST) footer_paint(api, c, "enter open  space hear  n new");
  else if (M.playing == P_SPEAKER) footer_paint(api, c, "space stop  + - tempo");
  else if (M.playing == P_MIDI) footer_paint(api, c, "enter stop  + - tempo  l loop");
  else footer_paint(api, c, "space hear  enter midi  c ask  e edit");
}

/* ---- keys and time ------------------------------------------------------------------------- */

static int key_ask(uint8_t k) {
  int n = (int)api->str_len(M.draft);
  if (k == CAPP_KEY_ESC) { M.view = M.ask_back; M.change[0] = 0; return 1; }
  if (k == CAPP_KEY_ENTER) { M.view = M.ask_back; ask_send(); return 1; }
  if (k == CAPP_KEY_BACK) { if (n) M.draft[n - 1] = 0; return 1; }
  if (k >= 32 && k < 127 && n < (int)sizeof M.draft - 1) { M.draft[n] = (char)k; M.draft[n + 1] = 0; }
  return 1;
}

static void ask(int change) {
  if (M.job[0]) { say(0, "Claude is still writing the last one"); return; }
  M.draft[0] = 0;
  M.change[0] = 0;
  if (change) api->fmt(M.change, sizeof M.change, "%s", M.file[M.sel]);
  M.ask_back = M.view;
  M.view = V_ASK;
}

/* Edit, and back to this song after: the launcher reopens this app fresh,
 * so the song to open is left on the card. */
static void edit_sel(void) {
  char path[64];
  if (!M.n) return;
  stop();
  save_as(REOPEN, M.file[M.sel]);
  api->fmt(path, sizeof path, DIR "/%s", M.file[M.sel]);
  api->run("edit", path);
}

#ifndef CIRCUIT
/* The song to the Claude app, to talk about and change; back here after. */
static void talk_sel(void) {
  char args[80];
  if (!M.n) return;
  stop();
  save_as(REOPEN, M.file[M.sel]);
  api->fmt(args, sizeof args, "-k song " DIR "/%s", M.file[M.sel]);
  if (api->run("claude", args) != 0) say(1, "no Claude app");
}
#endif

static void reopen(void) {
  int i;
  if (read_text(REOPEN) <= 0) return;
  api->remove(REOPEN);
  for (i = 0; i < M.n; i++) {
    int k;
    for (k = 0; M.text[k] && M.text[k] == M.file[i][k]; k++) ;
    if (!M.file[i][k] && (!M.text[k] || M.text[k] == '\n')) { M.sel = i; open_sel(); return; }
  }
}

/* Keys that mean the same in the list and in a song. */
static int key_common(uint8_t k) {
  switch (k) {
  case 'n': case 'N':  ask(0); break;
  case 'd': case 'D': case 0x7F: if (M.n) M.ask_delete = 1; break;
  case 'g': case 'G':  M.pin = M.pin == 1 ? 2 : 1; pin_save(); if (mi) mi->close();
                       api->fmt(M.status, sizeof M.status, "MIDI out on G%d", M.pin); M.bad = 0; break;
  case 't': case 'T':  test_tone(); break;
  default: return 0;
  }
  return 1;
}

/* u after Claude changed a song: the version before, back. */
static void undo(void) {
  char path[64];
  if (!M.undo[0] || read_text(UNDO) <= 0) { say(0, "nothing to undo"); return; }
  api->fmt(path, sizeof path, DIR "/%s", M.undo);
  stop();
  if (save_as(path, M.text) != 0) { say(1, "cannot write to the card"); return; }
  api->remove(UNDO);
  M.undo[0] = 0;
  rescan();
  if (load_song() == 0) say(0, "put back as it was");
}

static int key_song(uint8_t k) {
  switch (k) {
  case ' ':            if (M.playing == P_SPEAKER) stop(); else play_speaker(); break;
  case CAPP_KEY_ENTER: if (M.playing == P_MIDI) stop(); else play_midi(); break;
  case CAPP_KEY_ESC:   if (M.playing) stop(); else { M.view = V_LIST; say(0, ""); } break;
  case '+': case '=':  M.tempo_add += 4; replay(M.playing); break;
  case '-': case '_':  M.tempo_add -= 4; replay(M.playing); break;
  case 'l': case 'L':  M.loop_on = looping() ? 0 : 1; replay(M.playing); break;
#ifdef CIRCUIT
  case 'c': case 'C':  stop(); ask(1); break;   /* the composer knows the tracks; the talk would not */
#else
  case 'c': case 'C':  talk_sel(); break;
#endif
  case 'e': case 'E':  edit_sel(); break;
  case 'r': case 'R':  stop(); if (load_song() == 0) idle_status(); break;
  case 'u': case 'U':  undo(); break;
  default:             return key_common(k);
  }
  return 1;
}

static int key_list(uint8_t k) {
  switch (k) {
  case CAPP_KEY_UP:    if (M.sel > 0) M.sel--; break;
  case CAPP_KEY_DOWN:  if (M.sel < M.n - 1) M.sel++; break;
  case CAPP_KEY_ENTER: open_sel(); break;
  case ' ':            open_sel(); if (M.view == V_SONG && !M.bad) play_speaker(); break;
  case 'e': case 'E':  edit_sel(); break;
  case 'r': case 'R':  rescan(); say(0, ""); break;
  default:             return key_common(k);
  }
  return 1;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (M.view == V_ASK) { key_ask(k); mark_all(); return 1; }
  if (M.ask_delete) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_WAIT) return 1;
    M.ask_delete = 0;
    if (a == CONFIRM_YES) { stop(); delete_sel(); M.view = V_LIST; }
    mark_all();
    return 1;
  }
  if (!(M.view == V_SONG ? key_song(k) : key_list(k))) return 0;
  mark_all();
  return 1;
}

static int app_wants_text(void *st) { (void)st; return M.view == V_ASK; }

static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (M.job[0] && (int32_t)(now - M.next_poll) >= 0) { ask_poll(); return 1; }
  if (M.playing && (M.playing == P_SPEAKER ? au != 0 : mi != 0)) {
    int x;
    int on = M.playing == P_SPEAKER ? au->state() == CAPP_AUDIO_PLAYING || au->paused() : mi->playing();
    uint32_t pos = M.playing == P_SPEAKER ? au->pos_ms() : mi->pos_ms();
    if (!on) {
      M.playing = P_NONE;
      M.head_x = -1;
      if (M.view == V_SONG) idle_status();
      mark_all();
      return 1;
    }
    x = roll_x(pos % (M.len_ms ? M.len_ms : 1));
    if (x != M.head_x && M.have_c && M.view == V_SONG) {
      CRect r = roll_rect();
      if (M.head_x >= r.x) api->damage(capp_rect(M.head_x, r.y, 1, r.h));
      M.head_x = x;
      api->damage(capp_rect(x, r.y, 1, r.h));
      return 1;
    }
  }
  return 0;
}

#ifndef CIRCUIT
const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "MIDI",
  /* 16x16: piano keys. */
  { 0x00, 0x00, 0xFF, 0xFF, 0x92, 0x49, 0x92, 0x49,
    0x92, 0x49, 0x92, 0x49, 0x92, 0x49, 0xB6, 0xDB,
    0xB6, 0xDB, 0xB6, 0xDB, 0x92, 0x49, 0x92, 0x49,
    0x92, 0x49, 0x92, 0x49, 0xFF, 0xFF, 0x00, 0x00 },
  "In the list\nup/down\tchoose a song\nenter\topen it\nspace\topen it and hear it\n"
  "n\task Claude to write one\nr\tread the folder again\n"
  "\nIn a song\nspace\thear it on the speaker, up to eight notes at once; again to stop\n"
  "enter\tplay it to MIDI; again to stop\n+ -\ttempo\nl\tloop on and off\n"
  "c\ttalk to Claude about it; ctrl-s there saves a change\n"
  "u\tput back the song before Claude last changed it\n"
  "e\tedit it in Edit, and come back to it\nr\tread it again\nesc\tstop, then back to the list\n"
  "\nAnywhere\nd, del\tdelete (asks)\ng\tMIDI out on G1 or G2 (whichever your converter uses)\n"
  "t\ta test arpeggio\n\nsongs are text in /songs; see apps/midi/midiseq.h.\n",
};
#endif

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  mi = api->midi();
  au = api->audio();
#ifdef CIRCUIT
  /* The arena is too small for a beat; without the block the list still
   * works and a song says why it will not open. */
  M.song = (MsSong *)api->big_alloc(sizeof *M.song);
  map_load();
#else
  M.song = &s_song;
#endif
  M.loop_on = -1;
  pin_load();
  api->mkdir(DIR);
  rescan();
  reopen();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}

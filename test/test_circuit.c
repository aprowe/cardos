/* Circuit: the MIDI app built for a Novation Circuit. What it adds is tested
 * here -- the track map, the drums in the preview, the lanes, the composer
 * asked with the kit; the song format is test_midiseq.c's, the server's
 * prompt server/tests/test_midi.py's. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info circuit_capp_info
#define capp_main circuit_capp_main
#include "apps/midi/circuit.c"
#undef capp_info
#undef capp_main

static CardApi CF;

static CappMidiEvent s_sent[64];
static int s_nsent;
static int c_open(int pin) { (void)pin; return 0; }
static void c_void(void) {}
static int c_send(const uint8_t *b, int n) { (void)b; return n; }
static int c_play(const CappMidiEvent *e, int n, uint32_t l) {
  (void)l;
  s_nsent = n < 64 ? n : 64;
  memcpy(s_sent, e, (size_t)s_nsent * sizeof *e);
  return 0;
}
static int c_zero(void) { return 0; }
static uint32_t c_u32(void) { return 0; }
static const CappMidi CMIDI = { c_open, c_void, c_send, c_play, c_void, c_zero, c_u32 };
static const CappMidi *c_midi(void) { return &CMIDI; }

static const char BEAT[] =
  "# Test beat\n"
  "tempo 120\n"
  "ch 1\nn 0 C4 0.25 120\n"
  "ch 3\nn 0.5 C4 0.1 80\n"
  "ch 5\nn 0 C4,E4,G4 1 90\n"
  "ch 6\nn 0 C2 0.5\nn 1 G2 0.5\n";

static void copen(void) {
  fakeapi_init(&CF);
  fakefs_mem(&CF);
  CF.midi = c_midi;
  api = &CF;
  memset(&M, 0, sizeof M);
  s_nsent = 0;
}

static void start_with(const char *map) {
  copen();
  if (map) fakefs_put(MAP_CONF, map);
  fakefs_put(DIR "/test-beat.song", BEAT);
  circuit_capp_main(&CF, 0, 0);
}

void test_circuit_writes_a_track_map_to_edit(void) {
  int i;
  start_with(0);
  CHECK(fakefs_get(MAP_CONF) != NULL);
  CHECK(strstr(fakefs_get(MAP_CONF), "1 1 0   kick") != NULL);
  for (i = 0; i < 8; i++) CHECK(M.map_ch[i] == i + 1 && M.map_note[i] == 0);
}

void test_circuit_sends_each_track_where_the_map_says(void) {
  /* A Circuit Tracks on its factory channels: drums on 10, a note each. */
  start_with("# mine\n1 10 60\n2 10 62\n3 10 64 hat\n4 10 65\n5 1 0\n6 2\n");
  CHECK(M.map_ch[0] == 10 && M.map_note[2] == 64);
  CHECK(M.map_ch[5] == 2 && M.map_note[5] == 0);       /* a missing note is as written */
  CHECK(M.map_ch[6] == 7);                              /* a missing line keeps its own */
  M.sel = 0;
  play_midi();
  CHECK_EQ(M.playing, P_MIDI);
  {
    int i, kick = 0, hat = 0, keys = 0, bass = 0;
    for (i = 0; i < s_nsent; i++) {
      const uint8_t *b = s_sent[i].b;
      if ((b[0] & 0xF0) != 0x90) continue;
      if (b[0] == 0x99 && b[1] == 60) kick++;
      if (b[0] == 0x99 && b[1] == 64) hat++;
      if (b[0] == 0x90) keys++;                         /* track 5 to channel 1 */
      if (b[0] == 0x91 && (b[1] == 36 || b[1] == 43)) bass++;
    }
    CHECK_EQ(kick, 1);
    CHECK_EQ(hat, 1);
    CHECK_EQ(keys, 3);
    CHECK_EQ(bass, 2);
  }
}

void test_circuit_lanes_have_their_own_ranges(void) {
  start_with(0);
  M.sel = 0;
  CHECK_EQ(load_song(), 0);
  CHECK(M.lane_lo[4] == 60 && M.lane_hi[4] == 67);     /* keys: C4..G4 */
  CHECK(M.lane_lo[5] == 36 && M.lane_hi[5] == 43);     /* bass: C2..G2 */
  CHECK(M.lane_lo[1] == 127);                           /* no snare */
}

/* The preview: drums are hits, not tones -- at beat 0 only the keys sound
 * as voices, and the kick is struck and dies away. */
void test_circuit_hears_drums_as_drums(void) {
  int pitch[VOICES], vel[VOICES], n, i;
  int32_t peak = 0, late = 0;
  start_with(0);
  M.sel = 0;
  CHECK_EQ(load_song(), 0);
  n = sounding(0, pitch, vel);
  CHECK_EQ(n, 4);                                       /* C4 E4 G4 and the bass, no kick */
  for (i = 0; i < n; i++) CHECK(pitch[i] != 60 || vel[i] == 90);
  memset(s_dr, 0, sizeof s_dr);
  drum_hits(0, 10);
  CHECK(s_dr[0].lvl > 0 && s_dr[2].lvl == 0);           /* the kick, not the hat at 0.5 */
  for (i = 0; i < 1600; i++) { int32_t v = drum_sample(); if (v < 0) v = -v; if (v > peak) peak = v; }
  for (i = 0; i < 16000; i++) { int32_t v = drum_sample(); if (v < 0) v = -v; late = v; }
  CHECK(peak > 5000);
  CHECK_EQ(late, 0);
  CHECK_EQ(render_preview(), 0);
  CHECK_EQ(fakefs_size(PREVIEW), 44 + 2 * (12000 + 4000));   /* 1.5 beats at 120, and the ring */
}

static char s_url[160], s_body[200];
static int c_http(const char *m, const char *url, const char *body, const char *ct,
                  const char *bearer, char *out, size_t n, int ms) {
  (void)m; (void)ct; (void)bearer; (void)ms;
  snprintf(s_url, sizeof s_url, "%s", url);
  snprintf(s_body, sizeof s_body, "%s", body ? body : "");
  snprintf(out, n, "abc123\n");
  return 7;
}
static int c_ready(void) { return 1; }

void test_circuit_asks_claude_for_the_circuits_tracks(void) {
  start_with(0);
  CF.http = c_http; CF.net_ready = c_ready;
  ask(0);
  CHECK_EQ(M.view, V_ASK);
  snprintf(M.draft, sizeof M.draft, "dusty boom bap");
  key_ask(CAPP_KEY_ENTER);
  CHECK(strstr(s_url, "/midi/compose?kit=circuit") != NULL);
  CHECK(!strcmp(s_body, "dusty boom bap"));
  CHECK(!strcmp(M.job, "abc123"));
}

void test_circuit_c_changes_the_beat_and_u_puts_it_back(void) {
  start_with(0);
  CF.http = c_http; CF.net_ready = c_ready;
  M.sel = 0;
  open_sel();
  key_song('c');
  CHECK_EQ(M.view, V_ASK);
  snprintf(M.draft, sizeof M.draft, "busier");
  key_ask(CAPP_KEY_ENTER);
  CHECK(strstr(s_body, "busier\n---song---\n# Test beat") == s_body);
  /* Claude's answer lands over the song; u brings the old one back. */
  snprintf(M.text, TEXT_MAX, "ok\n# Test beat\ntempo 90\nch 1\nn 0 C4 1\n");
  M.job[0] = 0;
  CHECK(save_as(DIR "/test-beat.song", M.text + 3) == 0);
  key_song('u');
  CHECK(!strcmp(fakefs_get(DIR "/test-beat.song"), BEAT));
  CHECK_EQ(SG.tempo, 120);
}

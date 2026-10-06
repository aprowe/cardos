/* Songs as text into MIDI: apps/midiseq.h. */
#include <string.h>
#include "tinytest.h"
#include "apps/midiseq.h"

static MsSong S;

void test_midiseq_notes_names_and_numbers(void) {
  CHECK_EQ(ms_pitch("C4"), 60);
  CHECK_EQ(ms_pitch("A4"), 69);
  CHECK_EQ(ms_pitch("D#5"), 75);
  CHECK_EQ(ms_pitch("Eb5"), 75);
  CHECK_EQ(ms_pitch("C-1"), 0);
  CHECK_EQ(ms_pitch("G9"), 127);
  CHECK_EQ(ms_pitch("64"), 64);
  CHECK_EQ(ms_pitch("H4"), -1);
  CHECK_EQ(ms_pitch("A10"), -1);
}

void test_midiseq_beats_are_decimals_or_fractions(void) {
  CHECK_EQ(ms_ticks("0"), 0);
  CHECK_EQ(ms_ticks("1"), 480);
  CHECK_EQ(ms_ticks("0.25"), 120);
  CHECK_EQ(ms_ticks("1/3"), 160);
  CHECK_EQ(ms_ticks("0.333"), 160);
  CHECK_EQ(ms_ticks("2.5/2"), 600);
  CHECK_EQ(ms_ticks("x"), -1);
  CHECK_EQ(ms_ticks("1/0"), -1);
}

void test_midiseq_a_short_song_becomes_timed_messages(void) {
  static const char SONG[] =
    "# Fur Elise, opening\n"
    "tempo 120\n"
    "prog 0\n"
    "n 0 E5 0.5 80\n"
    "n 0.5 D#5 0.5    ; a comment\n"
    "n 1 A3,E4 1\n";
  CHECK_EQ(ms_parse(&S, SONG), 0);
  CHECK(!strcmp(S.title, "Fur Elise, opening"));
  CHECK_EQ(S.tempo, 120);
  CHECK_EQ(S.nnotes, 4);
  /* prog at 0, E5 on at 0, E5 off at 240 then D#5 on at 240, ... */
  CHECK_EQ(S.ev[0].b[0], 0xC0);
  CHECK_EQ(S.ev[1].b[0], 0x90); CHECK_EQ(S.ev[1].b[1], 76); CHECK_EQ(S.ev[1].b[2], 80);
  CHECK_EQ(S.ev[2].tick, 240u); CHECK_EQ(S.ev[2].b[0], 0x80);   /* off first */
  CHECK_EQ(S.ev[3].tick, 240u); CHECK_EQ(S.ev[3].b[0], 0x90);
  CHECK_EQ(S.ev[3].b[2], 90);                                    /* the default */
  CHECK_EQ(ms_ms(&S, 480), 500u);                                /* a beat at 120 */
  CHECK_EQ(S.end_ticks, 960u);
  CHECK_EQ(S.loop, 0);
}

void test_midiseq_channels_cc_ramps_bends_and_loops(void) {
  static const char SONG[] =
    "ch 10\n"
    "n 0 36 0.25\n"
    "cc 0 7 100\n"
    "ramp 0 1 74 0 127\n"
    "bend 1 -8192\n"
    "loop\n";
  int i, ramps = 0, last = -1;
  CHECK_EQ(ms_parse(&S, SONG), 0);
  CHECK_EQ(S.ev[0].b[0] & 0x0F, 9);                  /* channel 10 */
  for (i = 0; i < S.nev; i++)
    if (S.ev[i].b[0] == 0xB9 && S.ev[i].b[1] == 74) {
      CHECK(S.ev[i].b[2] > last);
      last = S.ev[i].b[2];
      ramps++;
    }
  CHECK(ramps >= 8);
  CHECK_EQ(last, 127);
  CHECK_EQ(S.ev[S.nev - 1].b[0], 0xE9);               /* bend, last at beat 1 */
  CHECK_EQ(S.ev[S.nev - 1].b[1], 0);
  CHECK_EQ(S.ev[S.nev - 1].b[2], 0);
  CHECK_EQ(S.loop, 1);
  CHECK_EQ(S.loop_ticks, 4u * 480);                   /* the bar after the last thing */
}

void test_midiseq_a_decimal_tempo_is_rounded(void) {
  CHECK_EQ(ms_parse(&S, "tempo 42.5\nn 0 C4 1\n"), 0);
  CHECK_EQ(S.tempo, 43);
  CHECK_EQ(ms_parse(&S, "tempo 19.4\nn 0 C4 1\n"), -1);
}

void test_midiseq_errors_say_which_line(void) {
  CHECK_EQ(ms_parse(&S, "tempo 90\nn 0 H4 1\n"), -1);
  CHECK_EQ(S.err_line, 2);
  CHECK(strstr(S.err, "H4") != 0);
  CHECK_EQ(ms_parse(&S, "n 0 C4\n"), -1);
  CHECK(strstr(S.err, "how long") != 0);
  CHECK_EQ(ms_parse(&S, "jump 4\n"), -1);
  CHECK(strstr(S.err, "jump") != 0);
}

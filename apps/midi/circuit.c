/* Circuit: beats for a Novation Circuit, written by Claude from a prompt and
 * played into it through the MIDI converter.
 *
 * It is the MIDI app built again with CIRCUIT defined (apps/midi/midi.c says
 * what changes): the same songs, roll, preview and player, but Claude is
 * told the Circuit's eight tracks --
 *
 *   1 kick  2 snare  3 hi-hat  4 crash/special  5 keys  6 bass  7 misc  8 samples
 *
 * -- and keeps to them (apps/midi/server/__init__.py, kit=circuit). /config/circuit.txt
 * says which channel each track really plays on, and a note to play a
 * drum's every hit as, so a Circuit set up another way needs no new songs.
 * Beats are in /circuit. A beat is dense, so the song lives in a big-mode
 * block (CAPP_BIG), not the arena.
 *
 * @app folder Make
 * @app release extra
 */
#define CIRCUIT 1
#include "apps/midi/midi.c"

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_BIG,
  "Circuit",
  /* 16x16: a 4x4 grid of pads. */
  { 0x00, 0x00, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
    0x00, 0x00, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
    0x00, 0x00, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
    0x00, 0x00, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 },
  "In the list\nup/down\tchoose a beat\nenter\topen it\nspace\topen it and hear it\n"
  "n\tdescribe a beat; Claude writes it for the Circuit's tracks\nr\tread the folder again\n"
  "\nIn a beat\nspace\thear it on the speaker, drums as drums; again to stop\n"
  "enter\tplay it into the Circuit; again to stop\n+ -\ttempo\nl\tloop on and off\n"
  "c\task Claude to change it (\"busier hats, drop at bar 5\")\nu\tput back the version before\n"
  "e\tedit it in Edit\nesc\tstop, then back to the list\n"
  "\nAnywhere\nd, del\tdelete (asks)\ng\tMIDI out on G1 or G2\nt\ta test arpeggio on the keys track\n"
  "\ntracks: 1 kick, 2 snare, 3 hat, 4 crash/fx, 5 keys, 6 bass, 7 misc, 8 samples.\n"
  "/config/circuit.txt sends each to the channel (and drum note) your Circuit uses.\n",
};

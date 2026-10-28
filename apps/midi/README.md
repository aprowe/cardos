# MIDI and Circuit

_Moved from CLAUDE.md on 2026-10-27._

Songs are text in /songs (`apps/midi/midiseq.h`, host-
tested): `n BEAT NOTE LEN [VEL]`, chords, `cc`, `ramp`, `bend`, `prog`,
`ch`, `tempo`, `loop`, beats as decimals or fractions, integer ticks inside.
`n` in the app asks Claude (`apps/midi/server/__init__.py`, POST then poll); the reply is
checked against the format and a mistake goes back to Claude once. The
app's song buffers are sized to keep its data under 28 KB.

**Circuit: beats for a Novation Circuit** (2026-10-12, `apps/midi/circuit.c`,
Make). It is `apps/midi/midi.c` built again with `CIRCUIT` defined -- one
source, two apps. Claude is asked with the kit (`/midi/compose?kit=circuit`,
`apps/midi/server/__init__.py` CIRCUIT): channels 1-8 are kick, snare, hat, crash/fx,
keys, bass, misc, samples, no `prog`, and the check refuses channels past 8.
`/config/circuit.txt` sends each track to the channel (and a drum to the
note) the Circuit really listens on, so songs never change for a setup.
Beats in /circuit; the preview plays drums as synthesised hits; the roll is
a lane a track. A beat is dense (1536 events), so the song is a big-mode
block (`CAPP_BIG`), not the arena. `c` there asks the composer for a change
(it knows the tracks; the talk would not), `u` undoes -- in MIDI too.

A MIDI song opens like a project: Enter on the
list shows the roll, and play, hear, tempo, loop, ask, edit happen there.

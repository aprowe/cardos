/* MIDI out of the Grove port, and a player that keeps time. Device-only.
 *
 * MIDI is a serial line at 31250 baud; a Grove-to-MIDI converter takes it on
 * one of the port's two signal pins (G1 or G2 on both Cardputers), and which
 * one depends on the converter -- so the caller says. UART1, transmit only.
 *
 * The player is a task, not the app's tick: the shell's loop stalls for tens
 * of milliseconds whenever something paints or reads the card, and in music
 * that is heard. The app hands over the whole song as time-stamped messages
 * (copied), and the task sends each at its time, looping if asked. Stopping
 * sends note-off for everything still sounding and all-notes-off on every
 * channel, so nothing is left hanging on the synth.
 */
#ifndef CARDOS_MIDI_H
#define CARDOS_MIDI_H

#include <stdint.h>

typedef struct {
  uint32_t at_ms;      /* from the start */
  uint8_t  n;          /* 1..3 bytes */
  uint8_t  b[3];       /* status, data, data */
} MidiEvent;

int  midi_open(int tx_pin);                 /* 0, or -1 */
void midi_close(void);
int  midi_send(const uint8_t *b, int n);    /* now; 0 or -1 */

/* Play `ev` (sorted by time). loop_ms 0 plays once; otherwise it starts
 * again at loop_ms. 0, -1 not open, -2 no memory. A song playing is
 * replaced. */
int  midi_play(const MidiEvent *ev, int n, uint32_t loop_ms);
void midi_stop(void);
int  midi_playing(void);
uint32_t midi_pos_ms(void);                 /* into the song, this time round */

/* Let go of a player started by `owner` (an app's run), when it closes. */
void midi_release_owner(const void *owner);

#endif /* CARDOS_MIDI_H */

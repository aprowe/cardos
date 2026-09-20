/* The speaker: NS4168 class-D amplifier on I2S, BCLK 41, DATA 42, LRCLK 43.
 *
 * Plays a PCM WAV from the card, streaming it a block at a time -- a memo is
 * megabytes and the heap is not. 16-bit samples, mono or stereo, at
 * whatever rate the header says (8 to 48 kHz); anything else is refused
 * with a reason rather than played as noise.
 *
 * G43 is the microphone's PDM clock as well as this LRCLK, so recording and
 * playback are mutually exclusive: speaker_play_wav closes the mic before
 * it takes the pins, and gives them back when it is done. */
#ifndef CARDOS_SPEAKER_H
#define CARDOS_SPEAKER_H

#include <stdint.h>

/* What a WAV file says about itself. 0 if it is one this driver can play. */
typedef struct {
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint32_t data_bytes;
  uint32_t data_offset;
} WavInfo;
int speaker_wav_info(const char *path, WavInfo *out, const char **why);

/* Play `path` until it ends or `stop()` returns non-zero (polled between
 * blocks, ~32 ms). `progress`, if given, gets the byte offset played so far,
 * for a bar. Blocking: run it on a task. Returns 0, or -1 with the reason
 * in speaker_error(). */
int  speaker_play_wav(const char *path, int (*stop)(void),
                      void (*progress)(uint32_t bytes));
const char *speaker_error(void);

/* Volume, 0..100, remembered across reboots. 60 by default; the NS4168 is
 * loud. Takes effect on the next block, so mid-playback too. */
void speaker_set_volume(int pct);
int  speaker_volume(void);

#endif /* CARDOS_SPEAKER_H */

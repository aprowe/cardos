/* The speaker: NS4168 class-D amplifier on I2S, BCLK 41, DATA 42, LRCLK 43.
 *
 * Plays a PCM WAV from the card, streaming it a block at a time -- a memo is
 * megabytes and the heap is not. 16-bit samples, mono or stereo, at
 * whatever rate the header says (8 to 48 kHz); anything else is refused
 * with a reason rather than played as noise.
 *
 * G43 is the microphone's PDM clock as well as this LRCLK, so recording and
 * playback are mutually exclusive. One lock says whose the pin is --
 * speaker_pins_take -- and the mic takes it too (mic_record_wav). Opening
 * the channel while the mic is open is refused; it used to close the mic
 * under whoever was recording. */
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

/* The same, with two more questions asked between blocks: `paused()` holds
 * the place and sends silence -- the channel stays up, so resuming does not
 * click -- and `seek()` returns a byte offset into the audio to go to, or -1
 * for none. Either may be NULL. */
int  speaker_play_wav_ex(const char *path, int (*stop)(void),
                         void (*progress)(uint32_t bytes),
                         int (*paused)(void), int32_t (*seek)(void));
const char *speaker_error(void);

/* n mono samples from memory, already at the volume wanted; blocks until
 * played. -1 at once if the channel is in use -- for blip.c, whose sounds
 * are worth nothing late. */
int speaker_play_pcm(const int16_t *pcm, int n, uint32_t rate);

/* G43, held: 0 once it is the caller's (waiting up to wait_ms), -1 if
 * someone else kept it. A mutex: give it back from the same task. The
 * play functions above take it themselves; the mic's recorder takes it
 * for as long as it records. */
int  speaker_pins_take(int wait_ms);
void speaker_pins_give(void);

/* The channel is up (playing, or a blip). */
int  speaker_is_open(void);

/* Volume, 0..100, remembered across reboots. 60 by default; the NS4168 is
 * loud. Takes effect on the next block, so mid-playback too. */
void speaker_set_volume(int pct);
int  speaker_volume(void);

#endif /* CARDOS_SPEAKER_H */

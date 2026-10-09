/* Recording and playback as jobs an app can watch.
 *
 * The mic and the speaker drivers both block for as long as the sound
 * lasts, which is right for the voice button (the machine is deliberately
 * doing one thing) and wrong for an app that wants to draw a meter and take
 * a key to stop. So each runs on a task of its own here, and the app polls
 * from tick -- the same shape as printq and httpq, for the same reason:
 * the shell is one cooperative loop.
 *
 * One job at a time, and the two are exclusive by hardware anyway: G43 is
 * the mic's clock and the speaker's LRCLK.
 *
 * WHO HAS THE AUDIO. A job here, or a claim: the voice button records from
 * the shell, outside any job, and used to open the mic beside Music's
 * playback (both driving G43) or beside Memo's recording (one channel, two
 * readers, and the first to finish deleting it under the other). Now
 * kernel/sys/voice.c claims the audio first; while it holds it, a job is
 * refused (-1, busy), and while a job runs, the claim is. The drivers
 * enforce the pin underneath (speaker_pins_take). */
#ifndef CARDOS_AUDIO_H
#define CARDOS_AUDIO_H

#include <stdint.h>

enum { AUDIO_IDLE = 0, AUDIO_RECORDING, AUDIO_PLAYING };

/* Record a WAV (16 kHz mono 16-bit) to `path` for up to `max_ms`, or until
 * audio_stop. A NULL path listens only: audio_level moves, nothing is kept.
 * 0 started, -1 busy, -2 the mic or the card refused. */
int  audio_record(const char *path, int max_ms);

/* Play a PCM WAV. 0 started, -1 busy, -2 unplayable (audio_error says). */
int  audio_play(const char *path);

void audio_stop(void);
/* While playing: hold the place (1) or go on (0); the state stays
 * AUDIO_PLAYING. Seek to a time in the file. Nothing while idle. */
void audio_pause(int on);
int  audio_paused(void);
void audio_seek_ms(uint32_t ms);
int  audio_state(void);          /* AUDIO_* */

/* While recording: the last block's loudness, 0..100. Otherwise -1. */
int  audio_level(void);

/* While playing: how far, in milliseconds, and the whole length. */
uint32_t audio_pos_ms(void);
uint32_t audio_total_ms(void);

/* Bytes recorded by the last recording, or -1 if it failed. */
int  audio_last_bytes(void);
const char *audio_error(void);

/* The audio for someone outside a job (`who`, a static string, for
 * messages). 0 if it is theirs now, -1 if a job is running or someone else
 * holds it. Give it back with audio_release, from wherever. */
int  audio_claim(const char *who);
void audio_release(void);
/* Who has it -- a claimant's name, "a recording", "playback" -- or NULL. */
const char *audio_holder(void);

#endif /* CARDOS_AUDIO_H */

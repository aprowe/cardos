/* The microphone: SPM1423, PDM, on DAT 46 and CLK 43.
 *
 * Records straight to a file rather than to memory. Twenty seconds at 16 kHz
 * mono 16-bit is 640 KB and this machine has about 120 KB of heap; the card is
 * right there and the recogniser wants a file anyway.
 *
 * 16 kHz mono 16-bit is not a preference, it is what whisper.cpp takes without
 * resampling. Recording at anything else would mean converting on a device
 * that has no reason to own that code.
 *
 * G43 is shared with the speaker's LRCLK, so recording and playback are
 * mutually exclusive on this hardware. kernel/drv/speaker.c calls mic_close
 * before it takes the pins, and kernel/sys/audio.c runs one or the other.
 */
#ifndef CARDOS_MIC_H
#define CARDOS_MIC_H

#include <stdint.h>

#define MIC_RATE     16000
#define MIC_MAX_MS   15000     /* the voice button: a sentence, 480 KB at most */
#define MIC_HARD_MAX_MS (10 * 60 * 1000)   /* a memo: ten minutes, 19 MB, on the card */

/* Take the pins and start the converter. Cheap to call repeatedly; the second
 * call is a no-op. Returns 0, or -1 if the I2S channel could not be opened. */
int  mic_open(void);

/* Give the pins back. Called when a recording ends, so a future speaker driver
 * can have G43 without a reboot. */
void mic_close(void);

/* Record into `path` as a WAV, until `stop()` returns non-zero or max_ms
 * elapses, whichever comes first. `stop` is polled between blocks -- roughly
 * every 32 ms -- which is what makes push-to-talk feel immediate.
 *
 * `level`, if given, is called with a 0..100 loudness for the block just
 * captured, so a caller can draw a meter and the speaker can see the thing is
 * listening. Nothing is more disheartening than talking at a still screen.
 *
 * A NULL `path` listens without keeping anything: the level still comes,
 * nothing goes to the card. For a meter or a toy that runs for minutes, where
 * a file would be 32 KB a second of wear for nothing.
 *
 * Returns bytes of audio written (excluding the 44-byte header), or -1. With
 * no path, the bytes that were heard. */
int  mic_record_wav(const char *path, int max_ms,
                    int (*stop)(void), void (*level)(int pct));

#endif /* CARDOS_MIC_H */

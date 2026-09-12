/* Push to talk.
 *
 * The button on top records for as long as it is held, the words go to the
 * proxy, and one of two things happens:
 *
 *   the words start with "Carlos"   they are a command, and the device runs it
 *   otherwise                       they are text, and get typed into whatever
 *                                   has focus
 *
 * Which of those it is gets decided here rather than in an app, because the
 * answer has to be the same everywhere. An app that had to implement voice
 * would implement it differently from the next one.
 */
#ifndef CARDOS_VOICE_H
#define CARDOS_VOICE_H

/* Poll the button and drive the whole cycle. Called from the main loop next to
 * the keyboard, so it works in every shell and over every app -- including one
 * that owns the screen.
 *
 * Returns 1 if something happened worth repainting for. */
int voice_tick(void);

/* Record, transcribe and act. Blocks for the length of the recording plus the
 * round trip.
 *
 * `hold` says whether the button is the thing that started this. With it, the
 * recording ends when the button comes up. Without it -- `listen` on the
 * console, where there is no button being held -- it runs for max_ms.
 *
 * That distinction is not decoration: the first version polled the button
 * either way, so `listen` stopped on its first 32 ms block and reported "too
 * short" every time. The one path that exists for testing without a button
 * was the one path a button was required for. */
void voice_once(int max_ms, int hold);

/* What the last attempt did, as a sentence: heard, typed, ran, or why not. */
const char *voice_status(void);

/* Is it recording right now? The shells draw a marker when it is, because
 * talking at a device that gives no sign of listening is unpleasant. */
int voice_recording(void);

/* 0..100, the loudness of the last block captured. For that marker. */
int voice_level(void);

#endif /* CARDOS_VOICE_H */

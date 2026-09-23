/* The G0 button's two gestures, told apart by timing.
 *
 *   hold            talk to the machine: text, or a "Carlos" command
 *   tap, then hold  record a memo to /home/memos, until released -- offline
 *
 * Every press starts recording at once, so a spoken sentence loses nothing
 * to the wait for a decision. Only when it ends is it known to have been a
 * tap (shorter than G0_TAP_MS): that recording is thrown away, and a press
 * that comes within G0_GAP_MS of the tap's release is a memo.
 *
 * Portable, so the host suite covers the timing; kernel/sys/voice.c is the
 * device glue. Times are milliseconds on a counter that may wrap. */
#ifndef CARDOS_G0GESTURE_H
#define CARDOS_G0GESTURE_H

#include <stddef.h>
#include <stdint.h>

#define G0_TAP_MS 300          /* a press shorter than this was a tap */
#define G0_GAP_MS 500          /* how long after a tap a press means memo */

enum { G0_VOICE = 0, G0_MEMO = 1 };

typedef struct {
  uint32_t tap_up_ms;          /* when the last tap was released */
  int      armed;              /* a tap happened and has not been used */
} G0Gesture;

/* The button went down: which gesture this press is. Uses up a tap. */
int g0_press(G0Gesture *g, uint32_t now_ms);

/* A voice press came back up after held_ms. 1 if it was only a tap -- throw
 * its recording away; the next press may be a memo -- or 0 to keep it. */
int g0_release(G0Gesture *g, uint32_t held_ms, uint32_t now_ms);

/* Where a memo goes, named as apps/memo.c names them so the two sort
 * together: DIR/MMDD-HHMMSS.wav when the clock is known, DIR/mNNNN.wav
 * (NNNN = counter) when it is not. */
void memo_filename(char *out, size_t n, const char *dir, int have_time,
                   int month, int day, int hour, int min, int sec, int counter);

#endif

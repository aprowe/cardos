/* A panel drawn over whatever is on the screen.
 *
 * Voice needs this and nothing else does yet. Talking at a machine that shows
 * no sign of listening is unpleasant, and the moment where it has stopped
 * listening but has not answered -- the upload -- is exactly when a person
 * starts wondering whether it heard them at all. So there are three states and
 * the overlay shows which one it is in.
 *
 * It draws over the top of an app without telling it, which is fine going on
 * and a problem coming off: the panel cannot be erased, because nothing here
 * knows what was underneath. `overlay_close` therefore asks the active shell
 * to repaint itself, which is the only thing that does know -- the same answer
 * the mouse pointer arrived at.
 */
#ifndef CARDOS_OVERLAY_H
#define CARDOS_OVERLAY_H

#include <stdint.h>

/* Recording. `level` is 0..100 from the microphone and drives a meter, so the
 * bar moves while a person talks -- which is the whole point of it. */
void overlay_listening(int level);

/* Sending the recording. `pct` is how much of the file has gone; a real number
 * because half a megabyte over WiFi takes a few seconds and a spinner would be
 * lying about knowing nothing. */
void overlay_sending(int pct);

/* Waiting on something with no progress to report -- recognition, or a model
 * deciding what a sentence meant. `what` is a short line. */
void overlay_working(const char *what);

/* What happened, left up long enough to read. */
void overlay_result(const char *text);

/* Take it down and put back what it covered. */
void overlay_close(void);

#endif /* CARDOS_OVERLAY_H */

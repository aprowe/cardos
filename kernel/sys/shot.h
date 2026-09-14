/* Screenshots. Device-only.
 *
 * The panel cannot be read and there is no framebuffer, so a screenshot is
 * taken by repainting the whole screen with a tap on display_blit that
 * mirrors every row into a file on the card: 240x135 RGB565, 64,800 bytes,
 * the card standing in for the framebuffer the RAM cannot hold. The file is
 * then posted to the proxy, which turns it into a PNG (tools/shots.py). It
 * stays on the card either way, under /shots, with the extension Photos
 * looks for: a screenshot is a picture, and the device can show its own.
 */
#ifndef CARDOS_SHOT_H
#define CARDOS_SHOT_H

/* Take one. `name` is the file's stem, letters and digits; `repaint` is
 * whatever paints the whole of the current shell -- it is called once with
 * the tap in place. Returns 0 when the file is on the card, whether or not
 * the proxy took it; `shot_error` says what went wrong otherwise. */
int shot_take(const char *name, void (*repaint)(void));
const char *shot_error(void);

/* Where the last one went on the card, for a shell that wants to say. */
const char *shot_last_path(void);

#endif /* CARDOS_SHOT_H */

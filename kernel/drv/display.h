/* ST7789V2 display driver for the Cardputer. Device-only.
 *
 * The panel is natively 135x240; CardOS uses it as 240x135 landscape.
 */
#ifndef CARDOS_DISPLAY_H
#define CARDOS_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

#define DISPLAY_W 240
#define DISPLAY_H 135

/* RGB565 -- but stored byte-swapped.
 *
 * The ESP32 is little-endian, so a uint16_t goes onto the wire low byte first,
 * while the ST7789 reads each pixel most-significant byte first. Storing the
 * bytes pre-swapped is what makes the panel see the colour that was asked for.
 * Confirmed on hardware: without this, green (0x07E0) arrives as 0xE007, which
 * is bright red -- and that is exactly how the first boot came out.
 *
 * Doing it here rather than in the blit keeps it free: every pixel CardOS
 * draws comes from these macros, so nothing has to be swapped at run time. */
#define SWAP16(v) ((uint16_t)((((uint16_t)(v)) >> 8) | (((uint16_t)(v)) << 8)))

#define RGB565(r, g, b) \
  SWAP16((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define COLOR_BLACK   SWAP16(0x0000)
#define COLOR_WHITE   SWAP16(0xFFFF)
#define COLOR_GREEN   SWAP16(0x07E0)
#define COLOR_AMBER   SWAP16(0xFD20)
#define COLOR_RED     SWAP16(0xF800)
#define COLOR_GREY    SWAP16(0x8410)

int  display_init(void);

/* Landscape orientation is four mirror combinations crossed with two possible
 * x gaps -- the 135x240 panel sits in a 240x320 controller frame, so mirroring
 * moves the origin to the far corner and the offset becomes (240-135-52)=53
 * instead of 52. Which one is right cannot be worked out from a datasheet with
 * any confidence, so it is selectable at runtime and the answer gets baked in
 * as the default. */
#define DISPLAY_ORIENTS 8
void display_set_orient(int n);
int  display_orient(void);

/* Push a rectangle of RGB565 pixels. Blocking. */
void display_blit(int x, int y, int w, int h, const uint16_t *pixels);

void display_fill(uint16_t color);
void display_backlight(int on);

#endif /* CARDOS_DISPLAY_H */

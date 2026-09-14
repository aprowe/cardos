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

/* A copy of every blit, for a screenshot. There is no framebuffer to read
 * and the panel is write-only, so the only way to know what is on the screen
 * is to watch it being sent: set a tap, repaint everything, clear the tap.
 * Called on the drawing task after the pixels are on the panel; NULL to
 * stop. One at a time. */
typedef void (*DisplayTap)(int x, int y, int w, int h, const uint16_t *pixels);
void display_set_tap(DisplayTap tap);

void display_fill(uint16_t color);
void display_backlight(int on);

/* Backlight level in percent, PWM'd. Persisted in NVS by set; load reads it
 * back and applies it, so call it once NVS is up. Below the minimum the panel
 * is off to the eye, which is what display_backlight(0) is for. */
/* The dimmest level the setting can reach.
 *
 * The history is worth keeping, because the first two explanations were both
 * wrong. 25 blacked the screen, so the floor went to 50 -- and then 50 and 75
 * blacked it too, which is not what a threshold looks like. The fault was the
 * PWM itself, not the level: 5 kHz is faster than the converter behind this
 * backlight can switch. At 256 Hz with a duty floor (see display.c) the whole
 * range works, so the setting's floor is only about legibility now.
 *
 * opt-0 and safe mode remain the way back regardless. Anything that can make
 * the screen unreadable needs an escape that does not require reading it. */
#define DISPLAY_BRIGHT_MIN     25
#define DISPLAY_BRIGHT_DEFAULT 100
int  display_brightness(void);
void display_set_brightness(int pct);
void display_set_brightness_now(int pct);   /* applied, not saved */
void display_load_brightness(void);

#endif /* CARDOS_DISPLAY_H */

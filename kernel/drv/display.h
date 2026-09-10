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

/* RGB565, which is what the panel takes. */
#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_GREEN   0x07E0
#define COLOR_AMBER   0xFD20
#define COLOR_RED     0xF800
#define COLOR_GREY    0x8410

int  display_init(void);

/* Push a rectangle of RGB565 pixels. Blocking. */
void display_blit(int x, int y, int w, int h, const uint16_t *pixels);

void display_fill(uint16_t color);
void display_backlight(int on);

#endif /* CARDOS_DISPLAY_H */

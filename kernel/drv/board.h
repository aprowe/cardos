/* Which Cardputer this is, and the I2C bus the ADV hangs its chips on.
 * Device-only.
 *
 * One firmware runs on both. They share the chip, the flash, the screen and
 * its pins, the card, the radios -- everything the apps can see. They differ
 * in three things:
 *
 *   original (2023)  keyboard: a 74HC138 matrix scanned on GPIO
 *                    mic: SPM1423 PDM on 46/43; speaker: NS4168 I2S on 41/42/43
 *   ADV (2025)       keyboard: a TCA8418 on I2C 8/9, its interrupt on 11
 *                    audio: an ES8311 codec on the same I2C, I2S on 41/42/43/46
 *                    and a BMI270 motion sensor on the same I2C
 *
 * The ADV is told by asking for the TCA8418 at 0x34 on 8/9 before anything
 * else touches those pins. On the original they are the 74HC138's address
 * inputs: a probe there finds nothing and does nothing, and the bus is taken
 * down again so the keyboard can have the pins.
 *
 * Two binaries were the alternative, and an update that put the wrong one on
 * a device would have left it with no keyboard -- only USB brings that back.
 */
#ifndef CARDOS_BOARD_H
#define CARDOS_BOARD_H

#include <stddef.h>
#include <stdint.h>

typedef enum { BOARD_ORIGINAL = 0, BOARD_ADV = 1 } Board;

/* Look once; before keyboard_init. Afterwards board() answers. */
Board board_detect(void);
Board board(void);
const char *board_name(void);        /* "Cardputer" or "Cardputer ADV" */

/* The ADV's I2C bus: 8/9, 400 kHz, shared by the keyboard, the codec and the
 * motion sensor. Calls are serialised; each returns 0 or -1. Device addresses
 * are 7-bit. Nothing here on the original: they return -1. */
int board_i2c_write(uint8_t addr, const uint8_t *data, size_t n);
int board_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value);
int board_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out, size_t n);
int board_i2c_present(uint8_t addr);

#endif /* CARDOS_BOARD_H */

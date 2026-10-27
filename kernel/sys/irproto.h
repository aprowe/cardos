/* Infrared remote protocols as timings. Portable; host-tested.
 *
 * A remote's code goes out as marks (the LED flashing at the carrier,
 * 38 kHz for NEC) and spaces (dark), and this turns a code into those
 * durations: microseconds (32 bits: a repeat frame ends in 96 ms of dark),
 * alternating, a mark first. kernel/drv/irtx.c is
 * the transmitter that plays them.
 *
 * NEC is what nearly every cheap remote speaks -- LED strips and lamps,
 * the 24- and 44-key RGB remotes among them. A frame is a 9 ms mark, a
 * 4.5 ms space, 32 bits least significant first (562 us mark, then a
 * 562 us space for 0 or 1687 us for 1), and a closing mark. Holding a
 * button sends the frame once, then a short repeat frame every 108 ms.
 *
 * The 32 bits are written two ways, and both are here:
 *   IR_NEC      address and command, as IRremote and IRDB give them:
 *               address <= 0xFF sends the address then its inverse;
 *               above that it is a 16-bit "extended" address, low byte
 *               first. The command is followed by its inverse.
 *   IR_NEC_MSB  the 32-bit value older tools printed ("0xF7C03F"): its
 *               bits sent most significant first, as received.
 */
#ifndef CARDOS_IRPROTO_H
#define CARDOS_IRPROTO_H

#include <stdint.h>

#define IR_NEC      1
#define IR_NEC_MSB  2

#define IR_NEC_HZ        38000
#define IR_NEC_FRAME_US  108000   /* frame start to frame start */

/* Durations for a frame of `proto` and `repeats` repeat frames after it,
 * each padded out to IR_NEC_FRAME_US (the last space is the gap), into
 * `out`. The count written (always even: mark, space, ...), or -1 for an
 * unknown protocol or a code out of range, -2 when `max` is too small.
 * A frame is 68 durations, a repeat 4. */
int ir_encode(int proto, uint32_t addr, uint32_t cmd, int repeats,
              uint32_t *out, int max);

/* The carrier for `proto`, in Hz; 0 if unknown. */
uint32_t ir_carrier(int proto);

#endif /* CARDOS_IRPROTO_H */

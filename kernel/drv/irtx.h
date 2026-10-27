/* The infrared LED on G44: a remote control's transmitter. Device-only.
 *
 * Both Cardputers have an IR LED on G44 and no receiver, so the device can
 * send what a remote sends but cannot learn a remote by listening: a code
 * has to come from a table (kernel/sys/irproto.h turns one into timings).
 *
 * The RMT peripheral plays the timings: a mark is the LED flashing at the
 * carrier, a space is dark. The channel is made the first time something
 * is sent, not at boot -- most sessions never send -- and kept.
 */
#ifndef CARDOS_IRTX_H
#define CARDOS_IRTX_H

#include <stdint.h>

/* Play `n` durations in microseconds, alternating, a mark first, the marks
 * modulated at `carrier_hz` (0: not modulated). Blocks until the last one
 * has gone. 0, -1 the transmitter would not start or the send failed,
 * -2 bad arguments, -4 out of memory. */
int irtx_send(const uint32_t *us, int n, uint32_t carrier_hz);

/* A code of a kernel/sys/irproto.h protocol, with `repeats` repeat frames
 * after it (a held button). The same returns, and -2 for a code the
 * protocol cannot carry. */
int irtx_code(int proto, uint32_t addr, uint32_t cmd, int repeats);

#endif /* CARDOS_IRTX_H */

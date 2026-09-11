/* RAM-backed SwapDev for the host tests.
 *
 * Every access is bounds-checked. That is the point: an off-by-one in the
 * swap layer becomes a loud test failure here instead of silent corruption on
 * a device with no debugger.
 */
#ifndef FAKE_SWAPDEV_H
#define FAKE_SWAPDEV_H

#include "kernel/swap/swap.h"

typedef struct FakeSwapDev FakeSwapDev;

FakeSwapDev   *fake_swapdev_create(uint32_t sectors);
void           fake_swapdev_destroy(FakeSwapDev *f);
const SwapDev *fake_swapdev_dev(FakeSwapDev *f);

unsigned long  fake_swapdev_writes(FakeSwapDev *f);   /* sectors written */
unsigned long  fake_swapdev_reads(FakeSwapDev *f);    /* sectors read */
unsigned long  fake_swapdev_faults(FakeSwapDev *f);   /* out-of-range accesses */

/* Make the next write call fail, so error propagation can be tested. */
void           fake_swapdev_fail_next_write(FakeSwapDev *f);

#endif /* FAKE_SWAPDEV_H */

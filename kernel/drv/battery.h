/* The battery, which until now the OS did not know it had.
 *
 * There is no power-management chip on this board: the cell sits behind a
 * two-to-one divider on GPIO10 and that is the whole of it. So there is no
 * charge current, no coulomb counting and no time-remaining -- a voltage, and
 * what a lithium cell's voltage means.
 *
 * The pin and the ratio are from M5Unified's own board table (Power_Class.cpp,
 * board_M5Cardputer: ADC1_GPIO10_CHANNEL, _adc_ratio 2.0f), which is the same
 * source the display and mic pin maps came from and the same reason to trust
 * them: that library drives this hardware every day.
 */
#ifndef CARDOS_BATTERY_H
#define CARDOS_BATTERY_H

#include <stdint.h>

/* Open the ADC. Returns 0, or -1 if the converter would not start -- in which
 * case every reading below reports "unknown" rather than a made-up number. */
int battery_init(void);

/* Millivolts at the cell, averaged over a few samples. 0 if unknown. */
int battery_mv(void);

/* 0..100, or -1 if unknown.
 *
 * From a curve, not a straight line. A lithium cell spends most of its charge
 * between 3.9 and 3.7 volts and then falls off a cliff, so treating 3.3-4.2 as
 * linear reports 50% when the truth is nearer 15% -- which is worse than
 * saying nothing, because it is believed. */
int battery_percent(void);

/* Is it on the charger? Inferred, not measured: there is no status pin, but a
 * cell reading above its own full-charge voltage is being held there by
 * something. Rough, and labelled rough wherever it is shown. */
int battery_charging(void);

#endif /* CARDOS_BATTERY_H */

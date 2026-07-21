#ifndef PULSE_SOURCE_HW_H
#define PULSE_SOURCE_HW_H

#include <stdint.h>

/*
 * Hardware pulse source: phototransistor → LPCOMP → PPI → TIMER (counter).
 *
 * Entire chain runs during System-ON sleep with the CPU asleep — hitting
 * the multi-year AAA battery-life target from the design doc. The counter
 * is read on wake and fed to pulse_accumulator_update(); its 32-bit wrap
 * is handled by the accumulator.
 *
 * The LPCOMP threshold is chosen via lpcomp_ref.h from a target voltage
 * and the current VDD. Hysteresis is enabled so ambient flicker below the
 * hysteresis band doesn't produce spurious counts.
 *
 * NOTE on the min-pulse-width filter (design doc requirement): first-pass
 * relies on LPCOMP's built-in hysteresis as the noise mitigation. A
 * hardware min-pulse-width filter (LPCOMP UP starts a TIMER-compare in
 * measurement mode, LPCOMP DOWN cancels it, TIMER COMPARE event PPIs into
 * the counter) is a follow-up — architecture sketched in pulse_source_hw.c
 * so the delta stays small.
 */

int pulse_source_hw_init(uint32_t threshold_mv, uint32_t vdd_mv);

/* Returns the current TIMER counter value (monotonic 32-bit; wrap handled
 * downstream by pulse_accumulator).
 */
uint32_t pulse_source_hw_count(void);

#endif

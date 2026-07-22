#ifndef PULSE_ACCUMULATOR_H
#define PULSE_ACCUMULATOR_H

#include <stdint.h>

struct pulse_accumulator {
	uint64_t total;
	uint32_t last_hw;
};

void pulse_accumulator_init(struct pulse_accumulator *acc);

void pulse_accumulator_update(struct pulse_accumulator *acc, uint32_t hw_counter);

/*
 * Restore the accumulator from persistence. `current_hw_counter` must be the
 * live TIMER value read *right before* this call — the accumulator uses it as
 * the baseline for the next delta so pulses that landed between the last
 * pre-reset save and now (e.g. during a warm reboot where the LPCOMP+PPI+TIMER
 * chain kept counting) aren't double-counted against `saved_total`.
 */
void pulse_accumulator_restore(struct pulse_accumulator *acc,
			       uint64_t saved_total,
			       uint32_t current_hw_counter);

uint64_t pulse_accumulator_total(const struct pulse_accumulator *acc);

#endif

#ifndef PULSE_ACCUMULATOR_H
#define PULSE_ACCUMULATOR_H

#include <stdint.h>

struct pulse_accumulator {
	uint64_t total;
	uint32_t last_hw;
};

void pulse_accumulator_init(struct pulse_accumulator *acc);

void pulse_accumulator_update(struct pulse_accumulator *acc, uint32_t hw_counter);

void pulse_accumulator_restore(struct pulse_accumulator *acc, uint64_t saved_total);

uint64_t pulse_accumulator_total(const struct pulse_accumulator *acc);

#endif

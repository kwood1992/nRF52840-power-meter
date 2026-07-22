#include "pulse_accumulator.h"

void pulse_accumulator_init(struct pulse_accumulator *acc)
{
	acc->total = 0;
	acc->last_hw = 0;
}

void pulse_accumulator_update(struct pulse_accumulator *acc, uint32_t hw_counter)
{
	uint32_t delta = hw_counter - acc->last_hw;

	acc->total += delta;
	acc->last_hw = hw_counter;
}

void pulse_accumulator_restore(struct pulse_accumulator *acc,
			       uint64_t saved_total,
			       uint32_t current_hw_counter)
{
	acc->total = saved_total;
	acc->last_hw = current_hw_counter;
}

uint64_t pulse_accumulator_total(const struct pulse_accumulator *acc)
{
	return acc->total;
}

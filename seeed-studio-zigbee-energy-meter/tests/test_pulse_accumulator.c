#include "pulse_accumulator.h"

#include <assert.h>
#include <stdio.h>

static void test_init_starts_at_zero(void)
{
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);

	assert(pulse_accumulator_total(&acc) == 0);
}

static void test_update_advances_total_by_delta(void)
{
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	pulse_accumulator_update(&acc, 5);

	assert(pulse_accumulator_total(&acc) == 5);
}

static void test_multiple_updates_track_absolute_hw_counter(void)
{
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	pulse_accumulator_update(&acc, 5);
	pulse_accumulator_update(&acc, 12);
	pulse_accumulator_update(&acc, 12);

	assert(pulse_accumulator_total(&acc) == 12);
}

static void test_hw_counter_wrap_handled(void)
{
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	pulse_accumulator_update(&acc, 0xFFFFFFFEu);
	pulse_accumulator_update(&acc, 3u);

	assert(pulse_accumulator_total(&acc) == (uint64_t)0xFFFFFFFEu + 5u);
}

static void test_total_survives_many_wraps_past_32bit(void)
{
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	for (int i = 0; i < 5; i++) {
		pulse_accumulator_update(&acc, 0xFFFFFFFFu);
		pulse_accumulator_update(&acc, 0u);
	}

	assert(pulse_accumulator_total(&acc) == 5ULL * 0x100000000ULL);
}

static void test_restore_resumes_from_saved_total(void)
{
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	pulse_accumulator_restore(&acc, 12345ULL);
	pulse_accumulator_update(&acc, 100u);

	assert(pulse_accumulator_total(&acc) == 12445ULL);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("pulse_accumulator tests\n");
	RUN(test_init_starts_at_zero);
	RUN(test_update_advances_total_by_delta);
	RUN(test_multiple_updates_track_absolute_hw_counter);
	RUN(test_hw_counter_wrap_handled);
	RUN(test_total_survives_many_wraps_past_32bit);
	RUN(test_restore_resumes_from_saved_total);
	printf("all tests passed\n");
	return 0;
}

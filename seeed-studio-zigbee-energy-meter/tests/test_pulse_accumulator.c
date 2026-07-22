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
	/* Cold boot: hw counter is 0 (LPCOMP+PPI+TIMER just came up). */
	pulse_accumulator_restore(&acc, 12345ULL, 0u);
	pulse_accumulator_update(&acc, 100u);

	assert(pulse_accumulator_total(&acc) == 12445ULL);
}

static void test_restore_with_nonzero_hw_counter_no_doublecount(void)
{
	/* Warm reboot: the hardware TIMER kept counting through the reset
	 * (Zigbee reboot, watchdog, System-ON sleep exit). If restore assumed
	 * hw_counter == 0, the next update() would add the entire current
	 * TIMER register value into the restored total. Passing the live
	 * counter as the baseline prevents that.
	 */
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	pulse_accumulator_restore(&acc, 12345ULL, 7000u);

	/* Immediately after restore, before any new pulses, total should
	 * still equal saved_total — no phantom pulses.
	 */
	pulse_accumulator_update(&acc, 7000u);
	assert(pulse_accumulator_total(&acc) == 12345ULL);

	/* Subsequent pulses count normally from the restored baseline. */
	pulse_accumulator_update(&acc, 7005u);
	assert(pulse_accumulator_total(&acc) == 12350ULL);
}

static void test_restore_handles_hw_wrap_after_warm_reboot(void)
{
	/* Warm reboot near the 32-bit TIMER wrap point. The next update
	 * observes the counter after it has wrapped past zero. Delta
	 * arithmetic must still land the right number of new pulses on top
	 * of the restored total (i.e. use the same wrap-aware subtraction
	 * as steady-state update()).
	 */
	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);
	pulse_accumulator_restore(&acc, 500ULL, 0xFFFFFFFEu);
	pulse_accumulator_update(&acc, 3u);

	assert(pulse_accumulator_total(&acc) == 500ULL + 5ULL);
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
	RUN(test_restore_with_nonzero_hw_counter_no_doublecount);
	RUN(test_restore_handles_hw_wrap_after_warm_reboot);
	printf("all tests passed\n");
	return 0;
}

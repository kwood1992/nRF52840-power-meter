#include "persist_policy.h"

#include <assert.h>
#include <stdio.h>

/* Interval and delta chosen small so the test cases are readable; the
 * production values live in main.c.
 */
#define TEST_INTERVAL_MS       300000ULL   /* 5 min */
#define TEST_MAX_PULSE_DELTA   100ULL

static void test_init_stores_thresholds(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	assert(p.interval_ms == TEST_INTERVAL_MS);
	assert(p.max_pulse_delta == TEST_MAX_PULSE_DELTA);
}

static void test_no_write_when_total_unchanged(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	/* Same total on both sides — nothing to persist even after a long
	 * quiet period. Meter's just not seeing pulses.
	 */
	assert(!persist_policy_should_write(&p, 42, 42, TEST_INTERVAL_MS * 10));
}

static void test_no_write_when_neither_threshold_reached(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	/* Total changed by 1, interval barely elapsed — no reason to spend
	 * a flash cycle yet.
	 */
	assert(!persist_policy_should_write(&p, 43, 42, 1000));
}

static void test_write_when_interval_reached(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	/* Only 1 new pulse, but the wall-clock cadence has hit — write. */
	assert(persist_policy_should_write(&p, 43, 42, TEST_INTERVAL_MS));
	assert(persist_policy_should_write(&p, 43, 42, TEST_INTERVAL_MS + 1));
}

static void test_write_when_pulse_delta_reached(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	/* Delta hits the safety net well before the wall-clock cadence
	 * — bounds worst-case data loss under a pulse burst.
	 */
	assert(persist_policy_should_write(&p, 142, 42, 1000));
	assert(persist_policy_should_write(&p, 143, 42, 1000));
}

static void test_delta_just_under_threshold_does_not_write(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	/* One-below-threshold — off-by-one guard. */
	assert(!persist_policy_should_write(&p, 141, 42, 1000));
}

static void test_write_when_both_thresholds_reached(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	assert(persist_policy_should_write(&p, 200, 42, TEST_INTERVAL_MS * 2));
}

static void test_large_values_do_not_overflow(void)
{
	struct persist_policy p;

	persist_policy_init(&p, TEST_INTERVAL_MS, TEST_MAX_PULSE_DELTA);

	/* Real accumulator will grow into the tens of millions over years.
	 * Confirm the delta comparison stays sane at large u64 values.
	 */
	uint64_t huge = 1ULL << 40;

	assert(persist_policy_should_write(&p, huge + 200, huge, 1000));
	assert(!persist_policy_should_write(&p, huge + 1, huge, 1000));
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("persist_policy tests\n");
	RUN(test_init_stores_thresholds);
	RUN(test_no_write_when_total_unchanged);
	RUN(test_no_write_when_neither_threshold_reached);
	RUN(test_write_when_interval_reached);
	RUN(test_write_when_pulse_delta_reached);
	RUN(test_delta_just_under_threshold_does_not_write);
	RUN(test_write_when_both_thresholds_reached);
	RUN(test_large_values_do_not_overflow);
	printf("all tests passed\n");
	return 0;
}

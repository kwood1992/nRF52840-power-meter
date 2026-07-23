#include "metering_scale.h"

#include <assert.h>
#include <stdio.h>

static void test_zero(void)
{
	struct metering_u48 v = metering_scale_to_u48(0);

	assert(v.low == 0);
	assert(v.high == 0);
}

static void test_small_value_stays_in_low(void)
{
	/* One kWh at the design-doc default 1000 imp/kWh divisor. */
	struct metering_u48 v = metering_scale_to_u48(1000);

	assert(v.low == 1000);
	assert(v.high == 0);
}

static void test_max_u32_stays_in_low(void)
{
	/* Exactly 2^32 - 1 — the largest value that still fits in `low`
	 * without touching `high`. Off-by-one guard on the split.
	 */
	struct metering_u48 v = metering_scale_to_u48(0xFFFFFFFFULL);

	assert(v.low == 0xFFFFFFFFULL);
	assert(v.high == 0);
}

static void test_value_spans_low_and_high(void)
{
	/* 2^32 — the boundary; low wraps to 0, high picks up 1. */
	struct metering_u48 v = metering_scale_to_u48(0x100000000ULL);

	assert(v.low == 0);
	assert(v.high == 1);
}

static void test_arbitrary_48bit_value(void)
{
	/* Mid-range value that exercises both halves. */
	uint64_t input = 0x0000ABCD12345678ULL;
	struct metering_u48 v = metering_scale_to_u48(input);

	assert(v.low == 0x12345678U);
	assert(v.high == 0xABCDU);
}

static void test_exact_u48_max_is_preserved(void)
{
	struct metering_u48 v =
		metering_scale_to_u48(METERING_SUMMATION_U48_MAX);

	assert(v.low == 0xFFFFFFFFU);
	assert(v.high == 0xFFFFU);
}

static void test_overflow_saturates_at_u48_max(void)
{
	/* 2^48 — first value that would wrap without clamping. Clamping to
	 * the max value keeps the summation visibly stuck rather than
	 * appearing to roll back to zero (which Z2M would treat as a
	 * meter reset and wipe the HA energy dashboard baseline).
	 */
	struct metering_u48 v = metering_scale_to_u48(0x1000000000000ULL);

	assert(v.low == 0xFFFFFFFFU);
	assert(v.high == 0xFFFFU);
}

static void test_huge_value_saturates_at_u48_max(void)
{
	struct metering_u48 v = metering_scale_to_u48(0xFFFFFFFFFFFFFFFFULL);

	assert(v.low == 0xFFFFFFFFU);
	assert(v.high == 0xFFFFU);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("metering_scale tests\n");
	RUN(test_zero);
	RUN(test_small_value_stays_in_low);
	RUN(test_max_u32_stays_in_low);
	RUN(test_value_spans_low_and_high);
	RUN(test_arbitrary_48bit_value);
	RUN(test_exact_u48_max_is_preserved);
	RUN(test_overflow_saturates_at_u48_max);
	RUN(test_huge_value_saturates_at_u48_max);
	printf("all tests passed\n");
	return 0;
}

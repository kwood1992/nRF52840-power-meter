#include "calibration.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_rejects_zero(void)
{
	/* Divisor=0 would trigger a divide-by-zero in the coordinator's
	 * raw × Multiplier ÷ Divisor math. Hard reject.
	 */
	assert(!calibration_is_valid_imp_per_kwh(0));
}

static void test_rejects_just_below_min(void)
{
	assert(!calibration_is_valid_imp_per_kwh(
		CALIBRATION_IMP_PER_KWH_MIN - 1));
}

static void test_accepts_min(void)
{
	assert(calibration_is_valid_imp_per_kwh(
		CALIBRATION_IMP_PER_KWH_MIN));
}

static void test_accepts_common_values(void)
{
	/* Real-world imp/kWh values from meter datasheets — the range
	 * we exist to support.
	 */
	assert(calibration_is_valid_imp_per_kwh(500));
	assert(calibration_is_valid_imp_per_kwh(800));
	assert(calibration_is_valid_imp_per_kwh(1000));
	assert(calibration_is_valid_imp_per_kwh(1200));
	assert(calibration_is_valid_imp_per_kwh(2000));
	assert(calibration_is_valid_imp_per_kwh(3200));
}

static void test_accepts_max(void)
{
	assert(calibration_is_valid_imp_per_kwh(
		CALIBRATION_IMP_PER_KWH_MAX));
}

static void test_rejects_just_above_max(void)
{
	assert(!calibration_is_valid_imp_per_kwh(
		CALIBRATION_IMP_PER_KWH_MAX + 1));
}

static void test_rejects_u24_ceiling_and_above(void)
{
	/* u24 wire ceiling — still rejected because the practical range
	 * cap is 10000, well below.
	 */
	assert(!calibration_is_valid_imp_per_kwh(0xFFFFFFU));
	assert(!calibration_is_valid_imp_per_kwh(UINT32_MAX));
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("calibration tests\n");
	RUN(test_rejects_zero);
	RUN(test_rejects_just_below_min);
	RUN(test_accepts_min);
	RUN(test_accepts_common_values);
	RUN(test_accepts_max);
	RUN(test_rejects_just_above_max);
	RUN(test_rejects_u24_ceiling_and_above);
	printf("all tests passed\n");
	return 0;
}

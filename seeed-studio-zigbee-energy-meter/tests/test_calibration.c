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

static void test_pulse_min_width_rejects_zero(void)
{
	/* 0 µs would defeat the filter entirely; matches the reject-not-clamp
	 * policy used for imp/kWh.
	 */
	assert(!calibration_is_valid_pulse_min_width_us(0));
}

static void test_pulse_min_width_rejects_just_below_min(void)
{
	assert(!calibration_is_valid_pulse_min_width_us(
		CALIBRATION_PULSE_MIN_WIDTH_US_MIN - 1));
}

static void test_pulse_min_width_accepts_min(void)
{
	assert(calibration_is_valid_pulse_min_width_us(
		CALIBRATION_PULSE_MIN_WIDTH_US_MIN));
}

static void test_pulse_min_width_accepts_common_values(void)
{
	/* Compile-time default + the bench-verified value from impl-2's
	 * end-to-end MQTT-write test.
	 */
	assert(calibration_is_valid_pulse_min_width_us(500));
	assert(calibration_is_valid_pulse_min_width_us(1000));
	assert(calibration_is_valid_pulse_min_width_us(2500));
	assert(calibration_is_valid_pulse_min_width_us(5000));
}

static void test_pulse_min_width_accepts_max(void)
{
	assert(calibration_is_valid_pulse_min_width_us(
		CALIBRATION_PULSE_MIN_WIDTH_US_MAX));
}

static void test_pulse_min_width_rejects_just_above_max(void)
{
	assert(!calibration_is_valid_pulse_min_width_us(
		CALIBRATION_PULSE_MIN_WIDTH_US_MAX + 1));
}

static void test_pulse_min_width_rejects_huge_values(void)
{
	/* NVS slot is u32; make sure the far end of that range is rejected
	 * rather than accidentally wrapping into the valid band.
	 */
	assert(!calibration_is_valid_pulse_min_width_us(0xFFFFFFU));
	assert(!calibration_is_valid_pulse_min_width_us(UINT32_MAX));
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
	RUN(test_pulse_min_width_rejects_zero);
	RUN(test_pulse_min_width_rejects_just_below_min);
	RUN(test_pulse_min_width_accepts_min);
	RUN(test_pulse_min_width_accepts_common_values);
	RUN(test_pulse_min_width_accepts_max);
	RUN(test_pulse_min_width_rejects_just_above_max);
	RUN(test_pulse_min_width_rejects_huge_values);
	printf("all tests passed\n");
	return 0;
}

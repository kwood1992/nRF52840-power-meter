#include "lpcomp_ref.h"

#include <assert.h>
#include <stdio.h>

static void test_midsupply_maps_to_step_8(void)
{
	/* 1500 mV at 3000 mV VDD = exactly 0.5 = 8/16. */
	assert(lpcomp_choose_ref_step_16(1500, 3000) == 8);
}

static void test_rounds_to_nearest(void)
{
	/* 1000/3000 = 0.333... = 5.33/16 → step 5, not 6.
	 * Uses round-half-up integer arithmetic.
	 */
	assert(lpcomp_choose_ref_step_16(1000, 3000) == 5);
	/* 1125/3000 = 6/16 exactly → step 6. */
	assert(lpcomp_choose_ref_step_16(1125, 3000) == 6);
	/* 1200/3000 = 6.4/16 → step 6. */
	assert(lpcomp_choose_ref_step_16(1200, 3000) == 6);
	/* 1300/3000 = 6.93/16 → step 7. */
	assert(lpcomp_choose_ref_step_16(1300, 3000) == 7);
}

static void test_low_target_clamps_to_step_1(void)
{
	/* Well below 1/16 VDD — clamp to 1 rather than 0 so LPCOMP isn't
	 * sitting on the low rail and firing on noise.
	 */
	assert(lpcomp_choose_ref_step_16(50, 3000) == 1);
	assert(lpcomp_choose_ref_step_16(0, 3000) == 1);
}

static void test_high_target_clamps_to_step_15(void)
{
	/* Above 15/16 VDD — clamp to 15 for the same reason (avoid the
	 * high rail).
	 */
	assert(lpcomp_choose_ref_step_16(2900, 3000) == 15);
	assert(lpcomp_choose_ref_step_16(3000, 3000) == 15);
	assert(lpcomp_choose_ref_step_16(4000, 3000) == 15);
}

static void test_zero_vdd_returns_safe_fallback(void)
{
	/* Divide-by-zero guard — return the midpoint step so we boot
	 * with something sane while diagnostics figure out why VDD
	 * came through as 0.
	 */
	assert(lpcomp_choose_ref_step_16(1500, 0) == 8);
}

static void test_typical_battery_voltage_range(void)
{
	/* 2xAAA fresh = ~3.0 V, near end-of-life = ~1.8 V (device
	 * cutoff). Sanity-check step choices for both voltages with a
	 * 1000 mV target.
	 */
	uint8_t step_fresh = lpcomp_choose_ref_step_16(1000, 3000);
	uint8_t step_low = lpcomp_choose_ref_step_16(1000, 1800);

	/* At 1800 mV VDD, 1000/1800 = 8.89/16 → step 9. */
	assert(step_low == 9);
	/* Sanity: as VDD drops the step for the same absolute target rises. */
	assert(step_low > step_fresh);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("lpcomp_ref tests\n");
	RUN(test_midsupply_maps_to_step_8);
	RUN(test_rounds_to_nearest);
	RUN(test_low_target_clamps_to_step_1);
	RUN(test_high_target_clamps_to_step_15);
	RUN(test_zero_vdd_returns_safe_fallback);
	RUN(test_typical_battery_voltage_range);
	printf("all tests passed\n");
	return 0;
}

#include "battery_level.h"

#include <assert.h>
#include <stdio.h>

/* Defaults used throughout: 2x AAA Energizer Ultimate Lithium in series.
 * ~1.5 V nominal each -> 3000 mV full; ~1.0 V cutoff each -> 2000 mV empty.
 */
#define EMPTY 2000U
#define FULL  3000U

static void test_at_or_above_full_is_100(void)
{
	assert(battery_level_percent(FULL, EMPTY, FULL) == 100);
	assert(battery_level_percent(3300, EMPTY, FULL) == 100);
	/* A USB / bench-rail supply reads well above full. Must clamp, not
	 * wrap — the bench runs at ~3300 mV off the Pi 3V3 rail.
	 */
	assert(battery_level_percent(5000, EMPTY, FULL) == 100);
}

static void test_at_or_below_empty_is_0(void)
{
	assert(battery_level_percent(EMPTY, EMPTY, FULL) == 0);
	assert(battery_level_percent(1500, EMPTY, FULL) == 0);
	assert(battery_level_percent(0, EMPTY, FULL) == 0);
}

static void test_linear_midpoint(void)
{
	assert(battery_level_percent(2500, EMPTY, FULL) == 50);
	assert(battery_level_percent(2750, EMPTY, FULL) == 75);
	assert(battery_level_percent(2250, EMPTY, FULL) == 25);
}

static void test_rounds_to_nearest(void)
{
	/* 2605 mV over a 1000 mV span = 60.5 % -> 61, not 60. */
	assert(battery_level_percent(2605, EMPTY, FULL) == 61);
	/* 2604 mV = 60.4 % -> 60. */
	assert(battery_level_percent(2604, EMPTY, FULL) == 60);
}

static void test_degenerate_range_does_not_divide_by_zero(void)
{
	/* Misconfigured Kconfig (empty == full, or inverted). Must return a
	 * defined value rather than trapping — this runs on the report path
	 * and a fault here would take the device off the network.
	 */
	assert(battery_level_percent(2500, 3000, 3000) == 0);
	assert(battery_level_percent(2500, 3000, 2000) == 0);
	assert(battery_level_percent(3500, 3000, 3000) == 0);
}

static void test_zcl_percentage_is_half_percent_units(void)
{
	/* ZCL 0x0021 BatteryPercentageRemaining is in 0.5 % units, so the
	 * wire value is twice the percentage. Z2M divides by 2 again.
	 */
	assert(battery_level_zcl_percentage(0) == 0);
	assert(battery_level_zcl_percentage(50) == 100);
	assert(battery_level_zcl_percentage(100) == 200);
	/* 10 % is the example from the issue: exposes {"battery": 10}. */
	assert(battery_level_zcl_percentage(10) == 20);
}

static void test_zcl_percentage_clamps_over_100(void)
{
	/* Never emit > 200: 0xFF is reserved for "unknown" and values
	 * between 201 and 254 are undefined in the spec.
	 */
	assert(battery_level_zcl_percentage(101) == 200);
	assert(battery_level_zcl_percentage(255) == 200);
}

static void test_zcl_voltage_is_100mv_units(void)
{
	/* ZCL 0x0020 BatteryVoltage is in 100 mV units. Z2M multiplies by
	 * 100 to expose millivolts, so 2600 mV -> 26 -> {"voltage": 2600}.
	 */
	assert(battery_level_zcl_voltage(2600) == 26);
	assert(battery_level_zcl_voltage(3000) == 30);
	assert(battery_level_zcl_voltage(0) == 0);
}

static void test_zcl_voltage_rounds_to_nearest(void)
{
	assert(battery_level_zcl_voltage(2650) == 27);
	assert(battery_level_zcl_voltage(2649) == 26);
}

static void test_zcl_voltage_saturates_not_wraps(void)
{
	/* Above 25.4 V the u8 would wrap. Saturate one below the 0xFF
	 * "unknown" sentinel so a bad reading can't be mistaken for it.
	 */
	assert(battery_level_zcl_voltage(25500) == 254);
	assert(battery_level_zcl_voltage(65535) == 254);
}

static void test_unknown_sentinel(void)
{
	/* Used when the SAADC read fails, so Z2M shows nothing rather than
	 * a plausible-looking zero.
	 */
	assert(BATTERY_LEVEL_ZCL_UNKNOWN == 0xFF);
}

int main(void)
{
	test_at_or_above_full_is_100();
	test_at_or_below_empty_is_0();
	test_linear_midpoint();
	test_rounds_to_nearest();
	test_degenerate_range_does_not_divide_by_zero();
	test_zcl_percentage_is_half_percent_units();
	test_zcl_percentage_clamps_over_100();
	test_zcl_voltage_is_100mv_units();
	test_zcl_voltage_rounds_to_nearest();
	test_zcl_voltage_saturates_not_wraps();
	test_unknown_sentinel();

	printf("test_battery_level: all passed\n");
	return 0;
}

#include "battery_level.h"

uint8_t battery_level_percent(uint16_t mv, uint16_t empty_mv, uint16_t full_mv)
{
	if (full_mv <= empty_mv) {
		/* Misconfigured range. Report empty rather than trapping;
		 * see the header note on why this path must not fault.
		 */
		return 0U;
	}
	if (mv <= empty_mv) {
		return 0U;
	}
	if (mv >= full_mv) {
		return 100U;
	}

	uint32_t span = (uint32_t)full_mv - (uint32_t)empty_mv;
	uint32_t above = (uint32_t)mv - (uint32_t)empty_mv;

	/* Round to nearest: (above * 100 + span/2) / span. Max intermediate
	 * is 65535 * 100, comfortably inside u32.
	 */
	return (uint8_t)(((above * 100U) + (span / 2U)) / span);
}

uint8_t battery_level_zcl_percentage(uint8_t percent)
{
	if (percent >= 100U) {
		return 200U;
	}
	return (uint8_t)(percent * 2U);
}

uint8_t battery_level_zcl_voltage(uint16_t mv)
{
	uint32_t units = ((uint32_t)mv + 50U) / 100U;  /* round to nearest */

	if (units > 254U) {
		return 254U;
	}
	return (uint8_t)units;
}

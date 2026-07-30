#ifndef BATTERY_LEVEL_H
#define BATTERY_LEVEL_H 1

#include <stdint.h>

/*
 * Battery state-of-charge mapping and ZCL Power Configuration (0x0001)
 * wire encodings — issue #8.
 *
 * Pure integer logic, no Zephyr dependency, so it is host-tested in
 * tests/test_battery_level.c. The SAADC read that feeds it lives in
 * zigbee_app.c, which is the part that can only be verified on-bench.
 *
 * Why linear: 2x AAA lithium (Energizer Ultimate L92) holds a famously
 * flat discharge curve, so a voltage-to-percentage map is approximate at
 * best whatever shape you choose. A linear map between two Kconfig-tunable
 * endpoints is honest about that and is trivially re-tunable from bench
 * data without a table rebuild. Do not read the percentage as accurate
 * mid-life; it is a "replace soon" indicator.
 */

/* ZCL "unknown / invalid" for both BatteryVoltage (0x0020) and
 * BatteryPercentageRemaining (0x0021). Reported when the SAADC read
 * fails, so Z2M shows nothing rather than a plausible-looking zero.
 */
#define BATTERY_LEVEL_ZCL_UNKNOWN 0xFFU

/*
 * Linear map from millivolts to 0-100 %, clamped at both ends and
 * rounded to nearest. Returns 0 for a degenerate range (empty >= full)
 * rather than dividing by zero — this runs on the 5-minute report path
 * and a fault here would drop the device off the network.
 */
uint8_t battery_level_percent(uint16_t mv, uint16_t empty_mv, uint16_t full_mv);

/*
 * ZCL BatteryPercentageRemaining (0x0021): units of 0.5 %, so the wire
 * value is 0-200 and Z2M halves it again. Clamped to 200 — 0xFF is the
 * "unknown" sentinel and 201-254 are undefined.
 */
uint8_t battery_level_zcl_percentage(uint8_t percent);

/*
 * ZCL BatteryVoltage (0x0020): units of 100 mV, rounded to nearest.
 * Saturates at 254 so an out-of-range reading can never be mistaken for
 * the 0xFF "unknown" sentinel.
 */
uint8_t battery_level_zcl_voltage(uint16_t mv);

#endif /* BATTERY_LEVEL_H */

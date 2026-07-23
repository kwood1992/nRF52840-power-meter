#ifndef METERING_SCALE_H
#define METERING_SCALE_H

#include <stdint.h>

/*
 * Pure-logic helper for the Metering cluster's CurrentSummationDelivered
 * attribute (ZCL cluster 0x0702, attr 0x0000).
 *
 * ZCL stores CurrentSummationDelivered as a 48-bit unsigned integer.
 * Multiplier / Divisor are cluster-level metadata that the *reader*
 * (Z2M / HA) applies to derive kWh, so on device we just place the
 * raw pulse count into the u48 wire slot. With Multiplier=1 and
 * Divisor=1000 (design-doc defaults) a 1000 imp/kWh meter reads as
 * kWh = pulse_count / 1000 in Z2M with no arithmetic on device.
 *
 * Split from ZBOSS's zb_uint48_t so this stays host-testable —
 * `zb_uint48_t` is a packed struct only reachable when the ZBOSS
 * headers are on the include path, which they aren't for host tests.
 * On nRF (little-endian) our layout matches ZBOSS's exactly and the
 * caller copies field-for-field.
 */

/* Max value a Zigbee ZCL u48 attribute can carry: 2^48 - 1. */
#define METERING_SUMMATION_U48_MAX 0xFFFFFFFFFFFFULL

struct metering_u48 {
	uint32_t low;
	uint16_t high;
};

/*
 * Format `pulse_total` into a metering_u48. Values above 2^48-1 are
 * clamped to METERING_SUMMATION_U48_MAX. A saturating counter is much
 * less scary than a wrap to zero — Z2M would interpret a wrap as
 * "meter reset" and hard-reset the HA Energy Dashboard's baseline.
 * A saturated device is at least visibly stuck, not silently wrong.
 */
struct metering_u48 metering_scale_to_u48(uint64_t pulse_total);

#endif

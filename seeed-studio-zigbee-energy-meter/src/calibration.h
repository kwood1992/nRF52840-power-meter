#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure validation for the runtime-configurable imp/kWh calibration
 * (issue #48). The Metering cluster's Divisor attribute IS the imp/kWh
 * value directly (Multiplier is pinned to 1), so a Divisor write from
 * Z2M gets validated through here before it lands in NVS.
 *
 * Range picked defensively: 100 imp/kWh at the low end (below that, a
 * single pulse represents >0.01 kWh — coarse enough to make the
 * accumulator jumpy in Z2M), 10000 at the high end (well above any
 * residential meter I've seen documented; the u24 wire ceiling of
 * 16777215 sits far above this and isn't the tightening constraint).
 * Anything outside is a typo; reject rather than silently clamp so a
 * Z2M attribute write returns INVALID_VALUE and the user sees the
 * error rather than getting the wrong scale.
 */
#define CALIBRATION_IMP_PER_KWH_MIN 100U
#define CALIBRATION_IMP_PER_KWH_MAX 10000U

bool calibration_is_valid_imp_per_kwh(uint32_t imp_per_kwh);

/*
 * Validation for the min-pulse-width filter threshold (issue #59). Range
 * mirrors CONFIG_APP_PULSE_MIN_WIDTH_US:
 *
 * - 100 µs floor: below LPCOMP settling + hysteresis roundtrip; anything
 *   shorter is measurement noise.
 * - 10 000 µs ceiling: well below the shortest real electricity-meter
 *   LED (imp pulses are 20-100 ms), so any legal filter can't miss real
 *   pulses.
 *
 * Same reject-rather-than-clamp policy as imp/kWh — out-of-range writes
 * return INVALID_VALUE to Z2M so the installer sees the error.
 */
#define CALIBRATION_PULSE_MIN_WIDTH_US_MIN 100U
#define CALIBRATION_PULSE_MIN_WIDTH_US_MAX 10000U

bool calibration_is_valid_pulse_min_width_us(uint32_t min_width_us);

#endif

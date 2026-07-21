#ifndef PERSIST_POLICY_H
#define PERSIST_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Persistence-policy decision helper.
 *
 * Decouples the "when should we write the accumulator total to NVS?"
 * question from Zephyr's NVS API so it can be unit-tested on the host.
 *
 * Policy chosen (see docs/working/ notes for rationale):
 *
 *  - Wall-clock cadence: write every N ms (matches the eventual 5-min
 *    Zigbee report rhythm — one wake-write per report keeps flash wear
 *    to ~O(reports), not O(pulses)).
 *  - Pulse-delta safety net: if we've counted more than N pulses since
 *    the last save, write early. Bounds worst-case data loss on
 *    unexpected reset when pulses come in bursts (bench button-mashing,
 *    fault light going haywire, etc.).
 *  - Never write if the total hasn't changed. NVS reuses sectors, but a
 *    no-op write still consumes cycles and log noise.
 */
struct persist_policy {
	uint64_t interval_ms;
	uint64_t max_pulse_delta;
};

void persist_policy_init(struct persist_policy *p,
			 uint64_t interval_ms,
			 uint64_t max_pulse_delta);

/*
 * Returns true if the caller should persist `current_total` now.
 *
 *   current_total       — the accumulator's live cumulative count
 *   last_saved_total    — the last value we successfully persisted
 *   ms_since_last_write — wall-clock elapsed since the last persist
 */
bool persist_policy_should_write(const struct persist_policy *p,
				 uint64_t current_total,
				 uint64_t last_saved_total,
				 uint64_t ms_since_last_write);

#endif

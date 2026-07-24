#ifndef REPORT_GATE_H
#define REPORT_GATE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Every-N-th-call gate for the "force an explicit ZCL Report Attributes
 * frame" fallback on the metering CurrentSummationDelivered publish path.
 *
 * Motivation is issue #20: the ZBOSS reporting engine's delta trigger
 * (reportable_change from Z2M's ConfigureReporting) can silently stop
 * emitting after a factory-reset cycle even though our attribute writes
 * still land. Belt-and-braces: on every Nth publish, bypass the delta
 * gate and send an explicit report frame from the caller. Trades a tiny
 * amount of radio bandwidth for reliability; the Nth call also serves
 * as a heartbeat so a permanent silent-reporting failure surfaces
 * within N pulses instead of at the 65000 s max_interval force-fire.
 *
 * Pure logic — no ZBOSS deps — so it's host-testable. Advance() runs on
 * the sample loop, so it must be trivially cheap.
 */
struct report_gate {
	uint32_t period;   /* fire every period-th advance() call; 0 disables */
	uint32_t counter;  /* rolls up until it hits period, then resets */
};

/*
 * Initialise the gate to fire every `period` calls to advance().
 *   period == 0 disables the gate (advance() always returns false).
 *   period == 1 fires on every call.
 */
void report_gate_init(struct report_gate *g, uint32_t period);

/*
 * Bump the counter and return true iff this is the Nth call since the
 * last fire (or since init). Resets to 0 on fire so successive fires
 * are exactly `period` calls apart.
 */
bool report_gate_advance(struct report_gate *g);

#endif

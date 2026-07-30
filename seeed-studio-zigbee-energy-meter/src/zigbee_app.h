#ifndef ZIGBEE_APP_H
#define ZIGBEE_APP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Application-facing wrapper over the ncs-zigbee R23 stack. Hides
 * ZBOSS API details from main.c so the button/commissioning flow and
 * the metering-attribute publish path can evolve without dragging
 * ZBOSS internals through the sample loop.
 */

/*
 * Bring up the ZBOSS stack as a Sleepy End Device (ZB_ZED), register
 * the app endpoint with Basic + Identify + Metering (0x0702) clusters,
 * populate metering attributes with the design-doc defaults
 * (Multiplier=1, Divisor=1000, UnitOfMeasure=kWh, Status=0), and
 * start the ZBOSS thread. Returns 0 on success.
 *
 * If the device was previously joined, ZBOSS restores that state from
 * NVS on its own and rejoins on the next parent poll — no button press
 * required.
 */
int zigbee_app_init(void);

/*
 * Kick off BDB network-steering. Called from the button IRQ handler
 * on a short-press. Safe to call when already joined (BDB no-ops).
 */
void zigbee_app_start_join(void);

/*
 * Leave the network and wipe ZBOSS's NVS state, so the next boot comes
 * up unjoined and looking for a coordinator. Called from the button
 * IRQ handler on a long-press.
 */
void zigbee_app_factory_reset(void);

/*
 * True iff the device is currently associated with a coordinator.
 */
bool zigbee_app_is_joined(void);

/*
 * Re-open a turbo-poll window (CONFIG_APP_ZIGBEE_WAKE_TURBO_POLL_MS)
 * without touching join state. Called from the button dispatch thread on
 * a short-press while already joined — see issue #62.
 *
 * The point is downlink latency: in steady state the sleepy ED polls its
 * parent every CONFIG_APP_ZIGBEE_LONG_POLL_INTERVAL_MS (60 s), which is
 * far past Z2M's ~10 s ZCL write deadline, so writes to imp_per_kwh /
 * min_pulse_width_us time out. Turbo poll drops the cadence to ~100 ms
 * for the window, then ZBOSS reverts to the long poll on its own.
 *
 * No-op when CONFIG_APP_ZIGBEE_SLEEPY_ED is off (the radio is already
 * always live) or when not joined (there is no parent to poll — ZBOSS's
 * PIM would have nowhere to send).
 */
void zigbee_app_wake_for_write(void);

/*
 * Update the Metering cluster's `CurrentSummationDelivered` attribute
 * (0x0702 / 0x0000) with the current pulse-accumulator total.
 *
 * ZBOSS marks the attribute for reporting on write, so once the
 * coordinator (Z2M) has configured reporting on the attribute the
 * next report the ZBOSS reporting engine sends will carry this
 * value. Attribute reads (Z2M's manual "read attribute" query)
 * always return the last-written value.
 *
 * The write happens from the ZBOSS thread via ZB_SCHEDULE_APP_CALLBACK
 * — safe to call from any Zephyr thread, including a k_work handler
 * or the ADC sample loop. No-op if the endpoint hasn't been
 * registered yet (i.e. `zigbee_app_init` failed).
 */
void zigbee_app_publish_summation(uint64_t pulse_total);

/*
 * Same as `zigbee_app_publish_summation`, but ALSO explicitly emits a
 * ZCL Report Attributes frame after the write — bypassing the reporting
 * engine's delta-threshold gate. Use this on the 5-minute periodic
 * tick and on the per-N-pulses heartbeat where we want the coordinator
 * to see the value even if the delta since last report is below the
 * ConfigureReporting `reportable_change` value.
 *
 * The forced-report is a no-op (with a WRN log) if the coordinator
 * hasn't sent ConfigureReporting yet — there's no reporting slot to
 * populate the frame's destination from. The attribute write itself
 * still happens either way.
 *
 * Context: issue #20. The ZBOSS reporting engine can silently stop
 * emitting reports after a factory-reset + rejoin cycle even when
 * writes still land in the attribute table; explicit report frames
 * side-step whatever state got out of sync.
 */
void zigbee_app_publish_summation_and_report(uint64_t pulse_total);

#endif

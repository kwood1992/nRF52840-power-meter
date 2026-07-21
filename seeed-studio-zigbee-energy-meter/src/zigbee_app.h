#ifndef ZIGBEE_APP_H
#define ZIGBEE_APP_H

#include <stdbool.h>

/*
 * Application-facing wrapper over the ncs-zigbee R23 stack. Hides
 * ZBOSS API details from main.c so the button/commissioning flow can
 * evolve without dragging ZBOSS internals through the sample loop.
 *
 * NOT yet fully wired to the ZBOSS API — the button-side flow and
 * this interface are locked in, but the implementations that call
 * `bdb_start_top_level_commissioning()` and friends need to be
 * fleshed out against ncs-zigbee 1.3.0 headers (which land via
 * `west.yml` in issue #3). Search this file and zigbee_app.c for
 * "TODO(ncs-zigbee-api)" — those are the exact spots that need
 * header-verified calls before this feature is trusted.
 */

/*
 * Bring up the ZBOSS stack as a Sleepy End Device (ZB_ZED),
 * register the app endpoint with Basic + Identify clusters, and
 * start the ZBOSS thread. Returns 0 on success, negative errno
 * otherwise.
 *
 * If the device was previously joined, ZBOSS restores that state
 * from NVS on its own and rejoins on the next parent poll — no
 * button press required.
 */
int zigbee_app_init(void);

/*
 * Kick off BDB network-steering. Called from the button IRQ handler
 * on a short-press. Safe to call when already joined (BDB no-ops).
 */
void zigbee_app_start_join(void);

/*
 * Leave the network and wipe ZBOSS's NVS state, so the next boot
 * comes up unjoined and looking for a coordinator. Called from the
 * button IRQ handler on a long-press.
 */
void zigbee_app_factory_reset(void);

/*
 * True iff the device is currently associated with a coordinator.
 * Used by main.c only for logging today; #5 will grow this into a
 * gate for attribute reporting.
 */
bool zigbee_app_is_joined(void);

#endif

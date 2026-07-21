#include "zigbee_app.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zb_mem_config_med.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>

LOG_MODULE_REGISTER(zigbee_app, LOG_LEVEL_INF);

/*
 * Endpoint 10 chosen arbitrarily but conventionally — Nordic's own
 * Metering samples land clusters on endpoint 10 too, which makes
 * Z2M's default cluster discovery just work. Move if we need to
 * host multiple endpoints later.
 */
#define APP_ENDPOINT 10

/*
 * TODO(ncs-zigbee-api): fill in the ZBOSS cluster declarations for
 *   Basic (0x0000) + Identify (0x0003). ncs-zigbee provides
 *   ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST / ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST
 *   and the ZB_ZCL_DECLARE_..._CLUSTER_LIST family that stitches
 *   them together. Grab the exact macro names + Basic-attribute
 *   list (manufacturer, model, power source, ...) from
 *   `samples/light_switch/light_bulb` or similar in the R23 tree.
 *
 * TODO(ncs-zigbee-api): declare the device context with
 *   ZBOSS_DECLARE_DEVICE_CTX_1_EP(app_ctx, app_ep). This is the
 *   handle passed to ZB_AF_REGISTER_DEVICE_CTX() from
 *   zigbee_app_init().
 */

static bool joined;

/*
 * ZBOSS calls this from its main loop on every stack event. Keeping
 * it minimal — hand off to ncs-zigbee's default handler for the
 * bookkeeping (persistent-storage init, sleepy-device wake handling,
 * ...) and only override the signals we care about.
 */
void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t *sg_p;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sg_p);
	zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_FIRST_START:
	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
		/* First cold boot: stay quiet — user presses the button
		 * to trigger steering. Reboot after a successful join:
		 * ZBOSS will rejoin the same network on its own; we just
		 * flag `joined` when that lands via ZB_BDB_SIGNAL_STEERING.
		 */
		break;
	case ZB_BDB_SIGNAL_STEERING:
		joined = (status == RET_OK);
		LOG_INF("network steering result: %s",
			joined ? "joined" : "failed");
		break;
	default:
		break;
	}

	/* Let ncs-zigbee's helper do the standard bookkeeping for
	 * whatever we didn't handle above. This also frees `bufid`.
	 */
	zigbee_default_signal_handler(bufid);
}

int zigbee_app_init(void)
{
	/* TODO(ncs-zigbee-api): register the device context declared above.
	 *
	 *   ZB_AF_REGISTER_DEVICE_CTX(&app_ctx);
	 *
	 * Also register the endpoint's Identify callback so ncs-zigbee
	 * can drive the red LED for identify effects:
	 *
	 *   ZB_ZCL_REGISTER_DEVICE_CB(identify_cb);
	 *
	 * The Identify callback signature and where it hooks the LED
	 * lives in ncs-zigbee's identify example — copy that verbatim.
	 */

	/* Sleepy End Device role. ZB_ZED is the compile-time role set
	 * via CONFIG_ZIGBEE_ROLE_END_DEVICE in prj.conf; at runtime the
	 * only thing left to do here is confirm the ZBOSS stack picks it
	 * up correctly (checked via log during first join).
	 */
	zigbee_configure_sleepy_behavior(true);

	int rc = zigbee_enable();

	if (rc) {
		LOG_ERR("zigbee_enable failed: %d", rc);
		return rc;
	}
	return 0;
}

void zigbee_app_start_join(void)
{
	/* Idempotent — if already joined or already steering, BDB
	 * short-circuits.
	 */
	LOG_INF("starting network steering");
	bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
}

void zigbee_app_factory_reset(void)
{
	LOG_WRN("factory reset — leaving network and erasing NVS");
	joined = false;
	zb_bdb_reset_via_local_action(0);
}

bool zigbee_app_is_joined(void)
{
	return joined;
}

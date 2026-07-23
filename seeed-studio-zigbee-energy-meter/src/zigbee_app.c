#include "zigbee_app.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zb_mem_config_med.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>
#include <zb_nrf_platform.h>

#include "zb_range_extender.h"

LOG_MODULE_REGISTER(zigbee_app, LOG_LEVEL_INF);

/*
 * Endpoint 10 chosen arbitrarily but conventionally — Nordic's own
 * Metering samples land clusters on endpoint 10 too, which makes
 * Z2M's default cluster discovery just work. Move if we need to
 * host multiple endpoints later.
 */
#define APP_ENDPOINT 10

/*
 * Minimal-viable Zigbee endpoint: Basic (0x0000) + Identify (0x0003)
 * as servers. Uses the "Range Extender" device type (0x0008) from
 * ncs-zigbee's template because it's the smallest HA profile that
 * exposes exactly those two clusters and nothing else. The actual
 * Metering cluster (0x0702) that reports kWh gets added in issue #5;
 * this baseline is what's needed for the device to be commissionable
 * at all — without a registered device context, ZBOSS asserts inside
 * bdb_start_top_level_commissioning().
 */

struct zb_device_ctx {
	zb_zcl_basic_attrs_t basic_attr;
	zb_zcl_identify_attrs_t identify_attr;
};

static struct zb_device_ctx dev_ctx;

ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	identify_attr_list,
	&dev_ctx.identify_attr.identify_time);

ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST(
	basic_attr_list,
	&dev_ctx.basic_attr.zcl_version,
	&dev_ctx.basic_attr.power_source);

ZB_DECLARE_RANGE_EXTENDER_CLUSTER_LIST(
	app_clusters,
	basic_attr_list,
	identify_attr_list);

ZB_DECLARE_RANGE_EXTENDER_EP(
	app_ep,
	APP_ENDPOINT,
	app_clusters);

ZBOSS_DECLARE_DEVICE_CTX_1_EP(
	app_ctx,
	app_ep);

static bool joined;

/*
 * ZBOSS calls this from its main loop on every stack event.
 *
 * Design intent: sleepy end device with user-driven commissioning.
 * On first boot we do NOT auto-steer — the battery-life target
 * demands the CPU sleep between explicit user actions (button
 * press → join, or wake-timer → send-report-then-sleep once
 * joined). ncs-zigbee's default handler auto-starts steering on
 * FIRST_START and auto-retries on STEERING failure, both of which
 * defeat the battery model, so we override those signals and free
 * the buffer ourselves. For everything else (join success, reboot
 * of an already-commissioned device, LEAVE, etc.) we delegate to
 * the default handler for the standard bookkeeping.
 */
void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t *sg_p;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sg_p);
	zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_FIRST_START:
		/* Cold boot, never joined a network. Do nothing —
		 * ZBOSS will sleep between polls. Short-press the
		 * button to steer.
		 */
		LOG_INF("first boot — press button to join a network");
		joined = false;
		zb_buf_free(bufid);
		return;

	case ZB_BDB_SIGNAL_STEERING:
		joined = (status == RET_OK);
		LOG_INF("network steering: %s (status=%d)",
			joined ? "joined" : "failed", status);
		if (!joined) {
			/* Do NOT auto-retry — default handler would
			 * kick off a rejoin loop that keeps the radio
			 * awake and eventually resets ZBOSS. Sit
			 * quietly until the user presses the button
			 * again.
			 */
			zb_buf_free(bufid);
			return;
		}
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		/* Kicked from network (or factory-reset propagated).
		 * Mark unjoined and let the default handler tidy up.
		 */
		LOG_WRN("left network — waiting for button to re-join");
		joined = false;
		break;

	default:
		break;
	}

	/* Everything we haven't short-circuited above (REBOOT of an
	 * already-commissioned device, STEERING success, LEAVE, ...)
	 * goes through ncs-zigbee's default handler, which does the
	 * standard NVRAM/network-key bookkeeping and frees the buffer.
	 */
	zigbee_default_signal_handler(bufid);
}

/*
 * Identify callback. ncs-zigbee registers this via
 * ZB_AF_SET_IDENTIFY_NOTIFICATION_HANDLER so that the ZBOSS stack has
 * somewhere to call when a coordinator invokes Identify on our
 * endpoint (typical Z2M pairing flow). For now this is a stub — later
 * we'll drive the onboard LED for the requested identify duration.
 * Without a registered handler, ZBOSS asserts in
 * zb_zdo_dev_start_cont during commissioning (verified via SWD
 * breakpoint at zb_assert on 2026-07-22).
 */
static void identify_cb(zb_bufid_t bufid)
{
	if (bufid) {
		LOG_INF("identify request received (bufid=%u)", bufid);
	} else {
		LOG_INF("identify cancelled");
	}
}

static void app_clusters_attr_init(void)
{
	/* Basic cluster (0x0000). ZCL_VERSION is a required attribute
	 * declaring which revision of the Zigbee cluster library we
	 * implement — the R23 stack sets this from the header.
	 * Power source is battery — for a sleepy end device on AAA,
	 * this drives Z2M's expected reporting behavior (poll vs.
	 * report cadence). Bench-USB is still declared as battery
	 * because that's the target power model.
	 */
	dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;

	/* Identify cluster (0x0003). Default identify time = 0 =
	 * device is NOT currently identifying. Coordinators write
	 * this to N seconds to make the device blink for N seconds.
	 */
	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;
}

int zigbee_app_init(void)
{
	/* Register the endpoint context BEFORE zigbee_enable(). Without
	 * this, ZBOSS has no clusters to advertise during commissioning
	 * and bdb_start_top_level_commissioning() asserts inside the
	 * ZBOSS thread — we saw this as a `<err> zboss_osif: ZBOSS ...`
	 * printed once + immediate SoC reset, on the very first button-
	 * triggered join.
	 */
	ZB_AF_REGISTER_DEVICE_CTX(&app_ctx);
	app_clusters_attr_init();

	/* Register identify handler — MUST come after ctx registration
	 * and BEFORE zigbee_enable(). See identify_cb() comment for why.
	 */
	ZB_AF_SET_IDENTIFY_NOTIFICATION_HANDLER(APP_ENDPOINT, identify_cb);

	/* Sleepy End Device role. ZB_ZED is the compile-time role set
	 * via CONFIG_ZIGBEE_ROLE_END_DEVICE in prj.conf; at runtime the
	 * only thing left to do here is confirm the ZBOSS stack picks it
	 * up correctly (checked via log during first join).
	 *
	 * *** TEMPORARILY DISABLED for interview debugging ***
	 * With sleepy=true, Z2M reports "failed to interview because
	 * can not get node descriptor" — parent-poll latency exceeds
	 * Z2M's ZDO timeout. Running as an always-awake ED for bench
	 * verification; will re-enable + tune poll intervals once
	 * interview works and we can validate incremental changes.
	 * Non-sleepy ED still draws far less than a Router — the CPU
	 * just doesn't drop to SYSTEM_ON deep sleep between events.
	 */
	// zigbee_configure_sleepy_behavior(true);

	/* ncs-zigbee's zigbee_enable() returns void — it spawns the ZBOSS
	 * thread and any hard failures are surfaced via zboss_signal_handler
	 * rather than a return code. Nothing to check inline.
	 */
	zigbee_enable();
	return 0;
}

/* Callbacks run inside ZBOSS's own thread. Calling
 * bdb_start_top_level_commissioning() from any other thread doesn't
 * work reliably: the ZBOSS scheduler is asleep most of the time under
 * sleepy-ED, and a direct call from an app thread queues the request
 * but doesn't wake the scheduler. `ZB_SCHEDULE_APP_CALLBACK` does
 * both — it schedules the callback on the ZBOSS thread AND signals
 * it to run, which pulls ZBOSS out of sleep.
 */
static void start_steering_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);
	LOG_INF("ZBOSS thread: invoking bdb_start_top_level_commissioning");
	zb_bool_t rc = bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);

	LOG_INF("ZBOSS thread: bdb_start returned %s",
		rc == ZB_TRUE ? "TRUE (scheduled)" : "FALSE (rejected)");
}

static void factory_reset_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);
	LOG_WRN("ZBOSS thread: invoking zb_bdb_reset_via_local_action");
	joined = false;
	zb_bdb_reset_via_local_action(0);
}

void zigbee_app_start_join(void)
{
	/* Idempotent — if already joined or already steering, BDB
	 * short-circuits inside the callback.
	 */
	LOG_INF("starting network steering");
	ZB_SCHEDULE_APP_CALLBACK(start_steering_cb, 0);
}

void zigbee_app_factory_reset(void)
{
	LOG_WRN("factory reset — leaving network and erasing NVS");
	ZB_SCHEDULE_APP_CALLBACK(factory_reset_cb, 0);
}

bool zigbee_app_is_joined(void)
{
	return joined;
}

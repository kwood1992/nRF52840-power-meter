#include "zigbee_app.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zb_mem_config_med.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>
#include <zb_nrf_platform.h>

#include "led_controller.h"
#include "metering_scale.h"
#include "zb_meter_ep.h"

LOG_MODULE_REGISTER(zigbee_app, LOG_LEVEL_INF);

/*
 * Endpoint 10 — same slot the ncs-zigbee samples land clusters on,
 * which makes Z2M's default cluster discovery just work. If we ever
 * grow a second endpoint (e.g. a Power Configuration cluster on its
 * own EP), pick 11 for it and keep this one metering-only.
 */
#define APP_ENDPOINT 10

/*
 * Design-doc defaults for the Metering cluster (see the "Zigbee model"
 * row): Multiplier=1, Divisor=1000, UnitOfMeasure=0 (kWh),
 * SummationFormatting=0 (no additional formatting hints — Z2M knows to
 * apply Divisor). MeteringDeviceType=0 = "Electric Metering".
 *
 * The pulse-count-to-kWh math lives in Z2M: kWh = raw_summation ×
 * Multiplier ÷ Divisor. On a 1000 imp/kWh meter the raw summation IS
 * the pulse count, and Divisor=1000 gives Z2M kWh directly. If a field
 * meter turns out to be, say, 800 imp/kWh, the fix is Divisor=800 —
 * changeable at runtime via a Z2M attribute write, no reflash needed.
 */
#define METERING_MULTIPLIER      1U
#define METERING_DIVISOR         1000U
#define METERING_UNIT_KWH        ZB_ZCL_METERING_UNIT_OF_MEASURE_DEFAULT_VALUE  /* 0 = kWh */
#define METERING_SUMM_FORMATTING 0U
#define METERING_DEVICE_TYPE     0U  /* 0 = Electric Metering per SE 1.4 D.5.2.2.5.2 */

/*
 * Basic-cluster identity strings. These are the fingerprint Z2M's
 * external converter matches on (`zigbeeModel` + manufacturer). Kept
 * short and stable — changing them after commissioning would strand
 * paired coordinators. `SW_BUILD_ID` is a free-form version string
 * — updated by the CI when we start tagging releases; today it
 * mirrors the branch state.
 */
#define BASIC_APP_VERSION      0x01
#define BASIC_STACK_VERSION    0x03  /* ZBOSS R23 */
#define BASIC_HW_VERSION       0x01
#define BASIC_MANUF_NAME       "kwood1992"
#define BASIC_MODEL_ID         "xiao-power-meter"
#define BASIC_DATE_CODE        "20260723"  /* commissioning start — see #5 */
#define BASIC_LOCATION_DESC    ""
#define BASIC_SW_BUILD_ID      "0.5.0"

struct zb_device_ctx {
	zb_zcl_basic_attrs_ext_t basic_attr;
	zb_zcl_identify_attrs_t identify_attr;

	/* Metering (0x0702) attribute storage. Manually broken out
	 * because we want the EXT variant of the ZBOSS attribute-list
	 * macro (adds Multiplier/Divisor) and its bundled struct
	 * `zb_zcl_metering_attrs_t` doesn't include those fields.
	 */
	zb_uint48_t metering_current_summation;
	zb_uint8_t  metering_status;
	zb_uint8_t  metering_unit_of_measure;
	zb_uint8_t  metering_summation_formatting;
	zb_uint8_t  metering_device_type;
	zb_int24_t  metering_instantaneous_demand;
	zb_uint8_t  metering_demand_formatting;
	zb_uint8_t  metering_historical_consumption_formatting;
	zb_uint24_t metering_multiplier;
	zb_uint24_t metering_divisor;
};

static struct zb_device_ctx dev_ctx;

ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	identify_attr_list,
	&dev_ctx.identify_attr.identify_time);

/* EXT variant adds manufacturer name, model ID, hardware/app/stack
 * version, date code, location description, physical environment and
 * software build ID — Z2M's fingerprint match keys off manufacturer
 * name + model ID. Without them the auto-generated definition has
 * `zigbeeModel: ['']` and no external converter can attach.
 */
ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST_EXT(
	basic_attr_list,
	&dev_ctx.basic_attr.zcl_version,
	&dev_ctx.basic_attr.app_version,
	&dev_ctx.basic_attr.stack_version,
	&dev_ctx.basic_attr.hw_version,
	dev_ctx.basic_attr.mf_name,
	dev_ctx.basic_attr.model_id,
	dev_ctx.basic_attr.date_code,
	&dev_ctx.basic_attr.power_source,
	dev_ctx.basic_attr.location_id,
	&dev_ctx.basic_attr.ph_env,
	dev_ctx.basic_attr.sw_ver);

/* EXT variant so Multiplier/Divisor land in the attribute table
 * (the plain ATTRIB_LIST omits them, which would leave Z2M unable
 * to read the divisor and stuck at raw pulse-count units).
 */
ZB_ZCL_DECLARE_METERING_ATTRIB_LIST_EXT(
	metering_attr_list,
	&dev_ctx.metering_current_summation,
	&dev_ctx.metering_status,
	&dev_ctx.metering_unit_of_measure,
	&dev_ctx.metering_summation_formatting,
	&dev_ctx.metering_device_type,
	&dev_ctx.metering_instantaneous_demand,
	&dev_ctx.metering_demand_formatting,
	&dev_ctx.metering_historical_consumption_formatting,
	&dev_ctx.metering_multiplier,
	&dev_ctx.metering_divisor);

ZB_DECLARE_METER_CLUSTER_LIST(
	app_clusters,
	basic_attr_list,
	identify_attr_list,
	metering_attr_list);

ZB_DECLARE_METER_EP(
	app_ep,
	APP_ENDPOINT,
	app_clusters);

ZBOSS_DECLARE_DEVICE_CTX_1_EP(
	app_ctx,
	app_ep);

static bool joined;
static bool endpoint_registered;

/* Set by zigbee_app_start_join() (Zephyr thread context) and cleared by
 * the ZBOSS signal handler on ZB_BDB_SIGNAL_STEERING (ZBOSS thread
 * context) — an atomic keeps the two-thread hand-off honest. Only
 * user-initiated joins get LED feedback; stack-internal auto-rejoin
 * after ZB_ZDO_SIGNAL_LEAVE stays silent so a coordinator restart
 * in the middle of the night doesn't blink a mounted meter.
 */
static atomic_t user_join_in_flight = ATOMIC_INIT(0);

/*
 * ZBOSS calls this from its main loop on every stack event.
 *
 * ALWAYS delegate to `zigbee_default_signal_handler` regardless of
 * signal type, then free the buffer. See memory
 * `project_r23_signal_handler_must_delegate.md` for why — skipping
 * the default handler on FIRST_START breaks Z2M's ZDO interview
 * silently. The switch below is just for logging + local `joined`
 * state tracking; no early returns.
 */
void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t *sg_p;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sg_p);
	zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(bufid);

	switch (sig) {
	case ZB_BDB_SIGNAL_STEERING:
		joined = (status == RET_OK);
		LOG_INF("network steering: %s (status=%d)",
			joined ? "joined" : "failed", status);

		/* Only paint the outcome if a user-initiated join was in
		 * flight — auto-rejoin after ZB_ZDO_SIGNAL_LEAVE fires
		 * the same signal and must stay silent (issue #29).
		 */
		if (atomic_cas(&user_join_in_flight, 1, 0)) {
			led_cancel(LED_PATTERN_JOINING);
			if (status == RET_OK) {
				led_request(LED_PATTERN_JOIN_SUCCESS,
					    LED_PRIO_JOIN_SUCCESS);
			} else {
				led_request(LED_PATTERN_JOIN_FAIL,
					    LED_PRIO_JOIN_FAIL);
			}
		}
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		LOG_WRN("left network");
		joined = false;
		break;

	default:
		break;
	}

	ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));
	if (bufid) {
		zb_buf_free(bufid);
	}
}

static void metering_attrs_init(void)
{
	/* CurrentSummationDelivered — starts at 0; replaced by whatever
	 * NVS persisted before we finish booting, then live-updated as
	 * pulses come in.
	 */
	ZB_ASSIGN_UINT48(dev_ctx.metering_current_summation, 0, 0);

	dev_ctx.metering_status = 0;                 /* no fault flags */
	dev_ctx.metering_unit_of_measure = METERING_UNIT_KWH;
	dev_ctx.metering_summation_formatting = METERING_SUMM_FORMATTING;
	dev_ctx.metering_device_type = METERING_DEVICE_TYPE;

	/* Instantaneous demand — the design doc doesn't publish a live
	 * W/kW figure (HA derives it via the derivative helper). Leave
	 * zero. Demand/historical formatting fields are ignored by Z2M
	 * when the demand attribute isn't reporting, so 0 is safe.
	 * ZB_INT24_FROM_INT32 clamps in-place and needs an l-value.
	 */
	{
		zb_int32_t demand_init = 0;

		ZB_INT24_FROM_INT32(dev_ctx.metering_instantaneous_demand,
				    demand_init);
	}
	dev_ctx.metering_demand_formatting = 0;
	dev_ctx.metering_historical_consumption_formatting = 0;

	/* Multiplier / Divisor. Both are u24 in the wire representation
	 * (packed structs on nRF, low+high halves). Use the ZBOSS
	 * helper that converts a u32 into the packed u24 — writing the
	 * halves by hand is easy to get wrong and the helper is free.
	 */
	zb_uint32_to_uint24(METERING_MULTIPLIER, &dev_ctx.metering_multiplier);
	zb_uint32_to_uint24(METERING_DIVISOR, &dev_ctx.metering_divisor);
}

static void app_clusters_attr_init(void)
{
	/* Basic cluster (0x0000). ZCL_VERSION populated from header;
	 * power source declared as battery — a sleepy ED on AAA
	 * signals its polling model that way, which changes Z2M's
	 * expected report cadence (poll- rather than push-heavy).
	 * Bench-USB power is still declared as battery because that's
	 * the target power model.
	 */
	dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.app_version = BASIC_APP_VERSION;
	dev_ctx.basic_attr.stack_version = BASIC_STACK_VERSION;
	dev_ctx.basic_attr.hw_version = BASIC_HW_VERSION;
	dev_ctx.basic_attr.power_source = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
	dev_ctx.basic_attr.ph_env = ZB_ZCL_BASIC_ENV_UNSPECIFIED;

	/* ZCL strings are length-prefixed (Pascal-style): first byte is
	 * the length excluding the terminator. `ZB_ZCL_SET_STRING_VAL`
	 * writes both the prefix and the payload; `ZB_ZCL_STRING_CONST_SIZE`
	 * is the compile-time strlen minus trailing NUL.
	 */
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.mf_name,
			      BASIC_MANUF_NAME,
			      ZB_ZCL_STRING_CONST_SIZE(BASIC_MANUF_NAME));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.model_id,
			      BASIC_MODEL_ID,
			      ZB_ZCL_STRING_CONST_SIZE(BASIC_MODEL_ID));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.date_code,
			      BASIC_DATE_CODE,
			      ZB_ZCL_STRING_CONST_SIZE(BASIC_DATE_CODE));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.location_id,
			      BASIC_LOCATION_DESC,
			      ZB_ZCL_STRING_CONST_SIZE(BASIC_LOCATION_DESC));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_attr.sw_ver,
			      BASIC_SW_BUILD_ID,
			      ZB_ZCL_STRING_CONST_SIZE(BASIC_SW_BUILD_ID));

	/* Identify cluster (0x0003). Default identify time = 0 =
	 * device is NOT currently identifying.
	 */
	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	metering_attrs_init();
}

int zigbee_app_init(void)
{
	/* Register the endpoint context BEFORE zigbee_enable(). Without
	 * this, ZBOSS has no clusters to advertise during commissioning
	 * and bdb_start_top_level_commissioning() asserts — see #4's
	 * notes for the exact failure signature.
	 */
	ZB_AF_REGISTER_DEVICE_CTX(&app_ctx);
	app_clusters_attr_init();
	endpoint_registered = true;

	/*
	 * Sleepy-behavior is DISABLED for the USB-dev phase.
	 *
	 * `CONFIG_ZIGBEE_ROLE_END_DEVICE` (compile-time) keeps us in the
	 * ZED role at the MAC layer. What we turn off here at runtime is
	 * `zb_set_rx_on_when_idle(FALSE)` — the "radio dark between
	 * polls" mode that unlocks µA-range sleep current on AAAs.
	 *
	 * Reason to disable it TODAY: Z2M's ZCL Read of Basic.
	 * manufacturerName / .modelId during interview times out on a
	 * sleepy ED because the default parent-poll cadence (~7.5 s) is
	 * slower than Z2M's read deadline. The result is
	 * `interview_state: FAILED — can not get active endpoints` on
	 * a fresh join, and Z2M never auto-configures reporting on
	 * CurrentSummationDelivered.
	 *
	 * Rx-on-when-idle=TRUE burns ~5 mA average — a non-starter for
	 * the multi-year AAA target — but fine for the USB-dev phase
	 * we're in (see project overview memory). Turning it back on
	 * lands in #6/#7 alongside the LPCOMP pulse chain and real
	 * power measurement; those change the wake model wholesale
	 * so there's no point half-implementing sleep here.
	 */
	zigbee_configure_sleepy_behavior(false);

	zigbee_enable();
	return 0;
}

/* Callbacks that talk to ZBOSS API must run on the ZBOSS thread — see
 * the equivalent comment in the join/reset flow.
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

/*
 * Trampoline that runs on the ZBOSS thread and does the actual
 * `ZB_ZCL_SET_ATTRIBUTE` call. The publish API packs the pulse total
 * into a scheduler-callback-sized u8 index via a shadow variable —
 * `param` is only 8 bits so we can't pass the 48-bit summation
 * through it directly.
 */
static uint64_t pending_summation_total;

static void write_summation_attribute(uint64_t pulse_total)
{
	struct metering_u48 v = metering_scale_to_u48(pulse_total);
	zb_uint48_t summation;

	ZB_ASSIGN_UINT48(summation, v.high, v.low);

	/* Persist into the attribute table AND mark for reporting.
	 * `ZB_FALSE` on check_access means the write skips the ZCL
	 * read-only access check — required because the summation
	 * attribute is declared read-only (external writers must go
	 * through the ZCL API, but on-device app code goes direct).
	 */
	ZB_ZCL_SET_ATTRIBUTE(
		APP_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_METERING,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
		(zb_uint8_t *)&summation,
		ZB_FALSE);
}

/*
 * Explicit ZCL Report Attributes frame for CurrentSummationDelivered.
 * Runs on the ZBOSS thread. Bypasses the reporting engine's delta gate
 * — see the zigbee_app.h comment on publish_summation_and_report for
 * the rationale (issue #20).
 *
 * Two failure modes we intentionally log but recover from:
 *   * No reporting slot for the attribute yet — coordinator hasn't
 *     sent ConfigureReporting since our last join, or we booted before
 *     Z2M finished interviewing. Frame can't be built without a
 *     destination; skip and let the reporting engine try again next
 *     tick.
 *   * No free OUT buffer. ZBOSS's buffer pool is small; a report on
 *     top of an already-pending report can transiently starve. Skip
 *     and rely on the next tick.
 */
static void send_explicit_summation_report(void)
{
	zb_zcl_reporting_info_t *rep_info = zb_zcl_find_reporting_info(
		APP_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_METERING,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID);

	if (rep_info == NULL) {
		LOG_WRN("no reporting slot for CurrentSummationDelivered — "
			"coordinator ConfigureReporting not seen yet");
		return;
	}

	LOG_INF("reporting slot: min_interval=%us max_interval=%us "
		"reportable_change=0x%04x%08x",
		rep_info->u.send_info.min_interval,
		rep_info->u.send_info.max_interval,
		rep_info->u.send_info.delta.u48.high,
		rep_info->u.send_info.delta.u48.low);

	zb_bufid_t bufid = zb_buf_get_out();

	if (bufid == 0) {
		LOG_WRN("no free OUT buffer for explicit summation report");
		return;
	}

	zb_zcl_send_report_attr_command(rep_info, bufid);
}

static void publish_summation_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);

	write_summation_attribute(pending_summation_total);
}

static void publish_summation_and_report_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);

	write_summation_attribute(pending_summation_total);
	send_explicit_summation_report();
}

void zigbee_app_start_join(void)
{
	LOG_INF("starting network steering");
	/* Mark this steering as user-initiated so the signal handler
	 * paints the outcome (see zboss_signal_handler). The blue-blink
	 * request goes in here so the LED shows intent immediately —
	 * a stack-internal steering that hasn't been requested by the
	 * app never sets this flag and never asks for the LED.
	 */
	atomic_set(&user_join_in_flight, 1);
	led_request(LED_PATTERN_JOINING, LED_PRIO_JOINING);
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

void zigbee_app_publish_summation(uint64_t pulse_total)
{
	if (!endpoint_registered) {
		/* zigbee_app_init() failed or hasn't run yet — nothing
		 * to publish into. Silent no-op keeps main.c's sample
		 * loop from having to gate its call site on Zigbee
		 * bring-up state.
		 */
		return;
	}

	/* Hand off to the ZBOSS thread. `pending_summation_total` is
	 * a single-writer/single-reader shadow, and back-to-back calls
	 * will collapse into whichever value the ZBOSS thread reads
	 * last — that's actually the behaviour we want (report the
	 * freshest total, not a queue of stale ones).
	 */
	pending_summation_total = pulse_total;
	ZB_SCHEDULE_APP_CALLBACK(publish_summation_cb, 0);
}

void zigbee_app_publish_summation_and_report(uint64_t pulse_total)
{
	if (!endpoint_registered) {
		return;
	}

	pending_summation_total = pulse_total;
	ZB_SCHEDULE_APP_CALLBACK(publish_summation_and_report_cb, 0);
}

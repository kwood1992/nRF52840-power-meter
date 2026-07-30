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

#if IS_ENABLED(CONFIG_APP_HW_HFCLK_PROBE)
#include <hal/nrf_clock.h>
#endif

#include "calibration.h"
#include "hw_pulse_counter.h"
#include "led_controller.h"
#include "metering_scale.h"
#include "nvs_store.h"
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
 * row): Multiplier=1, Divisor=1000 (imp/kWh), UnitOfMeasure=0 (kWh),
 * SummationFormatting=0 (no additional formatting hints — Z2M knows to
 * apply Divisor). MeteringDeviceType=0 = "Electric Metering".
 *
 * The pulse-count-to-kWh math lives in Z2M: kWh = raw_summation ×
 * Multiplier ÷ Divisor. On a 1000 imp/kWh meter the raw summation IS
 * the pulse count, and Divisor=1000 gives Z2M kWh directly. For a
 * different meter (say 800 imp/kWh), overwrite Divisor from Z2M — see
 * `Metering Divisor is runtime-writable` below.
 *
 * Multiplier is pinned to 1 (kept read-only via the stock ZBOSS
 * descriptor macros) and is not persisted. The only tuning knob is
 * Divisor.
 */
#define METERING_MULTIPLIER      1U
#define METERING_DEFAULT_DIVISOR CONFIG_APP_METERING_DEFAULT_IMP_PER_KWH
#define METERING_UNIT_KWH        ZB_ZCL_METERING_UNIT_OF_MEASURE_DEFAULT_VALUE  /* 0 = kWh */
#define METERING_SUMM_FORMATTING 0U
#define METERING_DEVICE_TYPE     0U  /* 0 = Electric Metering per SE 1.4 D.5.2.2.5.2 */

/*
 * Manufacturer-specific min-pulse-width filter attribute (issue #59
 * impl-2). Attribute ID 0xF000 sits in the manufacturer-specific range
 * of the Metering cluster's attribute space; manuf_code 0x1015 is
 * Nordic Semiconductor's Zigbee Alliance member ID. The pair uniquely
 * identifies our extension against any collision on the wire.
 *
 * Kept in the Metering cluster (rather than a new manufacturer cluster)
 * so a single external converter serves both the imp/kWh knob (#48) and
 * this one — reduces surface area, avoids a second endpoint cluster
 * declaration + its ZBOSS boilerplate.
 */
#define ZB_ZCL_ATTR_METERING_MIN_PULSE_WIDTH_US_ID  0xF000U
#define ZB_ZCL_MIN_PULSE_WIDTH_MANUF_CODE           0x1015U

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

	/* Manufacturer-specific min-pulse-width filter threshold in µs
	 * (issue #59 impl-2). ZCL type u16; Kconfig range 100-10000
	 * fits comfortably. Exposed under manuf_code 0x1015 (Nordic
	 * Semiconductor's Zigbee Alliance member ID) so a naive Z2M
	 * probe treats it as a Nordic-vendor extension. Attribute ID
	 * 0xF000 is inside the manufacturer-specific range.
	 */
	zb_uint16_t metering_min_pulse_width_us;
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

/*
 * Metering Divisor is runtime-writable (issue #48). The stock ZBOSS
 * macro `ZB_ZCL_DECLARE_METERING_ATTRIB_LIST_EXT` declares Divisor as
 * `ZB_ZCL_ATTR_ACCESS_READ_ONLY` (zb_zcl_metering.h:2438-2445), which
 * makes Z2M's attribute-write path return NOT_AUTHORIZED at the ZCL
 * layer — no callback fires, no persistence hook runs. Hand-roll the
 * attribute list so we can flip just Divisor to READ_WRITE using
 * `ZB_ZCL_SET_ATTR_DESC_M`. All other entries reuse the stock
 * descriptors so the access flags for CurrentSummationDelivered et al
 * stay identical to what the ncs-zigbee reference builds ship.
 *
 * Multiplier stays read-only intentionally: keeping the two-variable
 * `kWh = raw × Mult ÷ Div` simple by making only one of them tunable
 * (Divisor == imp/kWh) is a deliberate scope choice — see the issue.
 */
ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(metering_attr_list,
						  ZB_ZCL_METERING)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
			     &dev_ctx.metering_current_summation)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_STATUS_ID,
			     &dev_ctx.metering_status)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
			     &dev_ctx.metering_unit_of_measure)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID,
			     &dev_ctx.metering_summation_formatting)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
			     &dev_ctx.metering_device_type)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID,
			     &dev_ctx.metering_instantaneous_demand)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_DEMAND_FORMATTING_ID,
			     &dev_ctx.metering_demand_formatting)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_HISTORICAL_CONSUMPTION_FORMATTING_ID,
			     &dev_ctx.metering_historical_consumption_formatting)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
			     &dev_ctx.metering_multiplier)
	ZB_ZCL_SET_ATTR_DESC_M(ZB_ZCL_ATTR_METERING_DIVISOR_ID,
			       &dev_ctx.metering_divisor,
			       ZB_ZCL_ATTR_TYPE_U24,
			       ZB_ZCL_ATTR_ACCESS_READ_WRITE)
	/*
	 * Manufacturer-specific min-pulse-width filter threshold (issue
	 * #59 impl-2). Kept in the Metering cluster's attribute list so
	 * the external converter can address it against the same cluster
	 * as the Divisor override — one converter, two knobs. The
	 * manuf_code (Nordic 0x1015) segregates it from any future
	 * Metering-cluster attribute ID collision.
	 */
	ZB_ZCL_SET_MANUF_SPEC_ATTR_DESC(
		ZB_ZCL_ATTR_METERING_MIN_PULSE_WIDTH_US_ID,
		ZB_ZCL_ATTR_TYPE_U16,
		ZB_ZCL_ATTR_ACCESS_READ_WRITE,
		ZB_ZCL_MIN_PULSE_WIDTH_MANUF_CODE,
		&dev_ctx.metering_min_pulse_width_us)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

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

/*
 * The current effective Divisor (imp/kWh), kept in sync with the
 * ZCL attribute table. Two roles:
 *   - Compare-against-previous when a Z2M attribute write arrives, so
 *     a rejected out-of-range write can be rolled back without
 *     re-parsing the packed u24 attribute value.
 *   - Debug logging so a "why is Z2M reading 0.5 kWh instead of 1?"
 *     can be answered from the console.
 * Populated from NVS (or CONFIG_APP_METERING_DEFAULT_IMP_PER_KWH on
 * cold boot) inside metering_attrs_init(), then updated by the ZCL
 * device callback on every accepted Divisor write.
 */
static uint32_t effective_divisor;

/* Set by zigbee_app_start_join() (Zephyr thread context) and cleared by
 * the ZBOSS signal handler on ZB_BDB_SIGNAL_STEERING (ZBOSS thread
 * context) — an atomic keeps the two-thread hand-off honest. Only
 * user-initiated joins get LED feedback; stack-internal auto-rejoin
 * after ZB_ZDO_SIGNAL_LEAVE stays silent so a coordinator restart
 * in the middle of the night doesn't blink a mounted meter.
 */
static atomic_t user_join_in_flight = ATOMIC_INIT(0);

/*
 * After a successful join (steering) or a successful re-attach
 * (device_reboot), set the steady-state long-poll interval and start
 * turbo poll for the Z2M interview window. Turbo poll runs at ~100 ms
 * cadence for its timeout and auto-reverts to the long-poll interval
 * on expiry — no separate leave callback required.
 *
 * ZBOSS API note: zb_zdo_pim_set_long_poll_interval is only valid
 * AFTER join; during steering the interval snaps back to the default
 * (5 s). That's why this runs from the signal handler, not init.
 */
static void apply_sleepy_poll_intervals_if_joined(zb_ret_t status)
{
	if (!IS_ENABLED(CONFIG_APP_ZIGBEE_SLEEPY_ED) || status != RET_OK) {
		return;
	}
	zb_zdo_pim_set_long_poll_interval(
		CONFIG_APP_ZIGBEE_LONG_POLL_INTERVAL_MS);
	zb_zdo_pim_start_turbo_poll_continuous(
		CONFIG_APP_ZIGBEE_JOIN_TURBO_POLL_MS);
	LOG_INF("sleepy ED: long_poll=%d ms, turbo_poll_window=%d ms",
		CONFIG_APP_ZIGBEE_LONG_POLL_INTERVAL_MS,
		CONFIG_APP_ZIGBEE_JOIN_TURBO_POLL_MS);
}

/*
 * HFCLK anchor probe for the 2026-07-29 hunt (#8). The 1.65 mA settled
 * baseline looks like "CPU System-ON idle, HFCLK on" from the datasheet;
 * this probe confirms whether HFCLK is actually running post-join by
 * reading the peripheral status registers directly. If HFCLKSTAT=0 and
 * HFCLKRUN=0 in the settled state, the anchor is somewhere other than
 * HFCLK. If HFCLKRUN=1 the next question is which stack holds the
 * refcount — see docs/working/2026-07-29-hfclk-anchor-hunt.md.
 *
 * Piggy-backs on the same call sites as the rx-idle probe (boot,
 * post-steering, post-reboot-reattach, 60 s tick).
 */
#if IS_ENABLED(CONFIG_APP_HW_HFCLK_PROBE)

/*
 * TIMER0-4 register snapshot + running-vs-stopped probe.
 *
 * MODE=0 (Timer, not Counter) + timer actually counting is what anchors
 * PCLK16M and keeps HFCLK on HFINT. INTENSET alone doesn't answer the
 * running question — a driver can leave IRQs disabled while using
 * events via PPI or shortcuts, so INTENSET=0 does NOT mean stopped.
 *
 * To detect running, do two back-to-back TASKS_CAPTURE hits into CC[5]
 * and log the delta. Non-zero delta → timer is running RIGHT NOW.
 * CC[5] is the least-likely CC slot to conflict with a driver's use
 * (they typically use CC[0..2]); still a soft assumption.
 *
 * The nRF52840 has TIMER0-4 as separate peripheral instances at
 * distinct base addresses; iterate via an array of pointers.
 */
static NRF_TIMER_Type * const app_timers[] = {
	NRF_TIMER0, NRF_TIMER1, NRF_TIMER2, NRF_TIMER3, NRF_TIMER4,
};

static void log_timer_status(const char *where)
{
	for (size_t i = 0; i < ARRAY_SIZE(app_timers); i++) {
		NRF_TIMER_Type *t = app_timers[i];
		uint32_t v1, v2;

		t->TASKS_CAPTURE[5] = 1;
		v1 = t->CC[5];
		t->TASKS_CAPTURE[5] = 1;
		v2 = t->CC[5];

		LOG_INF("timer probe [%s]: T%u MODE=%u BITMODE=%u PRESCALER=%u INTENSET=0x%08x CC5_delta=%u running=%d",
			where, (unsigned)i,
			(unsigned)t->MODE,
			(unsigned)t->BITMODE,
			(unsigned)t->PRESCALER,
			(unsigned)t->INTENSET,
			(unsigned)(v2 - v1),
			(v2 != v1) ? 1 : 0);
	}
}

static void log_hfclk_status(const char *where)
{
	bool xtal_running = nrf_clock_hf_is_running(
		NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY);
	uint32_t hfclkstat = NRF_CLOCK->HFCLKSTAT;
	uint32_t hfclkrun = NRF_CLOCK->HFCLKRUN;

	LOG_INF("hfclk probe [%s]: HFCLKSTAT=0x%08x HFCLKRUN=0x%08x xtal_running=%d",
		where, hfclkstat, hfclkrun, xtal_running ? 1 : 0);
	log_timer_status(where);
}

#else /* !CONFIG_APP_HW_HFCLK_PROBE */

static inline void log_hfclk_status(const char *where) { ARG_UNUSED(where); }

#endif /* CONFIG_APP_HW_HFCLK_PROBE */

/*
 * Diagnostic probe for issue #51 / #8-blocker-1: does ZBOSS clobber our
 * zb_set_rx_on_when_idle(FALSE) setting post-join? Sample the current
 * value at boot, after each signal event, and on a 60 s tick — under the
 * hypothesis that a sleepy ED is silently reverting to rx-on-when-idle
 * once it enters steady state, which would explain the 12.4 mA baseline
 * seen in the 2026-07-29 INA219 run.
 *
 * Gated on CONFIG_APP_ZIGBEE_RX_IDLE_PROBE so production builds don't
 * emit these lines. Enabled by rtt.conf. Also acts as the tick+kick
 * carrier for the HFCLK probe above (selected via Kconfig).
 */
#if IS_ENABLED(CONFIG_APP_ZIGBEE_RX_IDLE_PROBE)

#define RX_IDLE_PROBE_INTERVAL K_SECONDS(60)

static void rx_idle_probe_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rx_idle_probe_work, rx_idle_probe_work_handler);

static void log_rx_on_when_idle(const char *where)
{
	zb_bool_t rx_on = zb_get_rx_on_when_idle();

	LOG_INF("rx-on-when-idle probe [%s]: %s (joined=%d)",
		where, rx_on ? "TRUE" : "FALSE", joined ? 1 : 0);
}

static void rx_idle_probe_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	log_rx_on_when_idle("tick");
	log_hfclk_status("tick");
	k_work_reschedule(&rx_idle_probe_work, RX_IDLE_PROBE_INTERVAL);
}

static void rx_idle_probe_kick(const char *where)
{
	log_rx_on_when_idle(where);
	log_hfclk_status(where);
	k_work_reschedule(&rx_idle_probe_work, RX_IDLE_PROBE_INTERVAL);
}

#else /* !CONFIG_APP_ZIGBEE_RX_IDLE_PROBE */

static inline void log_rx_on_when_idle(const char *where) { ARG_UNUSED(where); }
static inline void rx_idle_probe_kick(const char *where) { ARG_UNUSED(where); }

#endif /* CONFIG_APP_ZIGBEE_RX_IDLE_PROBE */

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
		if (joined) {
			rx_idle_probe_kick("post-steering");
		}

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
		apply_sleepy_poll_intervals_if_joined(status);
		break;

	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
		/* Reboot that re-attached to the saved network in NVRAM (no
		 * fresh steering). Sleepy poll intervals aren't persisted, so
		 * re-apply them here. No LED — there was no user-initiated
		 * join event on this path.
		 */
		if (status == RET_OK) {
			LOG_INF("device reboot: reattached to saved network");
			joined = true;
			rx_idle_probe_kick("post-reboot-reattach");
		}
		apply_sleepy_poll_intervals_if_joined(status);
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
	 *
	 * Multiplier is pinned to 1 (design-doc default; the only tuning
	 * knob is Divisor).
	 *
	 * Divisor comes from NVS if a Z2M write has previously been
	 * persisted; otherwise falls back to the compile-time default.
	 * A value in NVS that has fallen out of range (e.g., a firmware
	 * that used to accept a wider range wrote something we now
	 * reject) is treated the same as no value — surface the fallback
	 * via a warning so the installer notices, but don't refuse to
	 * boot.
	 */
	zb_uint32_to_uint24(METERING_MULTIPLIER, &dev_ctx.metering_multiplier);

	uint32_t divisor = METERING_DEFAULT_DIVISOR;
	uint32_t nvs_val = 0;
	int rc = nvs_store_load_imp_per_kwh(&nvs_val);

	if (rc == 0) {
		if (calibration_is_valid_imp_per_kwh(nvs_val)) {
			divisor = nvs_val;
			LOG_INF("Metering Divisor: imp/kWh=%u (from NVS)",
				(unsigned)divisor);
		} else {
			LOG_WRN("NVS imp/kWh=%u out of range [%u..%u] — "
				"using compile-time default %u",
				(unsigned)nvs_val,
				CALIBRATION_IMP_PER_KWH_MIN,
				CALIBRATION_IMP_PER_KWH_MAX,
				(unsigned)divisor);
		}
	} else if (rc == -ENOENT) {
		LOG_INF("Metering Divisor: imp/kWh=%u (compile-time default; "
			"nothing persisted yet)",
			(unsigned)divisor);
	} else {
		LOG_ERR("nvs_store_load_imp_per_kwh failed: %d — using default %u",
			rc, (unsigned)divisor);
	}

	effective_divisor = divisor;
	zb_uint32_to_uint24(divisor, &dev_ctx.metering_divisor);

	/*
	 * Manufacturer-specific min-pulse-width filter threshold (#59
	 * impl-2). The active TIMER3.CC[0] value was already programmed
	 * by hw_pulse_counter_init() from the same NVS slot; we just
	 * need to mirror it into the ZCL attribute so a Z2M read reflects
	 * the running value. Do the load again (cheap) so this table
	 * stays authoritative even if the load order swaps in future.
	 */
	uint32_t min_width_us = CONFIG_APP_PULSE_MIN_WIDTH_US;
	uint32_t nvs_width = 0;
	int rc_width = nvs_store_load_pulse_min_width_us(&nvs_width);

	if (rc_width == 0 &&
	    calibration_is_valid_pulse_min_width_us(nvs_width)) {
		min_width_us = nvs_width;
	}
	dev_ctx.metering_min_pulse_width_us = (zb_uint16_t)min_width_us;
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

/*
 * ZCL device callback — invoked by ZBOSS post-write, after the
 * incoming attribute value has already been written into
 * `dev_ctx.metering_divisor`. If we reject, ZBOSS uses `p->status`
 * as the ZCL response; the attribute-table update is our problem to
 * undo (see the rollback path below).
 *
 * Scope: this handler is a filter for one attribute (Metering.Divisor)
 * and quietly RET_OK's every other invocation so cluster machinery
 * that ZBOSS drives through the same callback (identify effects, etc.)
 * still works. Other paths that want to hook this callback in future
 * should extend the switch here rather than replacing it.
 */
static void zcl_device_cb(zb_bufid_t bufid)
{
	zb_zcl_device_callback_param_t *p =
		ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t);

	p->status = RET_OK;

	if (p->device_cb_id != ZB_ZCL_SET_ATTR_VALUE_CB_ID) {
		return;
	}
	if (p->cb_param.set_attr_value_param.cluster_id !=
	    ZB_ZCL_CLUSTER_ID_METERING) {
		return;
	}

	uint16_t attr_id = p->cb_param.set_attr_value_param.attr_id;

	if (attr_id == ZB_ZCL_ATTR_METERING_MIN_PULSE_WIDTH_US_ID) {
		uint16_t new_width =
			p->cb_param.set_attr_value_param.values.data16;

		if (!calibration_is_valid_pulse_min_width_us(new_width)) {
			LOG_WRN("rejecting min-pulse-width write: %u out of "
				"range [%u..%u] — rolling back to %u",
				(unsigned)new_width,
				CALIBRATION_PULSE_MIN_WIDTH_US_MIN,
				CALIBRATION_PULSE_MIN_WIDTH_US_MAX,
				(unsigned)dev_ctx.metering_min_pulse_width_us);

			uint16_t rollback = dev_ctx.metering_min_pulse_width_us;

			ZB_ZCL_SET_ATTRIBUTE(APP_ENDPOINT,
					     ZB_ZCL_CLUSTER_ID_METERING,
					     ZB_ZCL_CLUSTER_SERVER_ROLE,
					     ZB_ZCL_ATTR_METERING_MIN_PULSE_WIDTH_US_ID,
					     (zb_uint8_t *)&rollback,
					     ZB_FALSE);
			p->status = RET_OUT_OF_RANGE;
			return;
		}

		/* Push to hardware first — even if NVS persistence fails,
		 * the running filter tracks the accepted value until the
		 * next reboot (which will revert to the last persisted or
		 * compile-time default). Same in-memory-only fallback as
		 * the Divisor path below.
		 */
		hw_pulse_counter_set_min_width_us(new_width);

		int rc = nvs_store_save_pulse_min_width_us(new_width);

		if (rc) {
			LOG_ERR("nvs_store_save_pulse_min_width_us(%u) "
				"failed: %d — value applied in memory only",
				(unsigned)new_width, rc);
		} else {
			LOG_INF("min-pulse-width updated: %u µs (persisted)",
				(unsigned)new_width);
		}

		dev_ctx.metering_min_pulse_width_us = new_width;
		return;
	}

	if (attr_id != ZB_ZCL_ATTR_METERING_DIVISOR_ID) {
		return;
	}

	/* zb_uint24_t on the target is a packed {u16 low, u8 high}
	 * struct (zb_types.h:1011). Rebuild the u32 by hand — the
	 * ZBOSS helper only goes the other way (u32→u24).
	 */
	zb_uint24_t v24 = p->cb_param.set_attr_value_param.values.data24;
	uint32_t new_divisor = ((uint32_t)v24.high << 16) | v24.low;

	if (!calibration_is_valid_imp_per_kwh(new_divisor)) {
		LOG_WRN("rejecting Metering Divisor write: %u out of range "
			"[%u..%u] — rolling back to %u",
			(unsigned)new_divisor,
			CALIBRATION_IMP_PER_KWH_MIN,
			CALIBRATION_IMP_PER_KWH_MAX,
			(unsigned)effective_divisor);

		/* ZBOSS already committed the bad value to the attribute
		 * store; put the previously-effective value back so a
		 * follow-up Z2M read returns the still-valid Divisor.
		 * ZB_FALSE bypasses the access-flag check (we're the app,
		 * we own this attribute).
		 */
		zb_uint24_t rollback;

		zb_uint32_to_uint24(effective_divisor, &rollback);
		ZB_ZCL_SET_ATTRIBUTE(APP_ENDPOINT,
				     ZB_ZCL_CLUSTER_ID_METERING,
				     ZB_ZCL_CLUSTER_SERVER_ROLE,
				     ZB_ZCL_ATTR_METERING_DIVISOR_ID,
				     (zb_uint8_t *)&rollback,
				     ZB_FALSE);
		p->status = RET_OUT_OF_RANGE;
		return;
	}

	int rc = nvs_store_save_imp_per_kwh(new_divisor);

	if (rc) {
		/* Persist failed — keep the write in memory anyway. Losing
		 * the value on next reboot is a worse outcome than
		 * refusing a valid write; the installer will notice next
		 * boot when the value snaps back to the compile-time
		 * default.
		 */
		LOG_ERR("nvs_store_save_imp_per_kwh(%u) failed: %d — "
			"value applied in memory only",
			(unsigned)new_divisor, rc);
	} else {
		LOG_INF("Metering Divisor updated: imp/kWh=%u (persisted)",
			(unsigned)new_divisor);
	}

	effective_divisor = new_divisor;
}

int zigbee_app_init(void)
{
	/* Register the endpoint context BEFORE zigbee_enable(). Without
	 * this, ZBOSS has no clusters to advertise during commissioning
	 * and bdb_start_top_level_commissioning() asserts — see #4's
	 * notes for the exact failure signature.
	 */
	ZB_ZCL_REGISTER_DEVICE_CB(zcl_device_cb);
	ZB_AF_REGISTER_DEVICE_CTX(&app_ctx);
	app_clusters_attr_init();
	endpoint_registered = true;

	/*
	 * Sleepy-ED behaviour (issue #8 blocker 1).
	 *
	 * CONFIG_ZIGBEE_ROLE_END_DEVICE fixes the MAC-layer role;
	 * zb_set_rx_on_when_idle() (called via zigbee_configure_sleepy_behavior)
	 * is the runtime flag that actually gates whether the radio sleeps
	 * between parent polls. rx-on-when-idle=TRUE burns ~5 mA average —
	 * a non-starter for the AAA target — so we default to sleepy=true
	 * and cover Z2M's interview deadline with a turbo-poll window
	 * (see zboss_signal_handler for ZB_BDB_SIGNAL_STEERING).
	 *
	 * dev.conf can flip APP_ZIGBEE_SLEEPY_ED=n as an escape hatch if the
	 * turbo-poll window ever fails to cover a slow Z2M read — that path
	 * is what the previous USB-dev build used unconditionally.
	 *
	 * ed_timeout / keepalive_timeout come from the R23 light_switch
	 * reference sample (samples/light_switch/src/main.c:837-838). Without
	 * a large aging timeout, the parent evicts a sleepy child before the
	 * next long-poll cycle — the practical failure mode is a device that
	 * joins once, drops off after ~7.5 s (ZB_TIME_ED_IDLE default), and
	 * never rejoins because it never wakes.
	 */
	if (IS_ENABLED(CONFIG_APP_ZIGBEE_SLEEPY_ED)) {
		zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN);
		zb_set_keepalive_timeout(
			ZB_MILLISECONDS_TO_BEACON_INTERVAL(30000));
	}
	zigbee_configure_sleepy_behavior(IS_ENABLED(CONFIG_APP_ZIGBEE_SLEEPY_ED));
	rx_idle_probe_kick("post-configure-sleepy");

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
 * Short-press wake (#62). Restarting turbo poll on an already-joined
 * device only changes the poll cadence — join state, bindings and the
 * attribute table are all untouched, which is what the acceptance
 * criteria require. ZBOSS reverts to the long-poll interval when the
 * window expires, so there's nothing to tear down afterwards.
 */
static void wake_turbo_poll_cb(zb_uint8_t param)
{
	ARG_UNUSED(param);
	zb_zdo_pim_start_turbo_poll_continuous(
		CONFIG_APP_ZIGBEE_WAKE_TURBO_POLL_MS);
	LOG_INF("ZBOSS thread: wake turbo-poll window open for %d ms",
		CONFIG_APP_ZIGBEE_WAKE_TURBO_POLL_MS);
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

void zigbee_app_wake_for_write(void)
{
	if (!IS_ENABLED(CONFIG_APP_ZIGBEE_SLEEPY_ED)) {
		/* Radio is already always-on; downlinks arrive immediately
		 * and there is no poll cadence to shorten.
		 */
		LOG_INF("wake request ignored — not a sleepy ED");
		return;
	}
	if (!joined) {
		/* zb_zdo_pim_* addresses the parent, and without an
		 * association there isn't one. The button dispatch routes
		 * this case to JOIN instead, so reaching here means the
		 * device dropped its parent between classify and dispatch.
		 */
		LOG_WRN("wake request ignored — not joined");
		return;
	}

	LOG_INF("wake for write: requesting %d ms turbo-poll window",
		CONFIG_APP_ZIGBEE_WAKE_TURBO_POLL_MS);
	ZB_SCHEDULE_APP_CALLBACK(wake_turbo_poll_cb, 0);
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

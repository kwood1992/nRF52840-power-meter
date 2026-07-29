#include "hw_pulse_counter.h"

#include "calibration.h"
#include "nvs_store.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_lpcomp.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>
#include <nrfx_lpcomp.h>
#include <nrfx_timer.h>

LOG_MODULE_REGISTER(hw_pulse_counter, LOG_LEVEL_INF);

/* TIMER2 is the pulse counter — 32-bit, counter mode, incremented by
 * TASKS_COUNT via PPI. TIMER0 is Nordic's radio softblock and TIMER1 is
 * claimed by the 802.15.4 driver (Kconfig at
 * zephyr/modules/hal_nordic/Kconfig `depends on !$(dt_nodelabel_enabled,timer1)`
 * makes enabling timer1 in DT a build error). TIMER2 has 4 CC channels
 * — only 1 needed for software-triggered CAPTURE reads — and 32-bit
 * width. See modules/hal/nordic/nrfx/samples/src/nrfx_timer/counter for
 * the canonical counter-mode idiom.
 */
static const nrfx_timer_t counter_timer = NRFX_TIMER_INSTANCE(2);
#define COUNTER_CAPTURE_CH NRF_TIMER_CC_CHANNEL0

/* TIMER3 is the min-pulse-width filter's width-measure timer (#59). Timer
 * mode, 1 MHz, 16-bit — plenty of range for the 100 µs–10 ms threshold
 * band we ever want and small enough that the CC[0]_STOP short fires
 * quickly. CC[0] holds the threshold in µs (1 µs per tick at 1 MHz);
 * SHORTS_COMPARE0_STOP freezes the timer once the threshold is crossed
 * so PCLK16M is only requested while a pulse is in flight — the design
 * spike's HFCLK-on cost analysis (docs/working/2026-07-29-min-pulse-
 * width-spike.md) depends on this stop-on-threshold behaviour.
 */
static const nrfx_timer_t width_timer = NRFX_TIMER_INSTANCE(3);
#define WIDTH_THRESHOLD_CH NRF_TIMER_CC_CHANNEL0

/* nRF52840 has one GPIOTE instance. Zephyr's GPIO driver also uses it
 * (for edge-triggered gpio_pin_interrupt_configure on other pins like
 * D6 = user button); nrfx's channel allocator arbitrates ownership.
 */
static const nrfx_gpiote_t gpiote = NRFX_GPIOTE_INSTANCE(0);

/* XIAO D7 = P1.12. Kept in sync manually with any change to app.overlay's
 * sw0/D6 vs D7 assignments — D7 is not a Zephyr GPIO consumer anymore, so
 * there's no DT alias to derive it from. If the pin ever moves, update
 * both this constant and the docs/README bench-wiring pointer.
 */
#define BENCH_PULSE_ABS_PIN NRF_GPIO_PIN_MAP(1, 12)

/* LPCOMP's nrfx driver refuses a NULL event handler (see
 * nrfx_lpcomp.h:114). We pass this stub and then start LPCOMP with an
 * event-enable-mask of 0, so the LPCOMP IRQ line stays masked at the
 * peripheral and this handler is never invoked. That's the whole point
 * of the LPCOMP → PPI → TIMER chain: pulses are counted in hardware
 * without waking the CPU.
 */
static void lpcomp_evt_stub(nrf_lpcomp_event_t event)
{
	ARG_UNUSED(event);
}

/* Load the effective threshold from NVS if the persisted value is in
 * range; otherwise fall back to the Kconfig compile-time default. Same
 * shape as the imp/kWh Divisor restore path in zigbee_app.c (issue #48).
 * The NVS/Kconfig split lets the field-adjust story from #59 impl-2
 * survive reboots without needing every deployment to reflash.
 */
static uint32_t effective_min_width_us_at_init(void)
{
	uint32_t nvs_val = 0;
	int rc = nvs_store_load_pulse_min_width_us(&nvs_val);

	if (rc == 0) {
		if (calibration_is_valid_pulse_min_width_us(nvs_val)) {
			LOG_INF("min-pulse-width: %u µs (from NVS)",
				(unsigned)nvs_val);
			return nvs_val;
		}
		LOG_WRN("NVS pulse_min_width_us=%u out of range — "
			"falling back to compile-time default %u µs",
			(unsigned)nvs_val,
			(unsigned)CONFIG_APP_PULSE_MIN_WIDTH_US);
	} else if (rc != -ENOENT) {
		LOG_ERR("nvs_store_load_pulse_min_width_us failed: %d — "
			"using compile-time default %u µs",
			rc, (unsigned)CONFIG_APP_PULSE_MIN_WIDTH_US);
	} else {
		LOG_INF("min-pulse-width: %u µs (compile-time default)",
			(unsigned)CONFIG_APP_PULSE_MIN_WIDTH_US);
	}

	return (uint32_t)CONFIG_APP_PULSE_MIN_WIDTH_US;
}

int hw_pulse_counter_init(void)
{
	nrfx_err_t err;

	/* --- TIMER2 (32-bit counter). NULL handler; no IRQ_CONNECT
	 * needed because we never enable a compare/capture interrupt.
	 *
	 * nrfx `.frequency` MUST be non-zero even in counter mode — the
	 * driver calls prescaler_calculate() unconditionally from
	 * nrfx_timer_init() and frequency=0 causes DIV_0_TRP →
	 * K_ERR_ARM_USAGE_DIV_0. In counter mode the timer counts
	 * external COUNT-task edges so the internal-clock prescaler is
	 * functionally irrelevant — 1 MHz gives a legal prescaler=4.
	 */
	const nrfx_timer_config_t counter_cfg = {
		.frequency = NRFX_MHZ_TO_HZ(1),
		.mode = NRF_TIMER_MODE_COUNTER,
		.bit_width = NRF_TIMER_BIT_WIDTH_32,
		.interrupt_priority = NRFX_TIMER_DEFAULT_CONFIG_IRQ_PRIORITY,
		.p_context = NULL,
	};

	err = nrfx_timer_init(&counter_timer, &counter_cfg, NULL);
	if (err != NRFX_SUCCESS && err != NRFX_ERROR_ALREADY) {
		LOG_ERR("nrfx_timer_init(counter) failed: 0x%08x", err);
		return -EIO;
	}
	nrfx_timer_clear(&counter_timer);

	/* --- TIMER3 (16-bit width-measure timer for #59). 1 MHz → 1 µs
	 * per tick. NULL handler; CC[0] event goes to PPI, not to CPU.
	 */
	const nrfx_timer_config_t width_cfg = {
		.frequency = NRFX_MHZ_TO_HZ(1),
		.mode = NRF_TIMER_MODE_TIMER,
		.bit_width = NRF_TIMER_BIT_WIDTH_16,
		.interrupt_priority = NRFX_TIMER_DEFAULT_CONFIG_IRQ_PRIORITY,
		.p_context = NULL,
	};

	err = nrfx_timer_init(&width_timer, &width_cfg, NULL);
	if (err != NRFX_SUCCESS && err != NRFX_ERROR_ALREADY) {
		LOG_ERR("nrfx_timer_init(width) failed: 0x%08x", err);
		return -EIO;
	}

	/* CC[0] = threshold in µs (= ticks at 1 MHz). COMPARE0_STOP
	 * short freezes the timer once the threshold is crossed, so
	 * PCLK16M / HFCLK is only requested while a pulse is in flight.
	 * `enable_int=false` — the CC[0] event is consumed by PPI, no
	 * CPU IRQ needed.
	 *
	 * effective_min_width_us_at_init() picks from NVS (impl-2 field
	 * override) or falls back to CONFIG_APP_PULSE_MIN_WIDTH_US.
	 */
	uint32_t effective_min_width_us = effective_min_width_us_at_init();

	nrfx_timer_extended_compare(&width_timer,
				    WIDTH_THRESHOLD_CH,
				    effective_min_width_us,
				    NRF_TIMER_SHORT_COMPARE0_STOP_MASK,
				    false);
	nrfx_timer_clear(&width_timer);

	/* --- LPCOMP: AIN0 (P0.02 = XIAO A0), VDD*3/8 threshold, HYST on.
	 *
	 * Threshold is set via the DT node (`refsel = "VDD_3_8"` in
	 * app.overlay); we override the nrfx default (VDD*4/8) here in
	 * code to match. Detection stays UP-only — the nrfx driver only
	 * uses `.detection` to select which IRQ mask is enabled, not
	 * which EVENTS_ registers publish. Both EVENTS_UP and EVENTS_DOWN
	 * fire on their respective crossings regardless of the detection
	 * setting, so the PPI subscriptions to both events below are
	 * independent of this choice. (Confirmed against
	 * nrfx_lpcomp.c:75-100.)
	 *
	 * HYST is the first-line defence against threshold-band jitter;
	 * the TIMER3 min-pulse-width gate below is the second line, for
	 * crossings that are genuine but too short to be a real meter
	 * imp (ambient flicker, torch PWM — see
	 * project_led_torch_flicker_pulse_count memory and #59).
	 */
	nrfx_lpcomp_config_t lpcomp_cfg = NRFX_LPCOMP_DEFAULT_CONFIG(NRF_LPCOMP_INPUT_0);

	lpcomp_cfg.reference = NRF_LPCOMP_REF_SUPPLY_3_8;
	lpcomp_cfg.detection = NRF_LPCOMP_DETECT_UP;
#if NRF_LPCOMP_HAS_HYST
	lpcomp_cfg.hyst = NRF_LPCOMP_HYST_ENABLED;
#endif

	/* Vector-table slot must exist even though we never unmask the
	 * IRQ — a spurious LPCOMP interrupt left set from a prior boot
	 * would otherwise fault into a missing handler.
	 */
	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(comp)),
		    DT_IRQ(DT_NODELABEL(comp), priority),
		    nrfx_isr, nrfx_lpcomp_irq_handler, 0);

	err = nrfx_lpcomp_init(&lpcomp_cfg, lpcomp_evt_stub);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_lpcomp_init failed: 0x%08x", err);
		return -EIO;
	}

	/* --- GPIOTE for the bench D7 inject path.
	 *
	 * Two channels now — HITOLO for pulse-start (Pi drives D7 low),
	 * LOTOHI for pulse-end (Pi releases D7). Both feed TIMER3 the
	 * same way LPCOMP does so the bench inject is subject to the
	 * same min-pulse-width filter. Without the second channel the
	 * ticket AC bench test would exercise a filter-bypass path and
	 * prove nothing about the filter (see the spike doc's "D7
	 * bench-inject path — route through the filter" section).
	 */
	err = nrfx_gpiote_init(&gpiote, NRFX_GPIOTE_DEFAULT_CONFIG_IRQ_PRIORITY);
	if (err != NRFX_SUCCESS && err != NRFX_ERROR_ALREADY) {
		LOG_ERR("nrfx_gpiote_init failed: 0x%08x", err);
		return -EIO;
	}

	static uint8_t bench_gpiote_hitolo_ch;
	static uint8_t bench_gpiote_lotohi_ch;

	err = nrfx_gpiote_channel_alloc(&gpiote, &bench_gpiote_hitolo_ch);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gpiote channel_alloc(HITOLO) failed: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_gpiote_channel_alloc(&gpiote, &bench_gpiote_lotohi_ch);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gpiote channel_alloc(LOTOHI) failed: 0x%08x", err);
		return -EIO;
	}

	static const nrf_gpio_pin_pull_t bench_pull = NRF_GPIO_PIN_PULLUP;
	static const nrfx_gpiote_trigger_config_t bench_hitolo_trigger = {
		.trigger = NRFX_GPIOTE_TRIGGER_HITOLO,
		.p_in_channel = &bench_gpiote_hitolo_ch,
	};
	static const nrfx_gpiote_input_pin_config_t bench_hitolo_pin_cfg = {
		.p_pull_config = &bench_pull,
		.p_trigger_config = &bench_hitolo_trigger,
		.p_handler_config = NULL,
	};

	err = nrfx_gpiote_input_configure(&gpiote,
					  BENCH_PULSE_ABS_PIN,
					  &bench_hitolo_pin_cfg);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gpiote configure(HITOLO) failed: 0x%08x", err);
		return -EIO;
	}

	/* The two GPIOTE channels share the D7 pin — configuring a
	 * second edge trigger on the same physical pin. nrfx's
	 * gpiote_input_configure rewrites its per-pin bookkeeping to
	 * remember only the LAST channel it was called for, but at the
	 * hardware level both CONFIG[hitolo_ch] and CONFIG[lotohi_ch]
	 * are programmed and independent — each fires on its own edge.
	 *
	 * Consequence: the driver's nrfx_gpiote_trigger_enable(pin, ...)
	 * helper would only enable the last-configured channel. We enable
	 * both channels manually below via the HAL layer.
	 */
	static const nrfx_gpiote_trigger_config_t bench_lotohi_trigger = {
		.trigger = NRFX_GPIOTE_TRIGGER_LOTOHI,
		.p_in_channel = &bench_gpiote_lotohi_ch,
	};
	static const nrfx_gpiote_input_pin_config_t bench_lotohi_pin_cfg = {
		.p_pull_config = &bench_pull,
		.p_trigger_config = &bench_lotohi_trigger,
		.p_handler_config = NULL,
	};

	err = nrfx_gpiote_input_configure(&gpiote,
					  BENCH_PULSE_ABS_PIN,
					  &bench_lotohi_pin_cfg);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gpiote configure(LOTOHI) failed: 0x%08x", err);
		return -EIO;
	}

	/* Enable both GPIOTE channels' IN events at the HAL level —
	 * bypasses nrfx's per-pin trigger_enable so both edges fire
	 * regardless of which channel the driver's pin_flags currently
	 * remembers. No IRQ unmasking here; events flow to PPI only.
	 */
	nrf_gpiote_event_enable(NRF_GPIOTE, bench_gpiote_hitolo_ch);
	nrf_gpiote_event_enable(NRF_GPIOTE, bench_gpiote_lotohi_ch);

	/* --- PPI wiring: pulse-source events → TIMER3 start/stop with
	 * CLEAR forks, then TIMER3 CC[0] → TIMER2 COUNT. Five channels
	 * plus four fork endpoints — well inside the 20 configurable +
	 * 6 fork budget on the nRF52840.
	 *
	 * ch_up:      LPCOMP EVENTS_UP    → TIMER3 START (+ fork CLEAR)
	 * ch_down:    LPCOMP EVENTS_DOWN  → TIMER3 STOP  (+ fork CLEAR)
	 * ch_bench_h: GPIOTE IN[HITOLO]   → TIMER3 START (+ fork CLEAR)
	 * ch_bench_l: GPIOTE IN[LOTOHI]   → TIMER3 STOP  (+ fork CLEAR)
	 * ch_cnt:     TIMER3 CC[0] event  → TIMER2 COUNT
	 *
	 * The chain: pulse-source START event on ch_up / ch_bench_h resets
	 * TIMER3 to zero (fork CLEAR) and starts it (main TEP). TIMER3
	 * counts up; if it reaches CC[0] before the pulse ends, EVENTS_
	 * COMPARE[0] fires and PPI ch_cnt increments TIMER2 by one. The
	 * CC[0]_STOP short (set in nrfx_timer_extended_compare above)
	 * then freezes TIMER3, releasing PCLK16M / HFCLK. When the pulse
	 * ends, the STOP event on ch_down / ch_bench_l stops TIMER3
	 * (no-op if it stopped itself via the CC[0] short) and CLEARs it
	 * ready for the next pulse.
	 *
	 * Short pulses (< threshold) never reach CC[0]; the STOP-and-
	 * CLEAR on the DOWN edge terminates TIMER3 before it fires,
	 * so TIMER2 COUNT never triggers → pulse rejected. All in
	 * hardware; CPU stays asleep in both accept and reject paths.
	 */
	uint8_t ch_up;
	uint8_t ch_down;
	uint8_t ch_bench_h;
	uint8_t ch_bench_l;
	uint8_t ch_cnt;

	err = nrfx_gppi_channel_alloc(&ch_up);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (up) failed: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_gppi_channel_alloc(&ch_down);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (down) failed: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_gppi_channel_alloc(&ch_bench_h);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (bench_h) failed: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_gppi_channel_alloc(&ch_bench_l);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (bench_l) failed: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_gppi_channel_alloc(&ch_cnt);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (cnt) failed: 0x%08x", err);
		return -EIO;
	}

	nrfx_gppi_channel_endpoints_setup(
		ch_up,
		nrf_lpcomp_event_address_get(NRF_LPCOMP, NRF_LPCOMP_EVENT_UP),
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_START));
	nrfx_gppi_fork_endpoint_setup(
		ch_up,
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_CLEAR));

	nrfx_gppi_channel_endpoints_setup(
		ch_down,
		nrf_lpcomp_event_address_get(NRF_LPCOMP, NRF_LPCOMP_EVENT_DOWN),
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_STOP));
	nrfx_gppi_fork_endpoint_setup(
		ch_down,
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_CLEAR));

	/* Bench D7: both GPIOTE channels are configured for the same pin
	 * but different edges. Reference each channel's IN[n] event
	 * register directly (not the abs-pin helper, which would only
	 * resolve to whichever channel the driver's pin_flags currently
	 * remembers — the LOTOHI one after the second configure call).
	 */
	nrfx_gppi_channel_endpoints_setup(
		ch_bench_h,
		nrf_gpiote_event_address_get(
			NRF_GPIOTE,
			nrf_gpiote_in_event_get(bench_gpiote_hitolo_ch)),
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_START));
	nrfx_gppi_fork_endpoint_setup(
		ch_bench_h,
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_CLEAR));

	nrfx_gppi_channel_endpoints_setup(
		ch_bench_l,
		nrf_gpiote_event_address_get(
			NRF_GPIOTE,
			nrf_gpiote_in_event_get(bench_gpiote_lotohi_ch)),
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_STOP));
	nrfx_gppi_fork_endpoint_setup(
		ch_bench_l,
		nrfx_timer_task_address_get(&width_timer, NRF_TIMER_TASK_CLEAR));

	nrfx_gppi_channel_endpoints_setup(
		ch_cnt,
		nrfx_timer_compare_event_address_get(&width_timer,
						     WIDTH_THRESHOLD_CH),
		nrfx_timer_task_address_get(&counter_timer,
					    NRF_TIMER_TASK_COUNT));

	nrfx_gppi_channels_enable(BIT(ch_up) | BIT(ch_down) |
				  BIT(ch_bench_h) | BIT(ch_bench_l) |
				  BIT(ch_cnt));

	/* --- Bring the peripherals live. Counter TIMER first so it's
	 * ready to catch the very first CC[0] event from TIMER3.
	 * TIMER3 itself is left stopped; its START task is driven by
	 * the first PPI publish from an UP / HITOLO event.
	 */
	nrfx_timer_enable(&counter_timer);

	/* evt_en_mask = 0 keeps LPCOMP IRQ masked; shorts_mask = 0 means
	 * no LPCOMP shortcut is enabled. PPI still publishes the events.
	 */
	nrfx_lpcomp_start(0, 0);

	LOG_INF("hw pulse counter live: LPCOMP AIN0 refsel=VDD_3_8 HYST=on, "
		"bench D7 GPIOTE HITOLO+LOTOHI, TIMER2 counter mode, "
		"TIMER3 width filter threshold=%u µs",
		(unsigned)effective_min_width_us);

	return 0;
}

uint32_t hw_pulse_counter_read(void)
{
	return nrfx_timer_capture(&counter_timer, COUNTER_CAPTURE_CH);
}

void hw_pulse_counter_set_min_width_us(uint32_t min_width_us)
{
	/* CC[0] can be updated live — the CC0_STOP short logic just reads
	 * the current value on the next comparison. Cheap: two register
	 * writes (nrfx_timer_extended_compare also re-applies the SHORTS
	 * mask, which is idempotent). No re-init needed.
	 */
	nrfx_timer_extended_compare(&width_timer,
				    WIDTH_THRESHOLD_CH,
				    min_width_us,
				    NRF_TIMER_SHORT_COMPARE0_STOP_MASK,
				    false);
}

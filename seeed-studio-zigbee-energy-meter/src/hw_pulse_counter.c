#include "hw_pulse_counter.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_lpcomp.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>
#include <nrfx_lpcomp.h>
#include <nrfx_timer.h>

LOG_MODULE_REGISTER(hw_pulse_counter, LOG_LEVEL_INF);

/* TIMER2 is the pulse counter. TIMER0 is Nordic's radio softblock and
 * TIMER1 is claimed by the 802.15.4 driver (Kconfig at
 * zephyr/modules/hal_nordic/Kconfig `depends on !$(dt_nodelabel_enabled,timer1)`
 * makes enabling timer1 in DT a build error). TIMER2 has 4 CC channels
 * — only 1 needed for software-triggered CAPTURE reads — and 32-bit
 * width. See modules/hal/nordic/nrfx/samples/src/nrfx_timer/counter for
 * the canonical counter-mode idiom.
 */
static const nrfx_timer_t counter_timer = NRFX_TIMER_INSTANCE(2);
#define COUNTER_CAPTURE_CH NRF_TIMER_CC_CHANNEL0

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

int hw_pulse_counter_init(void)
{
	nrfx_err_t err;

	/* --- TIMER2 in 32-bit counter mode. NULL handler; no IRQ_CONNECT
	 * needed because we never enable a compare/capture interrupt.
	 *
	 * nrfx `.frequency` MUST be non-zero even in counter mode — the
	 * driver calls its internal prescaler_calculate() from
	 * nrfx_timer_init() unconditionally (16 MHz base / requested
	 * frequency → prescaler). frequency=0 causes DIV_0_TRP →
	 * K_ERR_ARM_USAGE_DIV_0 → dead. In counter mode the timer counts
	 * external COUNT-task edges so the internal-clock prescaler is
	 * functionally irrelevant to the pulse count — we just need any
	 * legal value here. 1 MHz gives prescaler=4 (matches Zephyr's
	 * nrfx counter examples) and is well inside spec.
	 */
	const nrfx_timer_config_t timer_cfg = {
		.frequency = NRFX_MHZ_TO_HZ(1),
		.mode = NRF_TIMER_MODE_COUNTER,
		.bit_width = NRF_TIMER_BIT_WIDTH_32,
		.interrupt_priority = NRFX_TIMER_DEFAULT_CONFIG_IRQ_PRIORITY,
		.p_context = NULL,
	};

	err = nrfx_timer_init(&counter_timer, &timer_cfg, NULL);
	if (err != NRFX_SUCCESS && err != NRFX_ERROR_ALREADY) {
		LOG_ERR("nrfx_timer_init failed: 0x%08x", err);
		return -EIO;
	}
	nrfx_timer_clear(&counter_timer);

	/* --- LPCOMP: AIN0 (P0.02 = XIAO A0), VDD*3/8 threshold, HYST on.
	 *
	 * Threshold is set via the DT node (`refsel = "VDD_3_8"` in
	 * app.overlay); we override the nrfx default (VDD*4/8) here in
	 * code to match, plus flip detection to UP-only so a full flash
	 * of the meter LED (dark → lit → dark) increments the counter
	 * exactly once. The nrfx default DETECT_CROSS would double-count.
	 *
	 * HYST is the only noise defence in this MVP. A per-pulse
	 * min-width validator (issue #7's acceptance criterion) would
	 * need an LPCOMP ISR that measures width against a TIMER capture
	 * — that's a per-pulse CPU wake, which negates the sleep-current
	 * target. Deferring the filter until bench measurement shows
	 * whether HYST + a shrouded phototransistor is enough; if noise
	 * measurably inflates the count, revisit and add software
	 * width-validation as a follow-up. Documented in the working
	 * doc for #7.
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

	/* --- GPIOTE for the bench D7 falling-edge inject path.
	 *
	 * NRFX_GPIOTE_TRIGGER_HITOLO matches how tools/xiao-pulse.sh drives
	 * the pin from the Pi (pulled up, driven low for ~250 ms per
	 * simulated pulse). No handler registered — the trigger only needs
	 * to fire the underlying GPIOTE IN event so PPI can subscribe.
	 */
	err = nrfx_gpiote_init(&gpiote, NRFX_GPIOTE_DEFAULT_CONFIG_IRQ_PRIORITY);
	if (err != NRFX_SUCCESS && err != NRFX_ERROR_ALREADY) {
		LOG_ERR("nrfx_gpiote_init failed: 0x%08x", err);
		return -EIO;
	}

	static uint8_t bench_gpiote_ch;

	err = nrfx_gpiote_channel_alloc(&gpiote, &bench_gpiote_ch);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_gpiote_channel_alloc failed: 0x%08x", err);
		return -EIO;
	}

	static const nrf_gpio_pin_pull_t bench_pull = NRF_GPIO_PIN_PULLUP;
	static const nrfx_gpiote_trigger_config_t bench_trigger_cfg = {
		.trigger = NRFX_GPIOTE_TRIGGER_HITOLO,
		.p_in_channel = &bench_gpiote_ch,
	};
	static const nrfx_gpiote_input_pin_config_t bench_pin_cfg = {
		.p_pull_config = &bench_pull,
		.p_trigger_config = &bench_trigger_cfg,
		.p_handler_config = NULL,
	};

	err = nrfx_gpiote_input_configure(&gpiote,
					  BENCH_PULSE_ABS_PIN,
					  &bench_pin_cfg);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_gpiote_input_configure failed: 0x%08x", err);
		return -EIO;
	}

	/* `false` = don't unmask the GPIOTE IRQ — we only want the event
	 * for PPI subscription, not a CPU wake per pulse.
	 */
	nrfx_gpiote_trigger_enable(&gpiote, BENCH_PULSE_ABS_PIN, false);

	/* --- PPI wiring: both event sources → TIMER2 COUNT task.
	 *
	 * Two channels because a single PPI channel has one publish
	 * endpoint and one subscribe endpoint; we need two distinct
	 * publishers (LPCOMP UP event, GPIOTE IN event) to fan into the
	 * same subscribe (TIMER2 COUNT task).
	 */
	uint8_t lpcomp_ppi_ch;
	uint8_t gpiote_ppi_ch;

	err = nrfx_gppi_channel_alloc(&lpcomp_ppi_ch);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (LPCOMP) failed: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_gppi_channel_alloc(&gpiote_ppi_ch);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("gppi alloc (GPIOTE) failed: 0x%08x", err);
		return -EIO;
	}

	nrfx_gppi_channel_endpoints_setup(
		lpcomp_ppi_ch,
		nrf_lpcomp_event_address_get(NRF_LPCOMP, NRF_LPCOMP_EVENT_UP),
		nrfx_timer_task_address_get(&counter_timer, NRF_TIMER_TASK_COUNT));

	nrfx_gppi_channel_endpoints_setup(
		gpiote_ppi_ch,
		nrfx_gpiote_in_event_address_get(&gpiote, BENCH_PULSE_ABS_PIN),
		nrfx_timer_task_address_get(&counter_timer, NRF_TIMER_TASK_COUNT));

	nrfx_gppi_channels_enable(BIT(lpcomp_ppi_ch) | BIT(gpiote_ppi_ch));

	/* --- Bring the peripherals live. TIMER first so it's ready to
	 * catch the very first LPCOMP event.
	 */
	nrfx_timer_enable(&counter_timer);

	/* evt_en_mask = 0 keeps LPCOMP IRQ masked; shorts_mask = 0 means
	 * no LPCOMP shortcut is enabled. PPI still publishes the events.
	 */
	nrfx_lpcomp_start(0, 0);

	LOG_INF("hw pulse counter live: LPCOMP AIN0 refsel=VDD_3_8 HYST=on, "
		"bench D7 GPIOTE HITOLO, TIMER2 counter mode");

	return 0;
}

uint32_t hw_pulse_counter_read(void)
{
	return nrfx_timer_capture(&counter_timer, COUNTER_CAPTURE_CH);
}

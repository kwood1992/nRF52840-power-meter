#include "pulse_source_hw.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <nrfx_lpcomp.h>
#include <nrfx_ppi.h>
#include <nrfx_timer.h>

#include "lpcomp_ref.h"

LOG_MODULE_REGISTER(pulse_source_hw, LOG_LEVEL_INF);

/*
 * TIMER instance for pulse counting. TIMER0 is typically claimed by the
 * softdevice / BLE controller, TIMER1 by ncs-zigbee's ZBOSS timer, so
 * TIMER2 is the first freely-available one on nRF52840. Change (and
 * update CONFIG_NRFX_TIMER<N>=y in prj.conf) if that assumption stops
 * holding.
 */
static const nrfx_timer_t counter_timer = NRFX_TIMER_INSTANCE(2);

static nrf_ppi_channel_t ppi_channel;
static bool initialized;

static void lpcomp_evt_handler(nrf_lpcomp_event_t event)
{
	/* We PPI directly from LPCOMP to TIMER — no CPU wake required. The
	 * handler is only here because nrfx_lpcomp_init insists on one;
	 * everything meaningful happens via hardware.
	 */
	ARG_UNUSED(event);
}

int pulse_source_hw_init(uint32_t threshold_mv, uint32_t vdd_mv)
{
	if (initialized) {
		return 0;
	}

	uint8_t step = lpcomp_choose_ref_step_16(threshold_mv, vdd_mv);

	LOG_INF("LPCOMP threshold=%u mV @ VDD=%u mV → step %u/16 (~%u mV)",
		threshold_mv, vdd_mv, step, (unsigned)(vdd_mv * step / 16U));

	/* TIMER in counter mode, 32-bit width. Ticks come from PPI, not from
	 * the internal clock — the frequency setting is ignored in counter
	 * mode but the driver still requires a value.
	 */
	nrfx_timer_config_t timer_cfg = NRFX_TIMER_DEFAULT_CONFIG;

	timer_cfg.mode = NRF_TIMER_MODE_COUNTER;
	timer_cfg.bit_width = NRF_TIMER_BIT_WIDTH_32;
	timer_cfg.frequency = NRF_TIMER_FREQ_1MHz;

	nrfx_err_t err = nrfx_timer_init(&counter_timer, &timer_cfg, NULL);

	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_timer_init: 0x%08x", err);
		return -EIO;
	}
	nrfx_timer_enable(&counter_timer);
	nrfx_timer_clear(&counter_timer);

	/* LPCOMP on AIN0 (P0.02 = XIAO A0), rising-edge detection, internal
	 * VDD/16 reference at the chosen step. Hysteresis on — the hardware
	 * hysteresis band is ~50 mV, small enough that a real meter LED pulse
	 * still triggers reliably but ambient flicker at the threshold
	 * doesn't produce double counts.
	 *
	 * TODO(min-pulse-width-filter): add the two-TIMER hardware filter
	 * per the header comment. Sketch:
	 *   TIMER-measure: shorts { COMPARE0 → STOP + CLEAR }, capture 0 =
	 *     min-pulse-width in µs.
	 *   PPI-A: LPCOMP UP event → TIMER-measure START.
	 *   PPI-B: LPCOMP DOWN event → TIMER-measure STOP + CLEAR.
	 *   PPI-C: TIMER-measure COMPARE0 event → TIMER-count COUNT task.
	 * Uses 3 PPI channels, 1 extra TIMER. Filter runs entirely in HW
	 * during System-ON sleep.
	 */
	nrfx_lpcomp_config_t lpcomp_cfg = {
		.hal = {
			.reference = NRF_LPCOMP_REF_SUPPLY_1_16 + (step - 1),
			.detection = NRF_LPCOMP_DETECT_UP,
			.hyst = NRF_LPCOMP_HYST_ENABLED,
		},
		.input = NRF_LPCOMP_INPUT_0,
		.interrupt_priority = 7,
	};

	err = nrfx_lpcomp_init(&lpcomp_cfg, lpcomp_evt_handler);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_lpcomp_init: 0x%08x", err);
		return -EIO;
	}
	nrfx_lpcomp_enable();

	/* PPI: LPCOMP UP event → TIMER COUNT task. This is what makes the
	 * chain wake-free — the CPU is not involved in incrementing the
	 * counter, only in reading it at report time.
	 */
	err = nrfx_ppi_channel_alloc(&ppi_channel);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_ppi_channel_alloc: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_ppi_channel_assign(
		ppi_channel,
		nrf_lpcomp_event_address_get(NRF_LPCOMP, NRF_LPCOMP_EVENT_UP),
		nrfx_timer_task_address_get(&counter_timer, NRF_TIMER_TASK_COUNT));
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_ppi_channel_assign: 0x%08x", err);
		return -EIO;
	}
	err = nrfx_ppi_channel_enable(ppi_channel);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_ppi_channel_enable: 0x%08x", err);
		return -EIO;
	}

	initialized = true;
	return 0;
}

uint32_t pulse_source_hw_count(void)
{
	if (!initialized) {
		return 0;
	}
	/* Capture channel 0 is unused by the PPI wiring above, so it's safe
	 * as a scratch read of the counter register.
	 */
	return nrfx_timer_capture(&counter_timer, NRF_TIMER_CC_CHANNEL0);
}

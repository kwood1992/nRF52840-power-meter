#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usb_device.h>

#include "button_press_classifier.h"
#include "pulse_accumulator.h"
#include "pulse_source_hw.h"
#include "zigbee_app.h"

/* LPCOMP trigger threshold. Round-trip: LPCOMP internal-reference ladder
 * gives 15 steps of VDD/16, so this value is coerced to the nearest step
 * inside pulse_source_hw_init() — see lpcomp_ref.[ch] for the mapping.
 * 1000 mV @ 3.0 V VDD picks step 5/16 (~937 mV). Tune on the bench with a
 * dark-vs-bright bench reading of the phototransistor.
 */
#define PHOTOTRANSISTOR_THRESHOLD_MV 1000U

/* Nominal VDD for the LPCOMP reference. 2×AAA fresh reads ~3.0 V direct
 * to BAT/VDD (no LDO). If we ever pipe a live SAADC read of VDD in, this
 * becomes dynamic; for now the phototransistor threshold-choice is done
 * once at boot against this constant.
 */
#define VDD_NOMINAL_MV 3000U

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Onboard red LED (P0.26, active-low) used as a boot indicator and
 * heartbeat. Blink pattern doubles as diagnostic if USB doesn't enumerate.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Phototransistor lives on AIN0 = XIAO pin A0 / D0 / P0.02. Wiring is
 * unchanged from the bench setup: thick leg -> 3V3, thin leg -> A0 and
 * -> 47k -> GND. LPCOMP reads it directly (no SAADC in the counting
 * path).
 */

/* User button on XIAO D6 (P1.11), active-low with internal pull-up.
 * Purely a Zigbee join / factory-reset trigger now — the bench
 * pulse-counting role has moved to the LPCOMP hardware chain.
 *
 * Press classification: short (<1 s) → network steering, long (≥3 s)
 * → factory reset. Presses that release in the 1–3 s gap do nothing.
 * See docs/working/2026-07-22-zigbee-join.md for why the gap exists.
 */
static const struct gpio_dt_spec user_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

#define BUTTON_SHORT_MAX_MS    1000U
#define BUTTON_LONG_MIN_MS     3000U

static int64_t button_press_start_ms;
static struct gpio_callback user_button_cb;
static struct button_press_classifier button_classifier;

static K_SEM_DEFINE(button_release_sem, 0, 1);
static atomic_t button_press_duration_ms = ATOMIC_INIT(0);

static void user_button_isr(const struct device *dev,
			    struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();
	int value = gpio_pin_get_dt(&user_button);

	if (value == 1) {
		/* Active — falling edge on the pin, press start. */
		button_press_start_ms = now;
	} else {
		/* Rising edge — press release. Publish the duration
		 * for the main thread to classify + dispatch. Doing
		 * the classification here would be fine but we keep
		 * ZBOSS API calls off the IRQ path.
		 */
		int64_t duration = now - button_press_start_ms;

		if (duration < 0) {
			duration = 0;
		}
		/* atomic_val_t is signed long — clamp to INT32_MAX to
		 * avoid a sign flip in the pathological "held forever"
		 * case. 24 days is well past any real button hold.
		 */
		if (duration > INT32_MAX) {
			duration = INT32_MAX;
		}
		atomic_set(&button_press_duration_ms, (atomic_val_t)duration);
		k_sem_give(&button_release_sem);
	}
}

static void blink(int times, int on_ms, int off_ms)
{
	for (int i = 0; i < times; i++) {
		gpio_pin_set_dt(&led, 1);
		k_sleep(K_MSEC(on_ms));
		gpio_pin_set_dt(&led, 0);
		k_sleep(K_MSEC(off_ms));
	}
}

static void wait_for_host_dtr_or_timeout(const struct device *cdc, int timeout_ms)
{
	uint32_t dtr = 0;
	int elapsed = 0;

	while (!dtr && elapsed < timeout_ms) {
		uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
		elapsed += 100;
	}
}

static int user_button_setup(void)
{
	if (!device_is_ready(user_button.port)) {
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(&user_button, GPIO_INPUT);

	if (err) {
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&user_button, GPIO_INT_EDGE_BOTH);
	if (err) {
		return err;
	}

	gpio_init_callback(&user_button_cb, user_button_isr, BIT(user_button.pin));
	return gpio_add_callback(user_button.port, &user_button_cb);
}

static void button_dispatch_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		k_sem_take(&button_release_sem, K_FOREVER);

		uint32_t duration = (uint32_t)atomic_get(&button_press_duration_ms);
		enum button_press_kind kind =
			button_press_classifier_classify(&button_classifier, duration);

		switch (kind) {
		case BUTTON_PRESS_SHORT:
			LOG_INF("button short-press (%u ms) — joining", duration);
			zigbee_app_start_join();
			break;
		case BUTTON_PRESS_LONG:
			LOG_WRN("button long-press (%u ms) — factory reset", duration);
			zigbee_app_factory_reset();
			break;
		case BUTTON_PRESS_NEITHER:
			LOG_INF("button press (%u ms) ignored — outside short/long band",
				duration);
			break;
		}
	}
}

K_THREAD_DEFINE(button_dispatch_tid, 1024, button_dispatch_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

int main(void)
{
	/* Boot indicator: 4 quick blinks means main() reached. */
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	blink(4, 100, 100);

	const struct device *const cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cdc)) {
		while (1) {
			blink(1, 80, 80);
		}
	}

	/* NCS 2.9.2's USB stack registers a SYS_INIT hook that calls
	 * usb_enable() before main() runs, so a second call here typically
	 * returns -EALREADY. Treat that as success; only bail on a real error.
	 */
	int usb_err = usb_enable(NULL);

	if (usb_err != 0 && usb_err != -EALREADY) {
		while (1) {
			blink(1, 80, 80);
		}
	}

	if (pulse_source_hw_init(PHOTOTRANSISTOR_THRESHOLD_MV, VDD_NOMINAL_MV)) {
		while (1) {
			blink(1, 250, 250);
		}
	}

	if (user_button_setup()) {
		while (1) {
			blink(1, 500, 500);
		}
	}

	/* Hold up to 5 s for a serial monitor to attach so early logs are
	 * visible; then proceed regardless.
	 */
	wait_for_host_dtr_or_timeout(cdc, 5000);

	LOG_INF("XIAO Zigbee Energy Meter booted");
	LOG_INF("LPCOMP+PPI+TIMER pulse chain on AIN0, target threshold=%u mV",
		PHOTOTRANSISTOR_THRESHOLD_MV);
	LOG_INF("user button on D6 (P1.11) — short-press (<1 s) join, long-press (>=3 s) factory reset");

	button_press_classifier_init(&button_classifier,
				     BUTTON_SHORT_MAX_MS,
				     BUTTON_LONG_MIN_MS);

	int zb_err = zigbee_app_init();

	if (zb_err) {
		LOG_ERR("zigbee_app_init failed: %d — continuing without Zigbee",
			zb_err);
	}

	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);

	uint32_t sample = 0;
	uint32_t heartbeat_counter = 0;

	while (1) {
		uint32_t hw_count = pulse_source_hw_count();

		pulse_accumulator_update(&acc, hw_count);

		LOG_INF("sample=%u hw_count=%u accumulator_total=%llu",
			sample++, hw_count,
			(unsigned long long)pulse_accumulator_total(&acc));

		/* Toggle LED every 5 samples = 1 Hz heartbeat. */
		if (++heartbeat_counter >= 5) {
			gpio_pin_toggle_dt(&led);
			heartbeat_counter = 0;
		}

		/* Sample cadence at 1 s. LPCOMP+PPI+TIMER runs during
		 * k_sleep() so no pulses are lost between reads —
		 * previously the ADC path forced a 10 Hz wake to catch
		 * edges, that's no longer required.
		 */
		k_sleep(K_MSEC(1000));
	}

	return 0;
}

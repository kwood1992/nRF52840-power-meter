#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usb_device.h>

#include "pulse_accumulator.h"
#include "pulse_edge_detector.h"

/* Voltage threshold at which the phototransistor is considered "lit" for a
 * meter-LED pulse. Adjust after seeing dark vs. bright readings on the bench.
 * Typical dark on the TEPT4400 with a 47k load reads a few hundred mV; a
 * torch/red LED at close range easily rails past 2 V. 1000 mV is a starting
 * midpoint.
 */
#define PHOTOTRANSISTOR_THRESHOLD_MV 1000

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Onboard red LED (P0.26, active-low) used as a boot indicator and
 * heartbeat. Blink pattern doubles as diagnostic if USB doesn't enumerate.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Phototransistor sampled on AIN0 = XIAO pin A0 / D0 / P0.02.
 * Wiring: thick leg -> 3V3, thin leg -> A0 and -> 47k -> GND.
 */
static const struct adc_dt_spec phototransistor_adc =
	ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

/* User button on XIAO D6 (P1.11), active-low with internal pull-up. This
 * is the long-term Zigbee join / factory-reset button; short-term each
 * press fires a falling-edge IRQ that increments a software pulse counter,
 * which the sample loop feeds into pulse_accumulator_update() — the same
 * way LPCOMP+PPI+TIMER will drive it in the final battery build.
 */
static const struct gpio_dt_spec user_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

#define BENCH_DEBOUNCE_MS 5

static atomic_t bench_pulse_count = ATOMIC_INIT(0);
static int64_t bench_last_edge_ms;
static struct gpio_callback user_button_cb;

static void user_button_pressed(const struct device *dev,
				 struct gpio_callback *cb,
				 uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();

	if (now - bench_last_edge_ms >= BENCH_DEBOUNCE_MS) {
		atomic_inc(&bench_pulse_count);
		bench_last_edge_ms = now;
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

static int read_phototransistor_mv(int32_t *out_mv)
{
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int err = adc_sequence_init_dt(&phototransistor_adc, &seq);

	if (err) {
		return err;
	}

	err = adc_read_dt(&phototransistor_adc, &seq);
	if (err) {
		return err;
	}

	int32_t mv = raw;

	err = adc_raw_to_millivolts_dt(&phototransistor_adc, &mv);
	if (err) {
		return err;
	}

	*out_mv = mv;
	return 0;
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

	err = gpio_pin_interrupt_configure_dt(&user_button, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		return err;
	}

	gpio_init_callback(&user_button_cb, user_button_pressed, BIT(user_button.pin));
	return gpio_add_callback(user_button.port, &user_button_cb);
}

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

	if (!adc_is_ready_dt(&phototransistor_adc) ||
	    adc_channel_setup_dt(&phototransistor_adc)) {
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
	LOG_INF("sampling phototransistor on A0 (AIN0) at 10 Hz, threshold=%d mV",
		PHOTOTRANSISTOR_THRESHOLD_MV);
	LOG_INF("user button on D6 (P1.11) — press to increment accumulator");

	struct pulse_accumulator acc;
	struct pulse_edge_detector detector;

	pulse_accumulator_init(&acc);
	pulse_edge_detector_init(&detector, PHOTOTRANSISTOR_THRESHOLD_MV);

	uint32_t sample = 0;
	uint32_t heartbeat_counter = 0;

	while (1) {
		int32_t mv = 0;
		int err = read_phototransistor_mv(&mv);

		if (!err && pulse_edge_detector_sample(&detector, mv)) {
			atomic_inc(&bench_pulse_count);
		}

		uint32_t pulses = (uint32_t)atomic_get(&bench_pulse_count);

		pulse_accumulator_update(&acc, pulses);

		if (err) {
			LOG_ERR("adc read failed: %d", err);
		} else {
			LOG_INF("sample=%u voltage_mv=%d accumulator_total=%llu",
				sample++, mv,
				(unsigned long long)pulse_accumulator_total(&acc));
		}

		/* Toggle LED every 5 samples = 1 Hz heartbeat. */
		if (++heartbeat_counter >= 5) {
			gpio_pin_toggle_dt(&led);
			heartbeat_counter = 0;
		}

		k_sleep(K_MSEC(100));
	}

	return 0;
}

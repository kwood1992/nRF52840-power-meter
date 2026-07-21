#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include "pulse_accumulator.h"

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

	/* Hold up to 5 s for a serial monitor to attach so early logs are
	 * visible; then proceed regardless.
	 */
	wait_for_host_dtr_or_timeout(cdc, 5000);

	LOG_INF("XIAO Zigbee Energy Meter booted");
	LOG_INF("sampling phototransistor on A0 (AIN0) at 10 Hz");

	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);

	uint32_t sample = 0;
	uint32_t heartbeat_counter = 0;

	while (1) {
		int32_t mv = 0;
		int err = read_phototransistor_mv(&mv);

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

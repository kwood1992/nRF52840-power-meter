#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usb_device.h>

#include "button_press_classifier.h"
#include "nvs_store.h"
#include "persist_policy.h"
#include "pulse_accumulator.h"
#include "pulse_edge_detector.h"
#include "report_gate.h"
#include "zigbee_app.h"

/* Voltage threshold at which the phototransistor is considered "lit" for a
 * meter-LED pulse. Adjust after seeing dark vs. bright readings on the bench.
 * Typical dark on the TEPT4400 with a 47k load reads a few hundred mV; a
 * torch/red LED at close range easily rails past 2 V. 1000 mV is a starting
 * midpoint.
 */
#define PHOTOTRANSISTOR_THRESHOLD_MV 1000

/* Persistence policy: write the accumulator total to NVS on whichever of
 * these fires first —
 *   * every 5 minutes of wall-clock (matches the Zigbee report cadence
 *     from issue #5, so one wake pays for both persistence and report);
 *   * every 100 counted pulses since the last save (safety net that bounds
 *     data loss under a burst — 100 pulses at the default 1000 imp/kWh
 *     is 0.1 kWh, a tolerable worst case).
 */
#define PERSIST_INTERVAL_MS      (5U * 60U * 1000U)
#define PERSIST_MAX_PULSE_DELTA  100ULL

/* Zigbee Metering-cluster report cadence. Matches the design doc's
 * 5-min target and is the cadence Home Assistant's Energy Dashboard
 * expects for a "sum of energy over 5 min" datapoint. On a proper
 * low-power build (issue #6/#7) this k_work_delayable becomes the RTC
 * wake source; today's non-suspending build fires the work at wall
 * clock cadence, which is functionally identical for Z2M's purposes.
 */
#define ZIGBEE_REPORT_INTERVAL_MS  (5U * 60U * 1000U)

/* Force an explicit ZCL Report Attributes frame every N per-pulse
 * publishes, in addition to the reporting engine's own delta trigger
 * and the 5-min wall-clock tick. Belt-and-braces for issue #20 — a
 * stale coordinator-side binding surfaces within N pulses (~100 s at
 * the typical bench 1 Hz injection rate) instead of at the 65000 s
 * max_interval force-fire. N chosen to match Z2M's default
 * reportable_change of 100, so the explicit path and the delta path
 * converge on the same worst-case cadence.
 */
#define ZIGBEE_REPORT_PULSE_HEARTBEAT  100U

/* Button press bands. Anything in the 1–3 s gap is ignored to avoid
 * accidental factory-resets when the user meant "short press". See
 * docs/working/2026-07-22-zigbee-join.md for why the gap exists.
 */
#define BUTTON_SHORT_MAX_MS    1000U
#define BUTTON_LONG_MIN_MS     3000U

/* Boot-hold accumulator erase: sw0 held continuously for this long at
 * boot wipes the NVS-persisted total to 0. Covers the redeployment case
 * (moving the sensor to a different physical meter). Confirmation is
 * BOOT_ERASE_BLINK_COUNT rapid red-LED blinks so the user has visible
 * feedback without needing serial. See
 * docs/working/2026-07-24-boot-hold-erase.md for the gesture rationale
 * and its relationship to the post-boot short/long-press bands.
 */
#define BOOT_ERASE_HOLD_MS       3000U
#define BOOT_ERASE_POLL_MS       10U
#define BOOT_ERASE_BLINK_COUNT   4
#define BOOT_ERASE_BLINK_MS      150

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

/* User button on XIAO D6 (P1.11), active-low with internal pull-up.
 * Zigbee join (short-press) / factory-reset (long-press) only. Pulse
 * counting moved off this pin in #16 onto D7 (see pulse_input below) so
 * the bench rig can inject pulses without triggering the join callback
 * on every simulated pulse.
 */
static const struct gpio_dt_spec user_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Dedicated bench pulse-simulator input on XIAO D7 (P1.12), active-low
 * with internal pull-up. Every falling edge bumps bench_pulse_count.
 * No debounce, no ZBOSS-touching side effects — Pi's ~/xiao-pulse.sh
 * generates clean pulses (~250 ms LOW), so on-MCU debounce would
 * suppress legitimate injections. Retired by #7's LPCOMP chain.
 */
static const struct gpio_dt_spec pulse_input = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);

static atomic_t bench_pulse_count = ATOMIC_INIT(0);
static int64_t button_press_start_ms;
static struct gpio_callback user_button_cb;
static struct gpio_callback pulse_input_cb;
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
		/* Active — falling edge on the pin, i.e. press start. */
		button_press_start_ms = now;
	} else {
		/* Rising edge — press release. Publish the duration
		 * for the dispatch thread to classify. Keeping ZBOSS
		 * calls off the IRQ path.
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

static void pulse_input_isr(const struct device *dev,
			    struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	atomic_inc(&bench_pulse_count);
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

/* Split from user_button_arm_irq() so main() can poll sw0 for the
 * boot-hold accumulator-erase gesture BEFORE the edge interrupt goes
 * live. If the IRQ were armed first, a "held at boot" state would
 * synthesize a spurious release event on eventual let-go and the
 * button classifier would misread the boot-hold as a long-press
 * factory-reset.
 */
static int user_button_configure(void)
{
	if (!device_is_ready(user_button.port)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&user_button, GPIO_INPUT);
}

static int user_button_arm_irq(void)
{
	int err = gpio_pin_interrupt_configure_dt(&user_button, GPIO_INT_EDGE_BOTH);

	if (err) {
		return err;
	}

	gpio_init_callback(&user_button_cb, user_button_isr, BIT(user_button.pin));
	return gpio_add_callback(user_button.port, &user_button_cb);
}

static bool boot_button_held(uint32_t threshold_ms)
{
	uint32_t elapsed = 0;

	while (elapsed < threshold_ms) {
		if (gpio_pin_get_dt(&user_button) != 1) {
			return false;
		}
		k_sleep(K_MSEC(BOOT_ERASE_POLL_MS));
		elapsed += BOOT_ERASE_POLL_MS;
	}
	return true;
}

static int pulse_input_setup(void)
{
	if (!device_is_ready(pulse_input.port)) {
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(&pulse_input, GPIO_INPUT);

	if (err) {
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&pulse_input, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		return err;
	}

	gpio_init_callback(&pulse_input_cb, pulse_input_isr, BIT(pulse_input.pin));
	return gpio_add_callback(pulse_input.port, &pulse_input_cb);
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

/* 5-minute Zigbee metering-report tick. Kept in the shared struct
 * pulse_accumulator (declared in main() below) via the file-scope
 * pointer so the work handler can read the live total without a
 * separate synchronization primitive — pulse_accumulator_total() is
 * atomic in practice for our access pattern (single writer, single
 * reader, uint64_t read on ARM is not tearable on 32-bit boundaries
 * but the sample loop and this work run on different threads so the
 * odd stale read is acceptable; the next tick corrects it).
 */
static struct pulse_accumulator *report_accumulator;

static void metering_report_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(metering_report_work,
			       metering_report_work_handler);

static void metering_report_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (report_accumulator == NULL) {
		return;
	}

	uint64_t total = pulse_accumulator_total(report_accumulator);

	LOG_INF("metering report tick: CurrentSummationDelivered=%llu",
		(unsigned long long)total);
	/* Force an explicit report frame on the 5-min tick. Without
	 * this the ZBOSS reporting engine only emits when the delta
	 * since last report crosses `reportable_change` (default 100
	 * from Z2M) — so a slow trickle of pulses can leave Z2M
	 * without a datapoint for hours (up to max_interval = 65000 s).
	 * The design-doc contract is "one report per 5 min"; this
	 * closes that gap. See #20.
	 */
	zigbee_app_publish_summation_and_report(total);

	k_work_reschedule(&metering_report_work,
			  K_MSEC(ZIGBEE_REPORT_INTERVAL_MS));
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

	if (user_button_configure()) {
		while (1) {
			blink(1, 500, 500);
		}
	}

	if (pulse_input_setup()) {
		while (1) {
			blink(1, 500, 500);
		}
	}

	/* NVS + boot-hold gesture check runs BEFORE wait_for_host_dtr_or_timeout
	 * and zigbee_app_init so the total delay from power-on to the point the
	 * hold poll starts stays around 1 s (Zephyr init + boot blink + GPIO
	 * configure). Any later placement compounds with the 5 s DTR wait on a
	 * battery-powered boot with no serial host and forces the user to hold
	 * the button for 8-10 s to trigger the erase — well past the 3 s spec
	 * in issue #14. Cost of the early placement: the "restored…" /
	 * "accumulator erased" LOG_INFs fire before DTR asserts, so a monitor
	 * that attaches after boot won't see them in the live stream (Zephyr's
	 * log backend still buffers them for the current session).
	 */
	struct pulse_accumulator acc;
	struct pulse_edge_detector detector;
	struct persist_policy policy;
	struct report_gate pulse_heartbeat;

	pulse_accumulator_init(&acc);
	pulse_edge_detector_init(&detector, PHOTOTRANSISTOR_THRESHOLD_MV);
	persist_policy_init(&policy, PERSIST_INTERVAL_MS, PERSIST_MAX_PULSE_DELTA);
	report_gate_init(&pulse_heartbeat, ZIGBEE_REPORT_PULSE_HEARTBEAT);

	uint64_t last_saved_total = 0;
	/* Backdate last_saved_ms by the full persist interval so the *first*
	 * time the total changes after boot triggers an immediate write via
	 * the wall-clock arm of persist_policy_should_write(). Without this,
	 * a bench cycle of "press N<100 times, reboot within 5 min" never
	 * fires either safety net and NVS stays empty — indistinguishable
	 * from a broken mount from the serial log.
	 */
	int64_t last_saved_ms = k_uptime_get() - (int64_t)PERSIST_INTERVAL_MS;

	int nvs_err = nvs_store_init();

	if (nvs_err) {
		LOG_ERR("nvs_store_init failed: %d — proceeding without persistence",
			nvs_err);
	} else {
		/* Erase-before-load: check the boot-hold gesture, overwrite
		 * the accumulator record with 0 if held, then let load_total
		 * run normally. After a successful erase the load returns 0
		 * and the sample loop's restore path logs
		 * "restored accumulator_total=0 from NVS" — indistinguishable
		 * from a device that legitimately reached total=0.
		 */
		if (boot_button_held(BOOT_ERASE_HOLD_MS)) {
			int erase_err = nvs_store_save_total(0);

			if (erase_err) {
				LOG_ERR("boot-hold accumulator erase failed: %d",
					erase_err);
			} else {
				LOG_INF("accumulator erased by boot-hold");
				blink(BOOT_ERASE_BLINK_COUNT,
				      BOOT_ERASE_BLINK_MS,
				      BOOT_ERASE_BLINK_MS);
			}
		}

		uint64_t saved = 0;
		int load_err = nvs_store_load_total(&saved);

		if (load_err == 0) {
			pulse_accumulator_restore(&acc, saved);
			last_saved_total = saved;
			LOG_INF("restored accumulator_total=%llu from NVS",
				(unsigned long long)saved);
		} else if (load_err == -ENOENT) {
			LOG_INF("no persisted accumulator total — cold boot");
		} else {
			LOG_ERR("nvs_store_load_total failed: %d", load_err);
		}
	}

	/* Hold up to 5 s for a serial monitor to attach so early logs are
	 * visible; then proceed regardless.
	 */
	wait_for_host_dtr_or_timeout(cdc, 5000);

	LOG_INF("XIAO Zigbee Energy Meter booted");
	LOG_INF("sampling phototransistor on A0 (AIN0) at 10 Hz, threshold=%d mV",
		PHOTOTRANSISTOR_THRESHOLD_MV);
	LOG_INF("user button on D6 (P1.11) — short-press (<1 s) join, long-press (>=3 s) factory reset");
	LOG_INF("bench pulse input on D7 (P1.12) — falling-edge = 1 pulse (no debounce, no join callback)");
	LOG_INF("boot-hold: sw0 held for %u ms at boot erases the NVS accumulator (4× LED confirm)",
		BOOT_ERASE_HOLD_MS);

	button_press_classifier_init(&button_classifier,
				     BUTTON_SHORT_MAX_MS,
				     BUTTON_LONG_MIN_MS);

	int zb_err = zigbee_app_init();

	if (zb_err) {
		LOG_ERR("zigbee_app_init failed: %d — continuing without Zigbee",
			zb_err);
	}

	/* Arm the sw0 edge interrupt only now that the boot-hold poll AND
	 * zigbee_app_init have run. Ordering matters: user_button_isr posts
	 * short-press events that the dispatch thread hands to
	 * zigbee_app_start_join(), which reaches into ZBOSS internals; that
	 * whole call chain relies on the classifier being initialised and
	 * ZBOSS having been brought up first.
	 */
	if (user_button_arm_irq()) {
		while (1) {
			blink(1, 500, 500);
		}
	}

	uint32_t sample = 0;
	uint32_t heartbeat_counter = 0;

	/* Publish the just-restored total once so Z2M sees a live
	 * value on the very first read, even before the first pulse
	 * lands or the 5-min report tick fires. Also arm the 5-min
	 * report cadence.
	 */
	report_accumulator = &acc;
	uint64_t last_published_total = pulse_accumulator_total(&acc);

	zigbee_app_publish_summation(last_published_total);
	k_work_reschedule(&metering_report_work,
			  K_MSEC(ZIGBEE_REPORT_INTERVAL_MS));

	while (1) {
		int32_t mv = 0;
		int err = read_phototransistor_mv(&mv);

		bool edge = !err && pulse_edge_detector_sample(&detector, mv);

		if (edge) {
			atomic_inc(&bench_pulse_count);
		}

		uint32_t pulses = (uint32_t)atomic_get(&bench_pulse_count);

		pulse_accumulator_update(&acc, pulses);

		uint64_t total = pulse_accumulator_total(&acc);

		if (total != last_published_total) {
			/* Push the fresh total into the Metering attribute
			 * so a manual Z2M read returns live data without
			 * waiting for the 5-min tick. Fires on both ADC-
			 * edge counts AND button-triggered bench pulses
			 * (the button ISR bumps bench_pulse_count directly,
			 * which advances the accumulator on the next sample
			 * iteration even though `edge` is false here).
			 *
			 * Every Nth per-pulse publish also forces an explicit
			 * report frame so a broken coordinator-side binding
			 * (issue #20) surfaces on the bench inside ~N pulses
			 * instead of only at the 5-min tick.
			 */
			if (report_gate_advance(&pulse_heartbeat)) {
				zigbee_app_publish_summation_and_report(total);
			} else {
				zigbee_app_publish_summation(total);
			}
			last_published_total = total;
		}

		if (err) {
			LOG_ERR("adc read failed: %d", err);
		} else if (edge) {
			/* Only log when a real pulse is counted. Per-sample
			 * logs at 10 Hz were flooding the serial and drowning
			 * out Zigbee events during commissioning.
			 */
			LOG_INF("pulse counted: voltage_mv=%d accumulator_total=%llu",
				mv,
				(unsigned long long)pulse_accumulator_total(&acc));
		}
		sample++;

		int64_t now = k_uptime_get();

		if (persist_policy_should_write(&policy, total, last_saved_total,
						(uint64_t)(now - last_saved_ms))) {
			int save_err = nvs_store_save_total(total);

			if (save_err) {
				LOG_ERR("nvs_store_save_total failed: %d", save_err);
			} else {
				last_saved_total = total;
				last_saved_ms = now;
				LOG_INF("persisted accumulator_total=%llu",
					(unsigned long long)total);
			}
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

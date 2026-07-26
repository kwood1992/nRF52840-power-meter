#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usb_device.h>

#include "button_press_classifier.h"
#include "hw_pulse_counter.h"
#include "led_controller.h"
#include "nvs_store.h"
#include "persist_policy.h"
#include "pulse_accumulator.h"
#include "report_gate.h"
#include "zigbee_app.h"

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
 * low-power build (issue #8) this k_work_delayable becomes the RTC
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
 * (moving the sensor to a different physical meter). Confirmation is a
 * 4× red blink pattern requested through led_controller — same
 * ERASE_CONFIRM pattern the runtime factory-reset path uses (#30). See
 * docs/working/2026-07-24-boot-hold-erase.md for the gesture rationale
 * and its relationship to the post-boot short/long-press bands.
 */
#define BOOT_ERASE_HOLD_MS       3000U
#define BOOT_ERASE_POLL_MS       10U

/* Wake cadence for the sample / persist / heartbeat loop. Pulse counting
 * is now hardware (LPCOMP + PPI + TIMER2) so this loop no longer needs
 * to be a Nyquist-margin oversampler of the phototransistor waveform —
 * it just needs to service the persist policy and blink the LED. 200 ms
 * keeps the LED heartbeat visible (~1 Hz with a toggle every 5 wakes)
 * and bounds "how long a fresh pulse waits before Z2M sees it" to well
 * under a second. Cadence tightens further in #8 when the loop becomes
 * an RTC-driven wake instead of a k_sleep.
 */
#define SAMPLE_LOOP_INTERVAL_MS   200U
#define HEARTBEAT_TOGGLE_EVERY    5U

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Onboard red LED (P0.26, active-low) used as a boot indicator and
 * heartbeat. Blink pattern doubles as diagnostic if USB doesn't enumerate.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* User button on XIAO D6 (P1.11), active-low with internal pull-up.
 * Zigbee join (short-press) / factory-reset (long-press) only. Pulse
 * counting lives entirely in hw_pulse_counter.c now — this pin has no
 * involvement in the counter path.
 */
static const struct gpio_dt_spec user_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static int64_t button_press_start_ms;
static struct gpio_callback user_button_cb;
static struct button_press_classifier button_classifier;

static K_SEM_DEFINE(button_release_sem, 0, 1);
static atomic_t button_press_duration_ms = ATOMIC_INIT(0);

/* Fires 3 s after the button falling edge if the press is still held.
 * Requests LONG_PRESS_HOLD so the LED lights solid red the moment the
 * hold crosses the factory-reset threshold — closes the "am I past 3 s
 * yet?" gap that had the user releasing in the 1–3 s dead zone or
 * overshooting without feedback (#30). Cancelled on the rising edge
 * whether the hold made it to 3 s or not.
 */
static void long_press_hold_arm_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(long_press_hold_arm,
			       long_press_hold_arm_handler);

static void long_press_hold_arm_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	led_request(LED_PATTERN_LONG_PRESS_HOLD, LED_PRIO_LONG_PRESS_HOLD);
}

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
		/* Active — falling edge on the pin, i.e. press start.
		 * Arm the 3 s "held long enough for factory-reset" LED
		 * so the user gets feedback the moment they cross the
		 * threshold (#30).
		 */
		button_press_start_ms = now;
		k_work_reschedule(&long_press_hold_arm, K_MSEC(BUTTON_LONG_MIN_MS));
	} else {
		/* Rising edge — press release. Cancel the pending
		 * long-press-hold LED arm and turn off any LED it may
		 * have already lit. Then publish the duration for the
		 * dispatch thread to classify, keeping ZBOSS calls off
		 * the IRQ path.
		 */
		k_work_cancel_delayable(&long_press_hold_arm);
		led_cancel(LED_PATTERN_LONG_PRESS_HOLD);

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
	/* Hold crossed the threshold — commit to the erase visually so
	 * a still-pressing user gets the "committed" signal (#30). The
	 * caller cancels LONG_PRESS_HOLD before transitioning to
	 * ERASE_CONFIRM.
	 */
	led_request(LED_PATTERN_LONG_PRESS_HOLD, LED_PRIO_LONG_PRESS_HOLD);
	return true;
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
			led_request(LED_PATTERN_BUTTON_ACK, LED_PRIO_BUTTON_ACK);
			zigbee_app_start_join();
			break;
		case BUTTON_PRESS_LONG:
			LOG_WRN("button long-press (%u ms) — factory reset", duration);
			zigbee_app_factory_reset();
			led_request(LED_PATTERN_ERASE_CONFIRM,
				    LED_PRIO_ERASE_CONFIRM);
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

	/* Optional 50 ms green flash so a bench user watching the
	 * device can see the 5-min report cadence at a glance (#33).
	 * Silent by default; turned on by dev.conf.
	 */
#if IS_ENABLED(CONFIG_APP_REPORT_HEARTBEAT)
	led_request(LED_PATTERN_HEARTBEAT, LED_PRIO_HEARTBEAT);
#endif
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
	/* Configure the red LED for its remaining direct-poke consumers
	 * (the 1 Hz sample-loop toggle and the "led_controller_init
	 * failed" fatal fallback). led_controller_init also reconfigures
	 * this pin along with green and blue; the double-configure is
	 * harmless.
	 */
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	const struct device *const cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cdc)) {
		/* Category 1 = CDC-ACM device_not_ready (see the flash-
		 * count → failure-site mapping in led_controller.h).
		 */
		led_request_fatal(1);
		while (1) {
			k_sleep(K_FOREVER);
		}
	}

	/* NCS 2.9.2's USB stack registers a SYS_INIT hook that calls
	 * usb_enable() before main() runs, so a second call here typically
	 * returns -EALREADY. Treat that as success; only bail on a real error.
	 */
	int usb_err = usb_enable(NULL);

	if (usb_err != 0 && usb_err != -EALREADY) {
		/* Category 2 = usb_enable failed. */
		led_request_fatal(2);
		while (1) {
			k_sleep(K_FOREVER);
		}
	}

	if (user_button_configure()) {
		/* Category 3 = user_button_configure failed. */
		led_request_fatal(3);
		while (1) {
			k_sleep(K_FOREVER);
		}
	}

	if (led_controller_init()) {
		/* Predates #32 — this is the one fatal path that can't
		 * go through led_request_fatal because the LED
		 * controller itself is the thing that failed. Kept as
		 * a direct blink(1, 750, 750) on the red LED main.c
		 * already has a handle for; distinct rate so the bench
		 * observer can tell it apart from the migrated fatals.
		 */
		while (1) {
			blink(1, 750, 750);
		}
	}

	/* Optional 100 ms white boot flash (#33). Silent by default so
	 * battery deployments emit no LED at boot; USB dev builds turn
	 * this on via dev.conf. Reuses BUTTON_ACK's renderer — same
	 * shape (all-LEDs-on, 100 ms one-shot).
	 */
#if IS_ENABLED(CONFIG_APP_BOOT_FLASH)
	led_request(LED_PATTERN_BUTTON_ACK, LED_PRIO_BUTTON_ACK);
#endif

	int hw_err = hw_pulse_counter_init();

	if (hw_err) {
		LOG_ERR("hw_pulse_counter_init failed: %d — pulse counting will not work",
			hw_err);
		/* Category 4 = hw_pulse_counter_init failed. */
		led_request_fatal(4);
		while (1) {
			k_sleep(K_FOREVER);
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
	struct persist_policy policy;
	struct report_gate pulse_heartbeat;

	pulse_accumulator_init(&acc);
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

			/* Regardless of erase outcome, the solid-red
			 * LONG_PRESS_HOLD indicator has done its job —
			 * turn it off so the ERASE_CONFIRM pattern (or
			 * nothing, on failure) can render.
			 */
			led_cancel(LED_PATTERN_LONG_PRESS_HOLD);

			if (erase_err) {
				LOG_ERR("boot-hold accumulator erase failed: %d",
					erase_err);
			} else {
				LOG_INF("accumulator erased by boot-hold");
				led_request(LED_PATTERN_ERASE_CONFIRM,
					    LED_PRIO_ERASE_CONFIRM);
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
	LOG_INF("pulse counting: phototransistor on A0 (AIN0) → LPCOMP → PPI → TIMER2, "
		"threshold=VDD*3/8 (HYST on)");
	LOG_INF("user button on D6 (P1.11) — short-press (<1 s) join, long-press (>=3 s) factory reset");
	LOG_INF("bench pulse input on D7 (P1.12) — falling-edge via GPIOTE→PPI→TIMER2 (no debounce)");
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
		/* Category 5 = user_button_arm_irq failed. */
		led_request_fatal(5);
		while (1) {
			k_sleep(K_FOREVER);
		}
	}

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
		/* Snapshot the hardware counter. LPCOMP-driven and GPIOTE
		 * (bench) events both feed TIMER2 via PPI, so this reads
		 * the union of "real meter pulses" + "bench simulator
		 * pulses" — no separate atomic to merge in.
		 */
		uint32_t pulses = hw_pulse_counter_read();

		pulse_accumulator_update(&acc, pulses);

		uint64_t total = pulse_accumulator_total(&acc);

		if (total != last_published_total) {
			uint64_t delta = total - last_published_total;

			/* Push the fresh total into the Metering attribute
			 * so a manual Z2M read returns live data without
			 * waiting for the 5-min tick.
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
			LOG_INF("pulse(s) counted: delta=%llu accumulator_total=%llu",
				(unsigned long long)delta,
				(unsigned long long)total);
			last_published_total = total;
		}

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

		if (++heartbeat_counter >= HEARTBEAT_TOGGLE_EVERY) {
			gpio_pin_toggle_dt(&led);
			heartbeat_counter = 0;
		}

		k_sleep(K_MSEC(SAMPLE_LOOP_INTERVAL_MS));
	}

	return 0;
}

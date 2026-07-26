#include "led_controller.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

LOG_MODULE_REGISTER(led_controller, LOG_LEVEL_INF);

/* All three onboard LEDs are active-low per xiao_ble_common.dtsi (red
 * P0.26, green P0.30, blue P0.6). Consumers use gpio_pin_set_dt with
 * logical values (1 = on, 0 = off); the DT_ACTIVE_LOW flag flips the
 * physical level for us.
 */
static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static struct led_priority_state state;
static struct k_spinlock lock;
static enum led_pattern_id rendered = LED_PATTERN_NONE;

/* Pattern-specific renderer state. All mutated under `lock`; both the
 * request/cancel wrappers and the shared tick handler take the same
 * spinlock, so no atomics needed.
 */
static bool     joining_phase_on;
static uint32_t join_fail_ticks_remaining;
static uint32_t erase_confirm_ticks_remaining;
static bool     identify_phase_on;
/* FATAL renderer state — see led_request_fatal in led_controller.h for
 * the flash-count → failure-site mapping. `fatal_flash_count` is 1..5.
 * `fatal_phase_index` counts modulo 2 * flash_count during the pre-
 * collapse window, then modulo 2 after the 10-min collapse (single
 * flash + long off).
 */
static uint32_t fatal_flash_count;
static uint32_t fatal_phase_index;
static int64_t  fatal_start_ms;

/* Single shared tick — only one pattern renders at a time, so one work
 * item is enough. Idle transitions cancel the work; the workqueue's
 * cancel is safe under spinlock (returns state flags, never blocks).
 */
static void tick_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_handler);

#define BUTTON_ACK_ON_MS       100
#define JOINING_HALF_PERIOD_MS 500
#define JOIN_SUCCESS_ON_MS     2000
#define JOIN_FAIL_HALF_PERIOD_MS 200
/* 3 blinks = 6 phase transitions total. Renderer starts with red on
 * (the 1st transition); five ticks toggle through off/on/off/on/off,
 * ending at the natural "all dark" resting state before cancelling.
 */
#define JOIN_FAIL_TICKS        5
/* 4 blinks — matches the pre-#30 direct blink(4, 150, 150) call and the
 * boot-hold erase confirmation the user already recognises. 4 on-phases
 * + 4 off-phases = 8 total; the initial "red on" at render start is
 * one of them, so seven ticks toggle through the remaining seven phase
 * transitions.
 */
#define ERASE_CONFIRM_HALF_PERIOD_MS 150
#define ERASE_CONFIRM_TICKS          7
/* Magenta = red + blue simultaneously. Chosen deliberately to avoid
 * collision with any other pattern's colour (#31): red alone reads as
 * fault/reset, green alone reads as joined, blue alone reads as
 * joining, white reads as button-ack — magenta is unambiguous.
 */
#define IDENTIFY_HALF_PERIOD_MS      200
/* Fatal pattern shape: N pulses of 100 ms on with 100 ms off between
 * them, then a long tail-off that stretches the cycle to 5 s total.
 * Keeps duty ≤ 10 % for every N in 1..5 (worst case N=5 → 500 ms on
 * / 5000 ms period = exactly 10 %).
 */
#define FATAL_PULSE_ON_MS            100
#define FATAL_PULSE_OFF_MS           100
#define FATAL_CYCLE_MS               5000
/* After this long in the fatal loop, collapse to a single 100 ms
 * flash every 10 s (~1 % duty) so the failure indicator survives
 * long enough to be noticed weeks later on a field visit.
 */
#define FATAL_COLLAPSE_AFTER_MS      (10 * 60 * 1000)
#define FATAL_COLLAPSE_ON_MS         100
#define FATAL_COLLAPSE_OFF_MS        9900

static void set_leds_rgb(int r, int g, int b)
{
	gpio_pin_set_dt(&led_red,   r);
	gpio_pin_set_dt(&led_green, g);
	gpio_pin_set_dt(&led_blue,  b);
}

static void set_all_leds(int on)
{
	set_leds_rgb(on, on, on);
}

static void render_start_locked(enum led_pattern_id pattern)
{
	/* Any transition cancels the previous pattern's pending tick.
	 * A stale tick that lands after we've moved on will find
	 * `rendered` no longer matches its case and no-op. Safe.
	 */
	k_work_cancel_delayable(&tick_work);

	switch (pattern) {
	case LED_PATTERN_BUTTON_ACK:
		set_all_leds(1);
		k_work_reschedule(&tick_work, K_MSEC(BUTTON_ACK_ON_MS));
		break;

	case LED_PATTERN_JOINING:
		joining_phase_on = true;
		set_leds_rgb(0, 0, 1);
		k_work_reschedule(&tick_work, K_MSEC(JOINING_HALF_PERIOD_MS));
		break;

	case LED_PATTERN_JOIN_SUCCESS:
		set_leds_rgb(0, 1, 0);
		k_work_reschedule(&tick_work, K_MSEC(JOIN_SUCCESS_ON_MS));
		break;

	case LED_PATTERN_JOIN_FAIL:
		join_fail_ticks_remaining = JOIN_FAIL_TICKS;
		set_leds_rgb(1, 0, 0);
		k_work_reschedule(&tick_work, K_MSEC(JOIN_FAIL_HALF_PERIOD_MS));
		break;

	case LED_PATTERN_LONG_PRESS_HOLD:
		/* Solid red — no tick needed. Held until the caller
		 * cancels (rising edge in the button ISR, or the boot
		 * path's transition into the erase confirm pattern).
		 */
		set_leds_rgb(1, 0, 0);
		break;

	case LED_PATTERN_IDENTIFY:
		/* Continuous 200/200 ms magenta blink. Runs until the
		 * ZCL identify duration elapses (coordinator sends
		 * cancel with bufid == NULL, which becomes
		 * led_cancel(IDENTIFY) in zigbee_app.c).
		 */
		identify_phase_on = true;
		set_leds_rgb(1, 0, 1);
		k_work_reschedule(&tick_work, K_MSEC(IDENTIFY_HALF_PERIOD_MS));
		break;

	case LED_PATTERN_ERASE_CONFIRM:
		erase_confirm_ticks_remaining = ERASE_CONFIRM_TICKS;
		set_leds_rgb(1, 0, 0);
		k_work_reschedule(&tick_work,
				  K_MSEC(ERASE_CONFIRM_HALF_PERIOD_MS));
		break;

	case LED_PATTERN_FATAL:
		/* Enter at phase 0 = first pulse on. Tick handler
		 * advances phase_index and picks pulse-on / pulse-off /
		 * long-tail from it, and also handles the 10-min
		 * collapse. fatal_flash_count and fatal_start_ms are
		 * populated by led_request_fatal before the reselect
		 * lands us here.
		 */
		fatal_phase_index = 0;
		set_leds_rgb(1, 0, 0);
		k_work_reschedule(&tick_work, K_MSEC(FATAL_PULSE_ON_MS));
		break;

	case LED_PATTERN_NONE:
		set_all_leds(0);
		break;

	default:
		/* Dispatchable but no renderer yet (long-press hold,
		 * identify, erase confirm, fatal, heartbeat — see
		 * follow-up issues). Keep LEDs off; the priority core
		 * still records the request so preemption stays
		 * observable.
		 */
		set_all_leds(0);
		break;
	}
}

static void reselect_if_changed_locked(void)
{
	enum led_pattern_id selected = led_priority_selected(&state);

	if (selected == rendered) {
		return;
	}
	rendered = selected;
	render_start_locked(selected);
}

int led_controller_init(void)
{
	if (!device_is_ready(led_red.port) ||
	    !device_is_ready(led_green.port) ||
	    !device_is_ready(led_blue.port)) {
		return -ENODEV;
	}

	int err;

	err = gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}
	err = gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	led_priority_init(&state);
	rendered = LED_PATTERN_NONE;

	return 0;
}

void led_request(enum led_pattern_id pattern, enum led_priority prio)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	led_priority_request(&state, pattern, prio);
	reselect_if_changed_locked();

	k_spin_unlock(&lock, key);
}

void led_cancel(enum led_pattern_id pattern)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	led_priority_cancel(&state, pattern);
	reselect_if_changed_locked();

	k_spin_unlock(&lock, key);
}

void led_request_fatal(uint32_t flash_count)
{
	if (flash_count < 1) {
		flash_count = 1;
	} else if (flash_count > 5) {
		flash_count = 5;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	fatal_flash_count = flash_count;
	fatal_start_ms = k_uptime_get();
	led_priority_request(&state, LED_PATTERN_FATAL, LED_PRIO_FATAL);
	reselect_if_changed_locked();

	k_spin_unlock(&lock, key);
}

static void tick_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_spinlock_key_t key = k_spin_lock(&lock);

	switch (rendered) {
	case LED_PATTERN_BUTTON_ACK:
		/* One-shot complete. Clearing the priority state and
		 * reselecting will either idle (LEDs off) or land on
		 * a lower-prio pattern that got queued during the
		 * flash (nothing lower today, but future-proofed).
		 */
		led_priority_cancel(&state, LED_PATTERN_BUTTON_ACK);
		reselect_if_changed_locked();
		break;

	case LED_PATTERN_JOINING:
		/* Continuous 500/500 blue blink. Runs indefinitely
		 * until the caller cancels — the join lifecycle
		 * caller (zigbee_app_c) cancels when ZB_BDB_SIGNAL_STEERING
		 * resolves.
		 */
		joining_phase_on = !joining_phase_on;
		set_leds_rgb(0, 0, joining_phase_on ? 1 : 0);
		k_work_reschedule(&tick_work, K_MSEC(JOINING_HALF_PERIOD_MS));
		break;

	case LED_PATTERN_JOIN_SUCCESS:
		/* Solid-on for 2 s, then off. */
		led_priority_cancel(&state, LED_PATTERN_JOIN_SUCCESS);
		reselect_if_changed_locked();
		break;

	case LED_PATTERN_JOIN_FAIL:
		if (join_fail_ticks_remaining == 0) {
			/* Cycle done — LEDs are already off from the
			 * final toggle below on the previous tick.
			 */
			led_priority_cancel(&state, LED_PATTERN_JOIN_FAIL);
			reselect_if_changed_locked();
		} else {
			/* Read the pin (via a shadow) and toggle it. GPIO
			 * driver doesn't offer a "toggle and read new
			 * state" primitive under this API, so use the
			 * remaining-ticks parity to decide: odd count
			 * remaining means we're about to go OFF, even
			 * means ON.
			 */
			bool red_on = (join_fail_ticks_remaining % 2) == 0;

			set_leds_rgb(red_on ? 1 : 0, 0, 0);
			join_fail_ticks_remaining--;
			k_work_reschedule(&tick_work,
					  K_MSEC(JOIN_FAIL_HALF_PERIOD_MS));
		}
		break;

	case LED_PATTERN_ERASE_CONFIRM:
		if (erase_confirm_ticks_remaining == 0) {
			led_priority_cancel(&state, LED_PATTERN_ERASE_CONFIRM);
			reselect_if_changed_locked();
		} else {
			bool red_on = (erase_confirm_ticks_remaining % 2) == 0;

			set_leds_rgb(red_on ? 1 : 0, 0, 0);
			erase_confirm_ticks_remaining--;
			k_work_reschedule(&tick_work,
					  K_MSEC(ERASE_CONFIRM_HALF_PERIOD_MS));
		}
		break;

	case LED_PATTERN_IDENTIFY:
		/* Runs until the ZCL identify duration elapses on the
		 * coordinator side (see zigbee_app.c identify_cb). The
		 * tick just keeps toggling magenta.
		 */
		identify_phase_on = !identify_phase_on;
		set_leds_rgb(identify_phase_on ? 1 : 0, 0,
			     identify_phase_on ? 1 : 0);
		k_work_reschedule(&tick_work, K_MSEC(IDENTIFY_HALF_PERIOD_MS));
		break;

	case LED_PATTERN_FATAL: {
		int64_t elapsed = k_uptime_get() - fatal_start_ms;

		if (elapsed >= FATAL_COLLAPSE_AFTER_MS) {
			/* Collapsed mode: 100 ms flash every 10 s.
			 * Two phases modulo 2: 0 = on, 1 = long off.
			 */
			fatal_phase_index = (fatal_phase_index + 1) % 2;
			if (fatal_phase_index == 0) {
				set_leds_rgb(1, 0, 0);
				k_work_reschedule(&tick_work,
						  K_MSEC(FATAL_COLLAPSE_ON_MS));
			} else {
				set_leds_rgb(0, 0, 0);
				k_work_reschedule(&tick_work,
						  K_MSEC(FATAL_COLLAPSE_OFF_MS));
			}
		} else {
			/* Pre-collapse: N pulses per 5-s cycle. Phase
			 * indices 0..2N-1: even = pulse on, odd = short
			 * off (100 ms) except the last odd index, which
			 * is the long tail-off filling to 5 s total.
			 */
			uint32_t phases = 2u * fatal_flash_count;

			fatal_phase_index = (fatal_phase_index + 1) % phases;
			bool on = (fatal_phase_index % 2) == 0;

			set_leds_rgb(on ? 1 : 0, 0, 0);

			uint32_t delay;

			if (on) {
				delay = FATAL_PULSE_ON_MS;
			} else if (fatal_phase_index < phases - 1u) {
				delay = FATAL_PULSE_OFF_MS;
			} else {
				/* Long tail = 5-s cycle minus the
				 * (2N-1) short-100 ms phases that
				 * precede it (N pulse-on + N-1 short-
				 * off between pulses). Pulse and short-
				 * off are both 100 ms so a single
				 * subtraction covers both.
				 */
				delay = FATAL_CYCLE_MS -
					(2u * fatal_flash_count - 1u) *
					FATAL_PULSE_ON_MS;
			}
			k_work_reschedule(&tick_work, K_MSEC(delay));
		}
		break;
	}

	default:
		/* Stale tick that fired after cancellation, or a pattern
		 * we haven't wired a renderer for yet. Nothing to do.
		 */
		break;
	}

	k_spin_unlock(&lock, key);
}

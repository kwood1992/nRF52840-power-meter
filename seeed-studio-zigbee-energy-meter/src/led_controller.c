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

/* Only pattern with a live renderer in the #28 foundation slice: a
 * single 100 ms all-LEDs-on flash on classified short-press. The rest
 * of the priority table is admissible but no-op-rendered.
 */
#define BUTTON_ACK_ON_MS 100

static void button_ack_off_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(button_ack_off_work, button_ack_off_handler);

static void set_all_leds(int on)
{
	gpio_pin_set_dt(&led_red,   on);
	gpio_pin_set_dt(&led_green, on);
	gpio_pin_set_dt(&led_blue,  on);
}

static void render(enum led_pattern_id pattern)
{
	switch (pattern) {
	case LED_PATTERN_BUTTON_ACK:
		set_all_leds(1);
		k_work_reschedule(&button_ack_off_work, K_MSEC(BUTTON_ACK_ON_MS));
		break;
	case LED_PATTERN_NONE:
		/* Cancelling any in-flight one-shot as we go idle keeps a
		 * button-ack that was preempted before its 100 ms elapsed
		 * from firing later and clobbering whatever pattern the
		 * preemption ended in. cancel is a no-op if the work
		 * isn't scheduled.
		 */
		k_work_cancel_delayable(&button_ack_off_work);
		set_all_leds(0);
		break;
	default:
		/* Dispatchable but not yet rendered — see follow-up issues.
		 * Leave the LEDs off; the priority core still records the
		 * request so its preemption behaviour is verifiable.
		 */
		k_work_cancel_delayable(&button_ack_off_work);
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
	render(selected);
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

static void button_ack_off_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	led_cancel(LED_PATTERN_BUTTON_ACK);
}

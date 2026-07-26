#ifndef LED_PRIORITY_H
#define LED_PRIORITY_H

#include <stdbool.h>

/*
 * Pure-logic priority arbiter for the RGB LED controller (issue #28).
 * Kept off the Zephyr side of led_controller.{c,h} so the priority table
 * and its preemption semantics can be host-tested without GPIO, work
 * queues, or a running kernel.
 *
 * The state models a "single active pattern" invariant. Multiple callers
 * can request different patterns concurrently; only the highest-priority
 * request wins, and losers are dropped rather than queued. That matches
 * the issue's "preempted patterns dropped, not resumed" rule for both
 * runtime preemption (higher-prio arrives after a pattern is running)
 * and admission-time preemption (a lower-prio request loses to the
 * incumbent and is discarded on arrival).
 */

/* Ordered by the issue's priority table — DO NOT renumber without
 * updating every caller AND the priority mapping in led_priority.c.
 * Only patterns 1..COUNT-1 are dispatchable; NONE is the idle state.
 */
enum led_pattern_id {
	LED_PATTERN_NONE = 0,
	LED_PATTERN_FATAL,
	LED_PATTERN_LONG_PRESS_HOLD,
	LED_PATTERN_IDENTIFY,
	LED_PATTERN_JOINING,
	LED_PATTERN_JOIN_SUCCESS,
	LED_PATTERN_JOIN_FAIL,
	LED_PATTERN_ERASE_CONFIRM,
	LED_PATTERN_BUTTON_ACK,
	LED_PATTERN_HEARTBEAT,

	LED_PATTERN_COUNT
};

/* Lower numeric value = higher priority. Values chosen to match the
 * issue's priority table exactly, so a caller passing
 * LED_PRIO_<X> alongside LED_PATTERN_<X> reads naturally.
 */
enum led_priority {
	LED_PRIO_FATAL           = 1,
	LED_PRIO_LONG_PRESS_HOLD = 2,
	LED_PRIO_IDENTIFY        = 3,
	LED_PRIO_JOINING         = 4,
	LED_PRIO_JOIN_SUCCESS    = 5,
	LED_PRIO_JOIN_FAIL       = 6,
	LED_PRIO_ERASE_CONFIRM   = 7,
	LED_PRIO_BUTTON_ACK      = 8,
	LED_PRIO_HEARTBEAT       = 9
};

struct led_priority_state {
	enum led_pattern_id active;
	enum led_priority   active_prio;
};

void led_priority_init(struct led_priority_state *s);

/* Request that `pattern` become the active pattern at `prio`. If nothing
 * is active, or if `prio` is stronger (numerically smaller) than the
 * incumbent's, this displaces the incumbent and returns true. Otherwise
 * the request is dropped and returns false — no queueing.
 */
bool led_priority_request(struct led_priority_state *s,
			  enum led_pattern_id pattern,
			  enum led_priority prio);

/* Clear the active pattern if it matches `pattern`; no-op otherwise.
 * (Because losing requests are never recorded, cancel only meaningfully
 * targets the currently-active pattern.)
 */
void led_priority_cancel(struct led_priority_state *s,
			 enum led_pattern_id pattern);

enum led_pattern_id led_priority_selected(const struct led_priority_state *s);

#endif /* LED_PRIORITY_H */

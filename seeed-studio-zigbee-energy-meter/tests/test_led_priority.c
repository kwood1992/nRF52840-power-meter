#include "led_priority.h"

#include <assert.h>
#include <stdio.h>

static void test_init_is_idle(void)
{
	struct led_priority_state s;

	led_priority_init(&s);

	assert(led_priority_selected(&s) == LED_PATTERN_NONE);
}

static void test_single_request_becomes_active(void)
{
	struct led_priority_state s;

	led_priority_init(&s);

	assert(led_priority_request(&s, LED_PATTERN_BUTTON_ACK,
				    LED_PRIO_BUTTON_ACK) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_BUTTON_ACK);
}

static void test_cancel_active_returns_to_idle(void)
{
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_BUTTON_ACK, LED_PRIO_BUTTON_ACK);

	led_priority_cancel(&s, LED_PATTERN_BUTTON_ACK);

	assert(led_priority_selected(&s) == LED_PATTERN_NONE);
}

static void test_cancel_non_active_is_noop(void)
{
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_JOINING, LED_PRIO_JOINING);

	/* Cancel a pattern that isn't the incumbent — JOINING should stay put. */
	led_priority_cancel(&s, LED_PATTERN_BUTTON_ACK);

	assert(led_priority_selected(&s) == LED_PATTERN_JOINING);
}

static void test_higher_prio_preempts_lower(void)
{
	struct led_priority_state s;

	led_priority_init(&s);

	/* BUTTON_ACK (prio 8) running… */
	assert(led_priority_request(&s, LED_PATTERN_BUTTON_ACK,
				    LED_PRIO_BUTTON_ACK) == true);
	/* …preempted by FATAL (prio 1). */
	assert(led_priority_request(&s, LED_PATTERN_FATAL, LED_PRIO_FATAL) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_FATAL);
}

static void test_lower_prio_is_dropped_not_queued(void)
{
	struct led_priority_state s;

	led_priority_init(&s);

	assert(led_priority_request(&s, LED_PATTERN_JOINING, LED_PRIO_JOINING) == true);
	/* BUTTON_ACK is lower priority (prio 8 > prio 4). It should be
	 * dropped silently — no queueing behind JOINING.
	 */
	assert(led_priority_request(&s, LED_PATTERN_BUTTON_ACK,
				    LED_PRIO_BUTTON_ACK) == false);
	assert(led_priority_selected(&s) == LED_PATTERN_JOINING);

	/* If the dropped request had been queued, cancelling JOINING would
	 * leave BUTTON_ACK visible. Prove it doesn't.
	 */
	led_priority_cancel(&s, LED_PATTERN_JOINING);
	assert(led_priority_selected(&s) == LED_PATTERN_NONE);
}

static void test_priority_table_ordering(void)
{
	/* For every pair (higher, lower) in the priority table, higher must
	 * preempt lower and lower must be dropped when higher is active.
	 * This is the load-bearing property of the whole module.
	 */
	static const struct {
		enum led_pattern_id pattern;
		enum led_priority prio;
	} table[] = {
		{ LED_PATTERN_FATAL,           LED_PRIO_FATAL           },
		{ LED_PATTERN_LONG_PRESS_HOLD, LED_PRIO_LONG_PRESS_HOLD },
		{ LED_PATTERN_IDENTIFY,        LED_PRIO_IDENTIFY        },
		{ LED_PATTERN_JOINING,         LED_PRIO_JOINING         },
		{ LED_PATTERN_JOIN_SUCCESS,    LED_PRIO_JOIN_SUCCESS    },
		{ LED_PATTERN_JOIN_FAIL,       LED_PRIO_JOIN_FAIL       },
		{ LED_PATTERN_ERASE_CONFIRM,   LED_PRIO_ERASE_CONFIRM   },
		{ LED_PATTERN_BUTTON_ACK,      LED_PRIO_BUTTON_ACK      },
		{ LED_PATTERN_HEARTBEAT,       LED_PRIO_HEARTBEAT       },
	};
	const size_t n = sizeof(table) / sizeof(table[0]);

	for (size_t hi = 0; hi < n; hi++) {
		for (size_t lo = hi + 1; lo < n; lo++) {
			struct led_priority_state s;

			/* higher preempts lower */
			led_priority_init(&s);
			assert(led_priority_request(&s, table[lo].pattern,
						    table[lo].prio) == true);
			assert(led_priority_request(&s, table[hi].pattern,
						    table[hi].prio) == true);
			assert(led_priority_selected(&s) == table[hi].pattern);

			/* lower is dropped when higher is incumbent */
			led_priority_init(&s);
			assert(led_priority_request(&s, table[hi].pattern,
						    table[hi].prio) == true);
			assert(led_priority_request(&s, table[lo].pattern,
						    table[lo].prio) == false);
			assert(led_priority_selected(&s) == table[hi].pattern);
		}
	}
}

static void test_button_ack_preempted_by_all_higher_slots(void)
{
	/* Explicit spelling-out of the button-ack case from issue #28's AC:
	 * this is the live consumer in the first slice, and every other
	 * priority-table entry above it must be able to preempt it.
	 */
	static const struct {
		enum led_pattern_id pattern;
		enum led_priority prio;
	} higher[] = {
		{ LED_PATTERN_FATAL,           LED_PRIO_FATAL           },
		{ LED_PATTERN_LONG_PRESS_HOLD, LED_PRIO_LONG_PRESS_HOLD },
		{ LED_PATTERN_IDENTIFY,        LED_PRIO_IDENTIFY        },
		{ LED_PATTERN_JOINING,         LED_PRIO_JOINING         },
		{ LED_PATTERN_JOIN_SUCCESS,    LED_PRIO_JOIN_SUCCESS    },
		{ LED_PATTERN_JOIN_FAIL,       LED_PRIO_JOIN_FAIL       },
		{ LED_PATTERN_ERASE_CONFIRM,   LED_PRIO_ERASE_CONFIRM   },
	};

	for (size_t i = 0; i < sizeof(higher) / sizeof(higher[0]); i++) {
		struct led_priority_state s;

		led_priority_init(&s);
		led_priority_request(&s, LED_PATTERN_BUTTON_ACK,
				     LED_PRIO_BUTTON_ACK);
		assert(led_priority_request(&s, higher[i].pattern,
					    higher[i].prio) == true);
		assert(led_priority_selected(&s) == higher[i].pattern);
	}
}

static void test_button_ack_preempts_heartbeat(void)
{
	/* And the one thing button-ack can preempt: heartbeat (prio 9). */
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_HEARTBEAT, LED_PRIO_HEARTBEAT);
	assert(led_priority_request(&s, LED_PATTERN_BUTTON_ACK,
				    LED_PRIO_BUTTON_ACK) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_BUTTON_ACK);
}

static void test_invalid_pattern_is_rejected(void)
{
	struct led_priority_state s;

	led_priority_init(&s);

	assert(led_priority_request(&s, LED_PATTERN_NONE, LED_PRIO_FATAL) == false);
	assert(led_priority_request(&s, LED_PATTERN_COUNT, LED_PRIO_FATAL) == false);
	assert(led_priority_selected(&s) == LED_PATTERN_NONE);
}

static void test_joining_preempts_button_ack(void)
{
	/* From issue #29 AC: JOINING must preempt BUTTON_ACK. Named explicitly
	 * (redundant with test_priority_table_ordering, but the AC calls it
	 * out and having a dedicated case makes regressions obvious).
	 */
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_BUTTON_ACK, LED_PRIO_BUTTON_ACK);

	assert(led_priority_request(&s, LED_PATTERN_JOINING,
				    LED_PRIO_JOINING) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_JOINING);
}

static void test_long_press_hold_preempts_joining_and_identify(void)
{
	/* From issue #30 AC: LONG_PRESS_HOLD (prio 2) must preempt JOINING
	 * (prio 4) and IDENTIFY (prio 3). Both are covered by
	 * test_priority_table_ordering; naming them makes regressions on
	 * this specific pair pop out immediately in test output.
	 */
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_JOINING, LED_PRIO_JOINING);
	assert(led_priority_request(&s, LED_PATTERN_LONG_PRESS_HOLD,
				    LED_PRIO_LONG_PRESS_HOLD) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_LONG_PRESS_HOLD);

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_IDENTIFY, LED_PRIO_IDENTIFY);
	assert(led_priority_request(&s, LED_PATTERN_LONG_PRESS_HOLD,
				    LED_PRIO_LONG_PRESS_HOLD) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_LONG_PRESS_HOLD);
}

static void test_joining_preempted_by_long_press_hold_and_identify(void)
{
	/* From issue #29 AC: JOINING must be preempted by LONG_PRESS_HOLD
	 * (prio 2) and IDENTIFY (prio 3).
	 */
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_JOINING, LED_PRIO_JOINING);
	assert(led_priority_request(&s, LED_PATTERN_LONG_PRESS_HOLD,
				    LED_PRIO_LONG_PRESS_HOLD) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_LONG_PRESS_HOLD);

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_JOINING, LED_PRIO_JOINING);
	assert(led_priority_request(&s, LED_PATTERN_IDENTIFY,
				    LED_PRIO_IDENTIFY) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_IDENTIFY);
}

static void test_re_request_same_pattern_keeps_stronger_prio(void)
{
	struct led_priority_state s;

	led_priority_init(&s);
	led_priority_request(&s, LED_PATTERN_JOINING, LED_PRIO_JOINING);

	/* Re-requesting the incumbent at a weaker priority (numerically
	 * larger) is a no-op — must not weaken the incumbent's grip, or a
	 * subsequent higher-prio-but-not-strong-enough request could sneak
	 * in.
	 */
	assert(led_priority_request(&s, LED_PATTERN_JOINING,
				    LED_PRIO_BUTTON_ACK) == true);
	assert(led_priority_selected(&s) == LED_PATTERN_JOINING);

	/* Prove the priority didn't get downgraded to BUTTON_ACK: a
	 * JOIN_SUCCESS request (prio 5, weaker than JOINING's original 4
	 * but stronger than BUTTON_ACK's 8) must NOT preempt.
	 */
	assert(led_priority_request(&s, LED_PATTERN_JOIN_SUCCESS,
				    LED_PRIO_JOIN_SUCCESS) == false);
	assert(led_priority_selected(&s) == LED_PATTERN_JOINING);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("led_priority tests\n");
	RUN(test_init_is_idle);
	RUN(test_single_request_becomes_active);
	RUN(test_cancel_active_returns_to_idle);
	RUN(test_cancel_non_active_is_noop);
	RUN(test_higher_prio_preempts_lower);
	RUN(test_lower_prio_is_dropped_not_queued);
	RUN(test_priority_table_ordering);
	RUN(test_button_ack_preempted_by_all_higher_slots);
	RUN(test_button_ack_preempts_heartbeat);
	RUN(test_invalid_pattern_is_rejected);
	RUN(test_joining_preempts_button_ack);
	RUN(test_joining_preempted_by_long_press_hold_and_identify);
	RUN(test_long_press_hold_preempts_joining_and_identify);
	RUN(test_re_request_same_pattern_keeps_stronger_prio);
	printf("all tests passed\n");
	return 0;
}

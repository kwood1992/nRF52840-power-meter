#include "button_action.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/*
 * The short press is the interesting case: it means "join" before the
 * device has a network and "wake me so a write can land" afterwards.
 * See issue #62 — writes to imp_per_kwh / min_pulse_width_us time out
 * in steady state because the sleepy ED polls its parent every 60 s.
 */

static void test_short_press_unjoined_joins(void)
{
	assert(button_action_for_press(BUTTON_PRESS_SHORT, false) ==
	       BUTTON_ACTION_JOIN);
}

static void test_short_press_joined_wakes(void)
{
	/* Already on a network — re-steering would be pointless and would
	 * disturb join state, which #62's acceptance criteria forbid. The
	 * press becomes a turbo-poll refresh instead.
	 */
	assert(button_action_for_press(BUTTON_PRESS_SHORT, true) ==
	       BUTTON_ACTION_WAKE);
}

static void test_long_press_always_factory_resets(void)
{
	/* Factory reset must not depend on join state — a device stuck
	 * half-joined is exactly when the user reaches for it.
	 */
	assert(button_action_for_press(BUTTON_PRESS_LONG, false) ==
	       BUTTON_ACTION_FACTORY_RESET);
	assert(button_action_for_press(BUTTON_PRESS_LONG, true) ==
	       BUTTON_ACTION_FACTORY_RESET);
}

static void test_neither_is_always_none(void)
{
	assert(button_action_for_press(BUTTON_PRESS_NEITHER, false) ==
	       BUTTON_ACTION_NONE);
	assert(button_action_for_press(BUTTON_PRESS_NEITHER, true) ==
	       BUTTON_ACTION_NONE);
}

static void test_unknown_kind_is_none(void)
{
	/* Defensive: a future classifier value must not fall through to
	 * a destructive action.
	 */
	assert(button_action_for_press((enum button_press_kind)99, true) ==
	       BUTTON_ACTION_NONE);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("button_action tests\n");
	RUN(test_short_press_unjoined_joins);
	RUN(test_short_press_joined_wakes);
	RUN(test_long_press_always_factory_resets);
	RUN(test_neither_is_always_none);
	RUN(test_unknown_kind_is_none);
	printf("all tests passed\n");
	return 0;
}

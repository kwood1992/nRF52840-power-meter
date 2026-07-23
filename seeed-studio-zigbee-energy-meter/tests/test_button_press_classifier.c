#include "button_press_classifier.h"

#include <assert.h>
#include <stdio.h>

/* Production thresholds (per design doc):
 *   short = <1000 ms → join
 *   long  = ≥3000 ms → factory reset
 * The 1000–3000 ms band deliberately classifies as NEITHER.
 */
#define TEST_SHORT_MAX_MS 1000U
#define TEST_LONG_MIN_MS  3000U

static void test_init_stores_thresholds(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	assert(c.short_max_ms == TEST_SHORT_MAX_MS);
	assert(c.long_min_ms == TEST_LONG_MIN_MS);
}

static void test_zero_duration_is_neither(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	/* Chatter / glitch on the line — release matches press timestamp.
	 * Don't act on it.
	 */
	assert(button_press_classifier_classify(&c, 0) == BUTTON_PRESS_NEITHER);
}

static void test_well_under_short_is_short(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	assert(button_press_classifier_classify(&c, 100) == BUTTON_PRESS_SHORT);
	assert(button_press_classifier_classify(&c, 500) == BUTTON_PRESS_SHORT);
	assert(button_press_classifier_classify(&c, 999) == BUTTON_PRESS_SHORT);
}

static void test_exactly_short_max_is_neither(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	/* short_max_ms is exclusive on the short side — press must be
	 * strictly under it to count. This is the guard against a
	 * one-tick-late release accidentally firing the join.
	 */
	assert(button_press_classifier_classify(&c, TEST_SHORT_MAX_MS) ==
	       BUTTON_PRESS_NEITHER);
}

static void test_between_thresholds_is_neither(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	assert(button_press_classifier_classify(&c, 1500) == BUTTON_PRESS_NEITHER);
	assert(button_press_classifier_classify(&c, 2000) == BUTTON_PRESS_NEITHER);
	assert(button_press_classifier_classify(&c, 2999) == BUTTON_PRESS_NEITHER);
}

static void test_exactly_long_min_is_long(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	/* long_min_ms is inclusive — an exactly-3-second hold triggers
	 * the factory reset. Symmetry with the SHORT side would say
	 * "strictly greater than", but factory reset is intentional
	 * enough that the inclusive threshold is safer to reason about.
	 */
	assert(button_press_classifier_classify(&c, TEST_LONG_MIN_MS) ==
	       BUTTON_PRESS_LONG);
}

static void test_well_over_long_is_long(void)
{
	struct button_press_classifier c;

	button_press_classifier_init(&c, TEST_SHORT_MAX_MS, TEST_LONG_MIN_MS);

	assert(button_press_classifier_classify(&c, 5000) == BUTTON_PRESS_LONG);
	assert(button_press_classifier_classify(&c, 30000) == BUTTON_PRESS_LONG);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("button_press_classifier tests\n");
	RUN(test_init_stores_thresholds);
	RUN(test_zero_duration_is_neither);
	RUN(test_well_under_short_is_short);
	RUN(test_exactly_short_max_is_neither);
	RUN(test_between_thresholds_is_neither);
	RUN(test_exactly_long_min_is_long);
	RUN(test_well_over_long_is_long);
	printf("all tests passed\n");
	return 0;
}

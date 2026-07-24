#include "report_gate.h"

#include <assert.h>
#include <stdio.h>

static void test_period_zero_never_fires(void)
{
	struct report_gate g;

	report_gate_init(&g, 0);

	/* Disabled gate — no matter how many pulses we throw at it, the
	 * bench-driven force-report path must stay off. Otherwise a
	 * Kconfig-off deployment would still burn extra radio time.
	 */
	for (int i = 0; i < 10; i++) {
		assert(!report_gate_advance(&g));
	}
}

static void test_period_one_fires_every_call(void)
{
	struct report_gate g;

	report_gate_init(&g, 1);

	for (int i = 0; i < 5; i++) {
		assert(report_gate_advance(&g));
	}
}

static void test_fires_exactly_on_the_nth_call(void)
{
	struct report_gate g;

	report_gate_init(&g, 3);

	assert(!report_gate_advance(&g)); /* 1 */
	assert(!report_gate_advance(&g)); /* 2 */
	assert(report_gate_advance(&g));  /* 3 — fires */
	assert(!report_gate_advance(&g)); /* 1 */
	assert(!report_gate_advance(&g)); /* 2 */
	assert(report_gate_advance(&g));  /* 3 — fires again */
}

static void test_period_100_matches_production_config(void)
{
	struct report_gate g;

	report_gate_init(&g, 100);

	int fires = 0;

	/* 250 advances should trip the gate exactly twice (at #100 and
	 * #200) — matches the "every 100 pulses" heartbeat cadence the
	 * fix targets in issue #20.
	 */
	for (int i = 0; i < 250; i++) {
		if (report_gate_advance(&g)) {
			fires++;
		}
	}
	assert(fires == 2);
}

static void test_reset_between_fires_prevents_drift(void)
{
	struct report_gate g;

	report_gate_init(&g, 4);

	int fires = 0;

	/* Off-by-one guard: after a fire the counter resets to 0 so the
	 * next fire happens exactly `period` calls later, not `period - 1`
	 * or `period + 1`. Regression coverage for a common counter bug.
	 */
	for (int i = 1; i <= 12; i++) {
		bool fired = report_gate_advance(&g);

		if (fired) {
			fires++;
			assert(i % 4 == 0);
		}
	}
	assert(fires == 3);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("report_gate tests\n");
	RUN(test_period_zero_never_fires);
	RUN(test_period_one_fires_every_call);
	RUN(test_fires_exactly_on_the_nth_call);
	RUN(test_period_100_matches_production_config);
	RUN(test_reset_between_fires_prevents_drift);
	printf("all tests passed\n");
	return 0;
}

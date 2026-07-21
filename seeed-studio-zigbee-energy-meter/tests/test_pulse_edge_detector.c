#include "pulse_edge_detector.h"

#include <assert.h>
#include <stdio.h>

static void test_first_sample_never_crosses(void)
{
	struct pulse_edge_detector d;

	pulse_edge_detector_init(&d, 1000);

	assert(pulse_edge_detector_sample(&d, 1500) == false);
}

static void test_below_to_above_is_rising_edge(void)
{
	struct pulse_edge_detector d;

	pulse_edge_detector_init(&d, 1000);
	pulse_edge_detector_sample(&d, 500);

	assert(pulse_edge_detector_sample(&d, 1500) == true);
}

static void test_staying_above_does_not_re_trigger(void)
{
	struct pulse_edge_detector d;

	pulse_edge_detector_init(&d, 1000);
	pulse_edge_detector_sample(&d, 500);
	pulse_edge_detector_sample(&d, 1500);

	assert(pulse_edge_detector_sample(&d, 1600) == false);
	assert(pulse_edge_detector_sample(&d, 2000) == false);
}

static void test_falling_edge_does_not_trigger(void)
{
	struct pulse_edge_detector d;

	pulse_edge_detector_init(&d, 1000);
	pulse_edge_detector_sample(&d, 1500);

	assert(pulse_edge_detector_sample(&d, 500) == false);
}

static void test_multiple_crossings_counted_separately(void)
{
	struct pulse_edge_detector d;

	pulse_edge_detector_init(&d, 1000);
	int32_t samples[] = { 500, 1500, 400, 2000, 300, 1200, 700 };
	int crossings = 0;

	for (int i = 0; i < (int)(sizeof(samples) / sizeof(samples[0])); i++) {
		if (pulse_edge_detector_sample(&d, samples[i])) {
			crossings++;
		}
	}

	assert(crossings == 3);
}

#define RUN(t) do { printf("  " #t " ... "); t(); printf("ok\n"); } while (0)

int main(void)
{
	printf("pulse_edge_detector tests\n");
	RUN(test_first_sample_never_crosses);
	RUN(test_below_to_above_is_rising_edge);
	RUN(test_staying_above_does_not_re_trigger);
	RUN(test_falling_edge_does_not_trigger);
	RUN(test_multiple_crossings_counted_separately);
	printf("all tests passed\n");
	return 0;
}

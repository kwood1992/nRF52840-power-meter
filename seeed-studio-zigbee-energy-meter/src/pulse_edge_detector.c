#include "pulse_edge_detector.h"

void pulse_edge_detector_init(struct pulse_edge_detector *d, int32_t threshold_mv)
{
	d->threshold_mv = threshold_mv;
	d->has_prev = false;
	d->prev_above = false;
}

bool pulse_edge_detector_sample(struct pulse_edge_detector *d, int32_t mv)
{
	bool above = mv > d->threshold_mv;

	if (!d->has_prev) {
		d->has_prev = true;
		d->prev_above = above;
		return false;
	}

	bool rising_edge = above && !d->prev_above;

	d->prev_above = above;
	return rising_edge;
}

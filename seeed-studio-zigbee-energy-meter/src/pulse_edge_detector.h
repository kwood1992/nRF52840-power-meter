#ifndef PULSE_EDGE_DETECTOR_H
#define PULSE_EDGE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

struct pulse_edge_detector {
	int32_t threshold_mv;
	bool has_prev;
	bool prev_above;
};

void pulse_edge_detector_init(struct pulse_edge_detector *d, int32_t threshold_mv);

bool pulse_edge_detector_sample(struct pulse_edge_detector *d, int32_t mv);

#endif

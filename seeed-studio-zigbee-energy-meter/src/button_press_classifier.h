#ifndef BUTTON_PRESS_CLASSIFIER_H
#define BUTTON_PRESS_CLASSIFIER_H

#include <stdint.h>

/*
 * Turns a "held for N ms" duration into a discrete short-press / long-press /
 * ignored classification. Kept as a pure-logic helper so the thresholds and
 * edge-case handling can be host-tested without Zephyr's GPIO stack.
 *
 * The gap between short_max_ms and long_min_ms is deliberate — the design
 * doc calls out short = <1 s (join) and long = >3 s (factory reset). Presses
 * that land between 1 and 3 s classify as NEITHER, so an accidental
 * mid-length release neither joins a network nor factory-resets the device.
 */

enum button_press_kind {
	BUTTON_PRESS_NEITHER = 0,
	BUTTON_PRESS_SHORT,
	BUTTON_PRESS_LONG,
};

struct button_press_classifier {
	uint32_t short_max_ms;
	uint32_t long_min_ms;
};

void button_press_classifier_init(struct button_press_classifier *c,
				  uint32_t short_max_ms,
				  uint32_t long_min_ms);

enum button_press_kind button_press_classifier_classify(
	const struct button_press_classifier *c,
	uint32_t duration_ms);

#endif

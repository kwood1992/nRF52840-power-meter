#include "button_press_classifier.h"

void button_press_classifier_init(struct button_press_classifier *c,
				  uint32_t short_max_ms,
				  uint32_t long_min_ms)
{
	c->short_max_ms = short_max_ms;
	c->long_min_ms = long_min_ms;
}

enum button_press_kind button_press_classifier_classify(
	const struct button_press_classifier *c,
	uint32_t duration_ms)
{
	if (duration_ms == 0) {
		return BUTTON_PRESS_NEITHER;
	}
	if (duration_ms < c->short_max_ms) {
		return BUTTON_PRESS_SHORT;
	}
	if (duration_ms >= c->long_min_ms) {
		return BUTTON_PRESS_LONG;
	}
	return BUTTON_PRESS_NEITHER;
}

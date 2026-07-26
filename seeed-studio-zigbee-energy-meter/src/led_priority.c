#include "led_priority.h"

void led_priority_init(struct led_priority_state *s)
{
	s->active = LED_PATTERN_NONE;
	s->active_prio = 0;
}

bool led_priority_request(struct led_priority_state *s,
			  enum led_pattern_id pattern,
			  enum led_priority prio)
{
	if (pattern <= LED_PATTERN_NONE || pattern >= LED_PATTERN_COUNT) {
		return false;
	}

	if (s->active == LED_PATTERN_NONE || prio < s->active_prio) {
		s->active = pattern;
		s->active_prio = prio;
		return true;
	}

	if (pattern == s->active) {
		/* Same pattern re-requested at equal or weaker priority.
		 * Keep the incumbent's stronger priority so future
		 * preemption comparisons stay honest.
		 */
		return true;
	}

	return false;
}

void led_priority_cancel(struct led_priority_state *s,
			 enum led_pattern_id pattern)
{
	if (s->active == pattern) {
		s->active = LED_PATTERN_NONE;
		s->active_prio = 0;
	}
}

enum led_pattern_id led_priority_selected(const struct led_priority_state *s)
{
	return s->active;
}

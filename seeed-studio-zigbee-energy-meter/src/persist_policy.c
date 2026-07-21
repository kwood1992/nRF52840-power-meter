#include "persist_policy.h"

void persist_policy_init(struct persist_policy *p,
			 uint64_t interval_ms,
			 uint64_t max_pulse_delta)
{
	p->interval_ms = interval_ms;
	p->max_pulse_delta = max_pulse_delta;
}

bool persist_policy_should_write(const struct persist_policy *p,
				 uint64_t current_total,
				 uint64_t last_saved_total,
				 uint64_t ms_since_last_write)
{
	if (current_total == last_saved_total) {
		return false;
	}

	if (current_total > last_saved_total &&
	    (current_total - last_saved_total) >= p->max_pulse_delta) {
		return true;
	}

	if (ms_since_last_write >= p->interval_ms) {
		return true;
	}

	return false;
}

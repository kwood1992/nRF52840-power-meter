#include "report_gate.h"

void report_gate_init(struct report_gate *g, uint32_t period)
{
	g->period = period;
	g->counter = 0;
}

bool report_gate_advance(struct report_gate *g)
{
	if (g->period == 0) {
		return false;
	}

	g->counter++;
	if (g->counter >= g->period) {
		g->counter = 0;
		return true;
	}
	return false;
}

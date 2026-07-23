#include "metering_scale.h"

struct metering_u48 metering_scale_to_u48(uint64_t pulse_total)
{
	struct metering_u48 v;

	if (pulse_total > METERING_SUMMATION_U48_MAX) {
		pulse_total = METERING_SUMMATION_U48_MAX;
	}

	v.low = (uint32_t)(pulse_total & 0xFFFFFFFFULL);
	v.high = (uint16_t)((pulse_total >> 32) & 0xFFFFULL);
	return v;
}

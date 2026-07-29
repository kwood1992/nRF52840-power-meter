#include "calibration.h"

bool calibration_is_valid_imp_per_kwh(uint32_t imp_per_kwh)
{
	return imp_per_kwh >= CALIBRATION_IMP_PER_KWH_MIN &&
	       imp_per_kwh <= CALIBRATION_IMP_PER_KWH_MAX;
}

bool calibration_is_valid_pulse_min_width_us(uint32_t min_width_us)
{
	return min_width_us >= CALIBRATION_PULSE_MIN_WIDTH_US_MIN &&
	       min_width_us <= CALIBRATION_PULSE_MIN_WIDTH_US_MAX;
}

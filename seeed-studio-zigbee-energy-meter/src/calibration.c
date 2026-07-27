#include "calibration.h"

bool calibration_is_valid_imp_per_kwh(uint32_t imp_per_kwh)
{
	return imp_per_kwh >= CALIBRATION_IMP_PER_KWH_MIN &&
	       imp_per_kwh <= CALIBRATION_IMP_PER_KWH_MAX;
}

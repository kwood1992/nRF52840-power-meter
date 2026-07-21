#include "lpcomp_ref.h"

uint8_t lpcomp_choose_ref_step_16(uint32_t target_mv, uint32_t vdd_mv)
{
	if (vdd_mv == 0) {
		return 8;
	}

	uint32_t scaled = (target_mv * 16U + vdd_mv / 2U) / vdd_mv;

	if (scaled < 1U) {
		return 1U;
	}
	if (scaled > 15U) {
		return 15U;
	}
	return (uint8_t)scaled;
}

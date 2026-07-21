#ifndef LPCOMP_REF_H
#define LPCOMP_REF_H

#include <stdint.h>

/*
 * Turns a desired trigger voltage into the nearest LPCOMP internal-reference
 * step. Kept as a pure function so the mapping stays host-testable — the
 * nrfx enum values it feeds into don't matter for the arithmetic.
 *
 * The 1/16 divider ladder gives 15 usable steps (1..15 × VDD/16). We always
 * pick the /16 ladder for finer resolution; the /8 mode is a subset. Result
 * is clamped to [1, 15] so callers never sit on the rail.
 */
uint8_t lpcomp_choose_ref_step_16(uint32_t target_mv, uint32_t vdd_mv);

#endif

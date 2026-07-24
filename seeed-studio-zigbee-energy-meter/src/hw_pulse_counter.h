#ifndef HW_PULSE_COUNTER_H
#define HW_PULSE_COUNTER_H

#include <stdint.h>

/* Bring up the LPCOMP → PPI → TIMER2 hardware pulse chain plus a second
 * PPI branch from GPIOTE(D7) → TIMER2 that preserves the tools/xiao-pulse.sh
 * bench-inject workflow. After a successful call the counter is running and
 * every phototransistor rising edge (LPCOMP UP) OR bench D7 falling edge
 * bumps the same 32-bit hardware register with no CPU involvement.
 *
 * Returns 0 on success or a negative errno on driver init failure.
 */
int hw_pulse_counter_init(void);

/* Snapshot the TIMER2 counter register via a CAPTURE task. Cheap (a
 * handful of register writes/reads), safe to call from any thread, and
 * does not stop or clear the counter. Wraps at 2^32 pulses — the software
 * accumulator handles that in the same way it already handled the atomic_t
 * source before this module existed.
 */
uint32_t hw_pulse_counter_read(void);

#endif

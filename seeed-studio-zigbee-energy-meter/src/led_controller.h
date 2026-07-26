#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <stdint.h>

#include "led_priority.h"

/*
 * Onboard RGB LED controller for the XIAO nRF52840. Owns led0 (red),
 * led1 (green), and led2 (blue) via their DT aliases, and mediates every
 * LED-driving code path through the priority table declared in
 * led_priority.h.
 *
 * Foundation slice of issue #28: only LED_PATTERN_BUTTON_ACK is wired to
 * a concrete renderer (100 ms all-LEDs-on flash) in this file. The other
 * slots are dispatchable — a caller can request them, priority-arbitration
 * runs — but nothing renders yet. Follow-up issues #29-#33 land the
 * remaining renderers (join lifecycle, identify, long-press hold, fatal
 * loops, boot / heartbeat flashes).
 *
 * The existing main.c LED users (heartbeat toggle, boot blink, erase
 * confirm) still poke led0 directly through their own gpio_dt_spec and
 * are NOT migrated in this slice — they land in follow-ups. That means a
 * button-ack flash briefly displaces whatever colour the heartbeat had on
 * red; acceptable per the issue's explicit "unchanged in this issue"
 * carve-out.
 */

int led_controller_init(void);

void led_request(enum led_pattern_id pattern, enum led_priority prio);
void led_cancel(enum led_pattern_id pattern);

/*
 * Request the top-priority fatal-error pattern with `flash_count` red
 * flashes per 5-second cycle at ~10% duty (#32). Clamped to 1..5.
 *
 * Flash-count → failure-site mapping (kept in main.c's fatal branches;
 * updating this table means updating the mapping there too):
 *   1 = CDC-ACM device_not_ready
 *   2 = usb_enable failed
 *   3 = user_button_configure failed
 *   4 = hw_pulse_counter_init failed
 *   5 = user_button_arm_irq failed
 *
 * After 10 minutes of continuous flashing, the pattern collapses to a
 * single 100 ms flash every 10 s — a broken field device with fresh
 * batteries needs the failure indicator to survive long enough to be
 * noticed on a maintenance visit weeks later, and 10% duty over
 * fortnights burns the pack.
 */
void led_request_fatal(uint32_t flash_count);

#endif /* LED_CONTROLLER_H */

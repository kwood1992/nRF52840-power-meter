#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

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

#endif /* LED_CONTROLLER_H */

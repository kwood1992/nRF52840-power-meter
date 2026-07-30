#ifndef BUTTON_ACTION_H
#define BUTTON_ACTION_H

#include <stdbool.h>

#include "button_press_classifier.h"

/*
 * Routes a classified button press to the action it should take, given
 * the current join state. Split from button_press_classifier because
 * "how long was it held" and "what should that mean right now" are two
 * separate decisions — the classifier is state-free, this is not.
 *
 * The short press is overloaded on purpose (issue #62):
 *
 *   not joined → JOIN. The original behaviour: kick off network
 *                steering so the user can pair the device.
 *   joined     → WAKE. Re-steering an already-joined device is a no-op
 *                at best, so the press instead opens a turbo-poll
 *                window. Without it, a Z2M write to a runtime-writable
 *                attribute (imp_per_kwh, min_pulse_width_us) has to
 *                wait out the 60 s steady-state long poll and blows
 *                past Z2M's 10 s ZCL deadline.
 *
 * Long press is unconditional — a device the user believes is stuck is
 * exactly when factory reset has to work regardless of join state.
 */

enum button_action {
	BUTTON_ACTION_NONE = 0,
	BUTTON_ACTION_JOIN,
	BUTTON_ACTION_WAKE,
	BUTTON_ACTION_FACTORY_RESET,
};

enum button_action button_action_for_press(enum button_press_kind kind,
					   bool joined);

#endif

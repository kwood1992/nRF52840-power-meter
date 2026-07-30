#include "button_action.h"

enum button_action button_action_for_press(enum button_press_kind kind,
					  bool joined)
{
	switch (kind) {
	case BUTTON_PRESS_SHORT:
		return joined ? BUTTON_ACTION_WAKE : BUTTON_ACTION_JOIN;
	case BUTTON_PRESS_LONG:
		return BUTTON_ACTION_FACTORY_RESET;
	case BUTTON_PRESS_NEITHER:
	default:
		/* Unknown classifications fall through to NONE rather than
		 * to anything that changes device state.
		 */
		return BUTTON_ACTION_NONE;
	}
}

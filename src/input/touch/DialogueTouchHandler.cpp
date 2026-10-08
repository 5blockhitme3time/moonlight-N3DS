/*
 * Touch routing for the dialogue panel.
 *
 * The bottom screen shows the panel, not the game, so touches are never
 * forwarded as mouse input - a click at those coordinates would land on an
 * unrelated spot of the streamed image. They are queued for the panel, which
 * handles them on its own thread (see PanelController).
 */
#include "TouchHandler.hpp"

#include "../../panel/panel_input.hpp"

void DialogueTouchHandler::_handle_touch_down(touchPosition touch) {
    panel_touch_push(PANEL_TOUCH_DOWN, touch.px, touch.py);
}

void DialogueTouchHandler::_handle_touch_hold(touchPosition touch) {
    panel_touch_push(PANEL_TOUCH_MOVE, touch.px, touch.py);
}

void DialogueTouchHandler::_handle_touch_up(touchPosition touch) {
    (void)touch;   // libctru reports no position on release
    panel_touch_push(PANEL_TOUCH_UP, 0, 0);
}

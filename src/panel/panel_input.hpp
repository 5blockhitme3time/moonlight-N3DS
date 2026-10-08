/*
 * What the input thread hands to the dialogue panel: touches and console
 * buttons. Kept apart from panel_controller.hpp so the input code does not
 * pull in the panel's font/curl dependency chain.
 *
 * Both functions only queue the event; PanelController handles it on its own
 * thread. They are safe to call when no panel exists (events are dropped).
 */
#pragma once

enum PanelTouchKind { PANEL_TOUCH_DOWN, PANEL_TOUCH_MOVE, PANEL_TOUCH_UP };

// Console buttons, by what they do in the panel (the input code maps the
// physical buttons, honouring the face-button swap setting).
enum PanelKey {
    PANEL_KEY_ADVANCE,   // A: next line / pick the highlighted option
    PANEL_KEY_BACK,      // B: back to the newest line from history
    PANEL_KEY_UP,        // D-pad: scroll history / move between options
    PANEL_KEY_DOWN,
    PANEL_KEY_HISTORY,   // L
    PANEL_KEY_SKIP,      // R
    PANEL_KEY_AUTO,      // X
    PANEL_KEY_VOICE,     // Y
    PANEL_KEY_MENU,      // START: settings / save / load / title
    PANEL_KEY_COUNT
};

void panel_touch_push(PanelTouchKind kind, int x, int y);
void panel_key_push(PanelKey key, bool down);

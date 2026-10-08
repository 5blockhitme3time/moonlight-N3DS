/*
 * Runs the dialogue panel on the console's bottom screen.
 *
 * Owns the fonts, the relay link and the UI, and does the platform work:
 * time, touch delivery, the framebuffer blit, and knowing when the panel may
 * draw at all. It draws only while the touch mode is DIALOGUE - every other
 * mode puts its own image (or the mirrored stream) on the bottom screen - and
 * it repaints after anything that can clobber that screen behind its back: a
 * renderer switch, or returning from the HOME menu or sleep.
 *
 * All UI work happens on the thread that calls tick(). Touches arrive on the
 * input thread and are only queued (panel_touch_push), so the UI never runs
 * concurrently with itself.
 */
#pragma once

#include <3ds.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../relay/relay_client.hpp"
#include "../system/subscriber.hpp"
#include "panel_font.hpp"
#include "panel_input.hpp"
#include "panel_ui.hpp"

class PanelController : public ISubscriber {
  public:
    PanelController();
    ~PanelController() override;

    void start(const std::string &relay_host, int relay_port);
    void stop();

    // Call from the stream loop. Cheap when nothing changed.
    void tick();

    void accept(IMessage *msg) override;   // touch mode changes

  private:
    static void apt_hook(APT_HookType hook, void *param);
    void blit();
    void set_mode(N3dsTouchType t);
    void follow_game_screen(uint64_t now);

    PanelFont body, small, tiny;
    RelayClient link;
    PanelUI ui;
    relay::Snapshot snap;
    std::vector<uint16_t> frame;

    std::atomic<bool> active{false};
    std::atomic<bool> repaint{false};
    std::atomic<N3dsTouchType> mode{N3dsTouchType::DISABLED};

    // following the game onto its own screens (see follow_game_screen)
    bool want_mirror = false;
    bool auto_mirrored = false;
    int stable_state = -1;    // 0 story/title, 1 game screen, -1 unknown
    int pending_state = -1;
    uint64_t pending_since = 0;
    uint64_t repaint_until = 0;
    uint64_t last_render = 0;
    bool started = false;
    aptHookCookie cookie;
};

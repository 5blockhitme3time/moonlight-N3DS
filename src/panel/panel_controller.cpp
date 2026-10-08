/*
 * Runs the dialogue panel on the console's bottom screen - implementation.
 */
#include "panel_controller.hpp"

#include <algorithm>
#include <cstring>

#include "../diag.hpp"
#include "../system/dispatcher.hpp"
#include "../system/pair_record.hpp"   // MOONLIGHT_3DS_PATH
#include "panel_assets.hpp"

namespace {

// ---- input queue (input thread -> panel thread) -------------------------

enum EventKind { EV_TOUCH_DOWN, EV_TOUCH_MOVE, EV_TOUCH_UP, EV_KEY_DOWN, EV_KEY_UP };

struct TouchEvent {
    EventKind kind;
    int x, y;   // touch position, or the PanelKey in x
    uint64_t t;
};

LightLock g_touch_lock;
bool g_touch_init = false;
bool g_touch_open = false;
TouchEvent g_touch[32];
int g_touch_n = 0;

void touch_lock_init() {
    if (!g_touch_init) {
        LightLock_Init(&g_touch_lock);
        g_touch_init = true;
    }
}

const int W = PanelUI::kWidth;
const int H = PanelUI::kHeight;

// Window after activation during which every tick re-blits: the renderer
// being replaced clears the bottom screen from another thread, and this
// makes the panel win that race without coordinating with it.
const uint64_t kRepaintWindowMs = 400;
// ~60 fps cap on re-renders while something animates or a finger drags.
const uint64_t kFrameMs = 15;

}  // namespace

static void push_event(EventKind kind, int x, int y) {
    touch_lock_init();
    LightLock_Lock(&g_touch_lock);
    if (g_touch_open) {
        // Consecutive moves only matter for their last position.
        if (kind == EV_TOUCH_MOVE && g_touch_n > 0 &&
            g_touch[g_touch_n - 1].kind == EV_TOUCH_MOVE) {
            g_touch[g_touch_n - 1].x = x;
            g_touch[g_touch_n - 1].y = y;
            g_touch[g_touch_n - 1].t = osGetTime();
        } else if (g_touch_n < 32) {
            g_touch[g_touch_n++] = TouchEvent{kind, x, y, osGetTime()};
        }
    }
    LightLock_Unlock(&g_touch_lock);
}

void panel_touch_push(PanelTouchKind kind, int x, int y) {
    push_event(kind == PANEL_TOUCH_DOWN   ? EV_TOUCH_DOWN
               : kind == PANEL_TOUCH_MOVE ? EV_TOUCH_MOVE
                                          : EV_TOUCH_UP,
               x, y);
}

void panel_key_push(PanelKey key, bool down) {
    push_event(down ? EV_KEY_DOWN : EV_KEY_UP, (int)key, 0);
}

PanelController::PanelController()
    : ui(PanelFonts{&body, &small, &tiny},
         [this](const char *cmd, int arg, int sel) {
             if (cmd[0] == '@') {
                 // handled here: "@retry" redials the relay now, "@mirror"
                 // changes the touch mode
                 if (strcmp(cmd, "@retry") == 0) {
                     link.retry_now();
                 } else {
                     want_mirror = true;
                 }
                 return;
             }
             link.send(cmd, arg, sel);
         }),
      frame((size_t)W * H, 0) {}

void PanelController::set_mode(N3dsTouchType t) {
    MessageDispatcher::get_instance()->post(
        std::make_shared<TouchStateChangedMsg>(t));
}

// The game's settings, save and load screens are full-screen pages made for
// a mouse; the panel has nothing useful to show over them. While one is open
// the bottom screen mirrors the stream instead, so it can be operated
// directly (Moonlight's own MIRROR mode), and the panel comes back when the
// game returns to the story or the title. Only switches it made itself are
// undone: a mode the user picked from the HOME menu is left alone.
void PanelController::follow_game_screen(uint64_t now) {
    bool game_ui = snap.relay_up && snap.game_up &&
                   (snap.loc == "menu" || snap.loc == "dialog");
    bool reading = snap.relay_up && snap.game_up &&
                   (snap.loc == "play" || snap.loc == "title");
    int state = game_ui ? 1 : reading ? 0 : -1;
    if (state != pending_state) {
        pending_state = state;
        pending_since = now;
        return;
    }
    // ignore screens that flash past (a menu opening and closing at once)
    if (now - pending_since < 300 || state == stable_state) {
        return;
    }
    int before = stable_state;
    stable_state = state;
    N3dsTouchType cur = mode.load();
    if (before == 0 && state == 1 && cur == N3dsTouchType::DIALOGUE) {
        auto_mirrored = true;
        set_mode(N3dsTouchType::ABSOLUTE_TOUCH);
    } else if (state == 0 && auto_mirrored) {
        auto_mirrored = false;
        if (cur == N3dsTouchType::ABSOLUTE_TOUCH) {
            set_mode(N3dsTouchType::DIALOGUE);
            ui.notify("已回到台词面板", now);
        }
    }
}

PanelController::~PanelController() { stop(); }

void PanelController::start(const std::string &relay_host, int relay_port) {
    if (started) {
        return;
    }
    started = true;
    const uint8_t *data = panel_font_data();
    size_t size = panel_font_size();
    body.load(data, size, 20);
    small.load(data, size, 15);
    tiny.load(data, size, 12);
    // A full CJK font on the SD card covers what the embedded subset
    // lacks (rare kanji, Korean, ...). The first file found is used.
    static const char *const kExtraFonts[] = {
        MOONLIGHT_3DS_PATH "/kiripanel/font.ttf",
        MOONLIGHT_3DS_PATH "/kiripanel/font.ttc",
        MOONLIGHT_3DS_PATH "/kiripanel/font.otf",
    };
    for (const char *path : kExtraFonts) {
        if (body.add_fallback(path)) {
            small.add_fallback(path);
            tiny.add_fallback(path);
            break;
        }
    }

    link.start(relay_host, relay_port);
    ui.set_relay_label(link.address());

    touch_lock_init();
    LightLock_Lock(&g_touch_lock);
    g_touch_open = true;
    g_touch_n = 0;
    LightLock_Unlock(&g_touch_lock);

    aptHook(&cookie, apt_hook, this);
    MessageDispatcher::get_instance()->subscribe(
        MessageType::TOUCH_STATE_CHANGED, this);
}

void PanelController::stop() {
    if (!started) {
        return;
    }
    started = false;
    // Unsubscribing waits out any accept() in progress on the dispatch
    // thread, so nothing calls into this object afterwards.
    MessageDispatcher::get_instance()->unsubscribe(
        MessageType::TOUCH_STATE_CHANGED, this);
    aptUnhook(&cookie);
    LightLock_Lock(&g_touch_lock);
    g_touch_open = false;
    g_touch_n = 0;
    LightLock_Unlock(&g_touch_lock);
    link.stop();
    if (active.load()) {
        // leave a blank screen rather than a frozen panel
        std::fill(frame.begin(), frame.end(), 0);
        blit();
    }
}

void PanelController::accept(IMessage *msg) {
    if (msg->getMessageType() != MessageType::TOUCH_STATE_CHANGED) {
        return;
    }
    auto m = static_cast<TouchStateChangedMsg *>(msg);
    mode.store(m->ttype);
    bool on = m->ttype == N3dsTouchType::DIALOGUE;
    active.store(on);
    if (on) {
        repaint.store(true);
    }
}

void PanelController::apt_hook(APT_HookType hook, void *param) {
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        static_cast<PanelController *>(param)->repaint.store(true);
    }
}

void PanelController::tick() {
    if (!started) {
        return;
    }
    u64 tick_start = svcGetSystemTick();
    uint64_t now = osGetTime();

    // touches first, so a tap and the state it changes land in one frame
    TouchEvent ev[32];
    int n = 0;
    LightLock_Lock(&g_touch_lock);
    n = g_touch_n;
    memcpy(ev, g_touch, sizeof(TouchEvent) * n);
    g_touch_n = 0;
    LightLock_Unlock(&g_touch_lock);
    bool on = active.load();
    // Only a panel on screen may hide the game's textbox: in any other mode
    // (mirroring the stream, gamepad) that box is the only text there is.
    link.set_reading(on);
    for (int i = 0; i < n; i++) {
        // releases are always delivered, so nothing stays "held" when the
        // mode changed in the middle of a press
        switch (ev[i].kind) {
        case EV_TOUCH_DOWN:
            if (on) ui.touch_down(ev[i].x, ev[i].y, ev[i].t);
            break;
        case EV_TOUCH_MOVE:
            if (on) ui.touch_move(ev[i].x, ev[i].y, ev[i].t);
            break;
        case EV_TOUCH_UP:
            ui.touch_up(ev[i].t);
            break;
        case EV_KEY_DOWN:
            if (on) ui.key_down((PanelKey)ev[i].x, ev[i].t);
            break;
        case EV_KEY_UP:
            ui.key_up((PanelKey)ev[i].x, ev[i].t);
            break;
        }
    }

    // The UI follows the relay even while hidden, so it is current the
    // moment the panel comes back.
    if (link.snapshot_if_changed(snap.rev, snap)) {
        ui.update(snap, now);
    }
    ui.tick(now);
    follow_game_screen(now);
    if (want_mirror) {
        want_mirror = false;
        if (on) {
            // asked for from the panel: undo it like an automatic switch
            // once the game is back on the story or the title
            auto_mirrored = stable_state != 0;
            set_mode(N3dsTouchType::ABSOLUTE_TOUCH);
        }
    }

    if (!on) {
        diag::add(diag::PANEL, diag::us_since(tick_start));
        diag::flush_if_due();
        return;
    }
    if (repaint.exchange(false)) {
        repaint_until = now + kRepaintWindowMs;
        ui.invalidate();
    }
    if (ui.needs_redraw() && now - last_render >= kFrameMs) {
        {
            diag::Timer render_timer(diag::RENDER);
            ui.render(frame.data(), now);
        }
        last_render = now;
        {
            diag::Timer blit_timer(diag::BLIT);
            blit();
        }
    } else if (now < repaint_until) {
        diag::Timer blit_timer(diag::BLIT);
        blit();
    }
    diag::add(diag::PANEL, diag::us_since(tick_start));
    diag::flush_if_due();
}

void PanelController::blit() {
    u16 fb_w = 0, fb_h = 0;
    u16 *dst = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fb_w, &fb_h);
    if (!dst || fb_w != H || fb_h != W) {
        return;   // not the 240x320 portrait buffer the 3DS uses
    }
    // The framebuffer is the display rotated 90 degrees: memory column fx is
    // display row H-1-fx, memory row fy is display column fy.
    const uint16_t *src = frame.data();
    for (int fy = 0; fy < W; fy++) {
        u16 *row = dst + (size_t)fy * H;
        const uint16_t *col = src + fy;
        for (int fx = 0; fx < H; fx++) {
            row[fx] = col[(size_t)(H - 1 - fx) * W];
        }
    }
    GSPGPU_FlushDataCache(dst, (u32)W * H * sizeof(u16));
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

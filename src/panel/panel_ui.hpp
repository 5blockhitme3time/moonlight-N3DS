/*
 * The bottom-screen dialogue panel: layout, touch handling and drawing.
 *
 *   +------------------------------------------------+
 *   | ● 已连接                               [自动中]  |  status
 *   |  天使：我说你啊，以后最好多加小心哦？            |
 *   |  伊织：诶，居然是随口说的？                      |  history (small)
 *   |  [伊织]                                          |
 *   |  「这样岂不是让深受感动的我很丢脸」 ▼            |  current line
 *   |                                                  |
 *   | [历史][自动][快进][语音][快存][快读][菜单]       |  buttons
 *   +------------------------------------------------+
 *
 * 菜单 (or Start) replaces the log with the game's own menu: settings, save,
 * load, back to the title, or operating the game screen directly. On the
 * title screen the title menu shows there instead. The game's settings and
 * save/load screens are full-screen pages made for a mouse, so the
 * controller switches the bottom screen to mirroring while one is open.
 *
 * The text area is one scrollable log: the newest line sits at the bottom in
 * the large font, everything before it above in a smaller one. Dragging
 * scrolls back through it; a tap advances the game, or returns to the newest
 * line when scrolled back. When the game shows a choice, the options replace
 * the log until one is picked.
 *
 * Nothing here touches the 3DS: time is passed in, commands go out through a
 * callback, and drawing targets a plain RGB565 buffer. PanelController does
 * the platform side, and _dev/tools/panel_preview renders and tests this on
 * the PC.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../relay/relay_protocol.hpp"
#include "panel_draw.hpp"
#include "panel_input.hpp"

struct PanelFonts {
    PanelFont *body = nullptr;    // current line
    PanelFont *small = nullptr;   // history, buttons, options
    PanelFont *tiny = nullptr;    // status bar, names, hints
};

class PanelUI {
  public:
    static const int kWidth = 320;
    static const int kHeight = 240;
    typedef std::function<void(const char *cmd, int arg, int sel)> Sender;

    PanelUI(const PanelFonts &fonts, Sender send);

    // "host:port" of the relay, shown while it cannot be reached.
    void set_relay_label(const std::string &label) { relay_label = label; }

    // A short message from the platform side (e.g. "back from mirroring").
    void notify(const std::string &text, uint64_t now) { show_toast(text, now); }

    void update(const relay::Snapshot &snap, uint64_t now_ms);
    void touch_down(int x, int y, uint64_t now_ms);
    void touch_move(int x, int y, uint64_t now_ms);
    void touch_up(uint64_t now_ms);
    // Console buttons (see PanelKey). Up/down repeat while held.
    void key_down(PanelKey key, uint64_t now_ms);
    void key_up(PanelKey key, uint64_t now_ms);
    void tick(uint64_t now_ms);

    bool needs_redraw() const { return dirty; }
    void invalidate() { dirty = true; }
    void render(uint16_t *fb, uint64_t now_ms);   // kWidth x kHeight

    // inspection, for tests and the preview tool
    int scroll_pos() const { return (int)(scroll + 0.5f); }
    int scroll_max() const { return max_scroll; }
    const std::string &toast() const { return toast_text; }
    bool toast_visible(uint64_t now) const { return now < toast_until; }
    bool qload_armed(uint64_t now) const { return now < armed_until; }
    bool showing_choices() const { return choice_mode(); }
    bool new_lines_below() const { return new_below; }
    int focused_choice() const { return focus; }
    bool menu_shown() const { return menu_open; }
    bool title_shown() const { return title_mode(); }
    int focused_tile() const { return tile_focus; }

  private:
    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
        bool hit(int px, int py, int slop = 0) const {
            return px >= x - slop && px < x + w + slop && py >= y - slop &&
                   py < y + h + slop;
        }
    };
    struct Block {
        int id = -1;        // history id; -1 for the line being typed
        int kind = 0;       // 0 history line, 1 divider, 2 current line
        bool big = true;    // current line in the body font
        std::string name, text;
        std::vector<std::string> lines;
        int name_w = 0;     // history: width of the "name：" prefix
        int height = 0;
        int y = 0;          // top, in content coordinates
    };
    struct ChoiceBar {
        int index = 0;
        Rect r;
        std::vector<std::string> lines;
        bool enabled = true, read = false;
    };
    // One entry of the menu sheet or the title screen.
    struct Tile {
        const char *label;
        const char *cmd;   // relay command, or "@mirror" (handled locally)
        int bit;           // availability bit in Snapshot::buttons, 0 = always
        bool confirm;      // needs a second tap
        Rect r;
    };
    enum Target { T_NONE, T_TEXT, T_BUTTON, T_CHOICE, T_PILL, T_TILE };

    void layout();
    void layout_choices();
    Block make_history(const relay::HistoryEntry &e);
    Block make_current(int id, const std::string &name,
                       const std::string &text);
    bool choice_mode() const;
    bool waiting() const;
    int content_top() const;
    void clamp_scroll();
    void scroll_to(float target);

    void show_toast(const std::string &text, uint64_t now);
    void press_button(int b, uint64_t now);
    void press_choice(int i, uint64_t now);
    void tap_text(uint64_t now);
    void step_key(PanelKey key, uint64_t now);
    void move_focus(int dir);

    bool title_mode() const;
    bool tile_mode() const { return title_mode() || menu_open; }
    void layout_tiles();
    bool tile_enabled(const Tile &t) const;
    void press_tile(int i, uint64_t now);
    void set_menu(bool open, int focus_to);
    void draw_tiles(Canvas &c, uint64_t now);
    bool button_enabled(int b) const;
    bool button_active(int b) const;
    const char *disabled_reason(int b) const;
    Rect button_rect(int b) const;
    bool pill_visible() const;
    Rect pill_rect() const;
    std::string status_label() const;

    void draw_status(Canvas &c);
    void draw_log(Canvas &c);
    void draw_choices(Canvas &c, uint64_t now);
    void draw_card(Canvas &c, const std::string &title,
                   const std::vector<std::string> &body);
    void draw_banner(Canvas &c, const std::string &text);
    void draw_pill(Canvas &c);
    void draw_buttons(Canvas &c, uint64_t now);
    void draw_toast(Canvas &c, uint64_t now);

    PanelFonts f;
    Sender send;
    std::string relay_label;
    relay::Snapshot snap;
    uint32_t hist_epoch = 0;
    uint32_t note_seq = 0;

    std::vector<Block> blocks;
    std::unordered_map<int, Block> hist_cache;   // wrapped history lines
    int content_h = 0;
    bool new_below = false;   // lines arrived while scrolled back

    std::vector<ChoiceBar> bars;
    int chose_idx = -1;
    uint64_t chose_until = 0;
    int focus = -1;           // option highlighted by the D-pad, -1 = none

    bool menu_open = false;   // the 菜单 sheet over the story
    std::vector<Tile> tiles;  // menu sheet or title screen, whichever shows
    int tile_focus = -1;
    int tile_armed = -1;      // tile waiting for its confirming second tap
    uint64_t tile_armed_until = 0;
    bool was_title = false;

    int held_key = -1;        // up/down while held, for repeat
    uint64_t repeat_at = 0;

    float scroll = 0;         // px scrolled back from the newest line
    int max_scroll = 0;
    float velocity = 0;       // px/ms, for flings
    bool flinging = false;
    bool animating = false;
    float anim_target = 0;
    uint64_t last_tick = 0;

    bool touching = false, dragging = false;
    Target target = T_NONE;
    int target_idx = -1;
    bool target_inside = false;
    int down_y = 0, last_y = 0;
    float drag_origin = 0;
    uint64_t down_t = 0, last_move_t = 0;

    std::string toast_text;
    uint64_t toast_until = 0;
    uint64_t armed_until = 0;
    int flash_btn = -1;
    uint64_t flash_until = 0;
    bool blink_on = true;

    bool dirty = true;
};

/*
 * The bottom-screen dialogue panel - implementation.
 */
#include "panel_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using relay::Snapshot;

namespace {

const int W = PanelUI::kWidth;
const int H = PanelUI::kHeight;

// geometry
const int kBarH = 18;
const int kStripH = 40;
const int kTextTop = kBarH;
const int kTextBottom = H - kStripH;
const int kViewH = kTextBottom - kTextTop;
const int kTextX = 10;
const int kTextW = W - kTextX - 14;   // leaves room for the scrollbar
const int kHang = 10;                 // closing punctuation may overhang
const int kPadTop = 8, kPadBottom = 10;
const int kGapHist = 5, kGapDivider = 9, kGapCurrent = 12;
const int kChipH = 18;

// buttons, in strip order
enum { B_HISTORY, B_AUTO, B_SKIP, B_VOICE, B_QSAVE, B_QLOAD, B_MENU, B_COUNT };
const char *kBtnLabel[B_COUNT] = {"历史", "自动", "快进", "语音",
                                  "快存", "快读", "菜单"};
const char *kBtnCmd[B_COUNT] = {"", "auto", "skip", "voice", "qsave", "qload", ""};
const int kBtnBit[B_COUNT] = {0, relay::BTN_AUTO, relay::BTN_SKIP,
                              relay::BTN_VOICE, relay::BTN_QSAVE,
                              relay::BTN_QLOAD, 0};

// The 菜单 sheet over the story, and the title screen's own menu.
const int kMenuBits = relay::BTN_CONFIG | relay::BTN_SAVE | relay::BTN_LOAD |
                      relay::BTN_TITLE;
const struct {
    const char *label, *cmd;
    int bit;
    bool confirm;
} kMenuTiles[] = {
    {"游戏设置", "config", relay::BTN_CONFIG, false},
    {"存档", "save", relay::BTN_SAVE, false},
    {"读档", "load", relay::BTN_LOAD, false},
    {"回到标题", "title", relay::BTN_TITLE, true},
    {"直接操作游戏画面", "@mirror", 0, false},
}, kTitleTiles[] = {
    {"继续游戏", "continue", relay::BTN_CONTINUE, false},
    {"开始游戏", "newgame", relay::BTN_NEWGAME, false},
    {"读取存档", "load", relay::BTN_LOAD, false},
    {"游戏设置", "config", relay::BTN_CONFIG, false},
};

// timing
const int kDragSlop = 8;
const uint64_t kTapMaxMs = 600;   // longer presses are a finger resting
const uint64_t kToastMs = 2200;
const uint64_t kArmMs = 2500;     // quick-load confirmation window
const uint64_t kChooseLockMs = 3000;
const uint64_t kFlashMs = 160;

// palette
const Color kBg = rgb(17, 20, 26);
const Color kBar = rgb(24, 28, 36);
const Color kBarLine = rgb(40, 46, 56);
const Color kText = rgb(236, 239, 244);
const Color kTextHist = rgb(140, 149, 162);
const Color kNameHist = rgb(178, 148, 94);
const Color kName = rgb(255, 200, 100);
const Color kChip = rgb(60, 47, 26);
const Color kDivider = rgb(54, 60, 70);
const Color kDividerText = rgb(104, 112, 124);
const Color kBtn = rgb(36, 42, 52);
const Color kBtnPress = rgb(66, 76, 92);
const Color kBtnOn = rgb(40, 110, 96);
const Color kSkipOn = rgb(150, 96, 40);
const Color kBtnText = rgb(216, 222, 230);
const Color kBtnDim = rgb(88, 96, 108);
const Color kWarn = rgb(176, 60, 60);
const Color kChoice = rgb(60, 36, 50);
const Color kChoiceEdge = rgb(182, 98, 128);
const Color kChoicePress = rgb(112, 54, 82);
const Color kChoiceOff = rgb(36, 34, 38);
const Color kChoiceRead = rgb(255, 196, 142);
const Color kToastBg = rgb(234, 237, 242);
const Color kToastText = rgb(22, 26, 32);
const Color kPill = rgb(40, 110, 96);
const Color kCard = rgb(28, 33, 42);
const Color kCardEdge = rgb(56, 64, 76);
const Color kBanner = rgb(58, 48, 28);
const Color kScroll = rgb(96, 104, 116);
const Color kOk = rgb(88, 200, 120);
const Color kMid = rgb(240, 180, 60);
const Color kBad = rgb(220, 80, 80);

}  // namespace

PanelUI::PanelUI(const PanelFonts &fonts, Sender sender)
    : f(fonts), send(std::move(sender)) {}

// ------------------------------------------------------------ state ----

bool PanelUI::choice_mode() const {
    return snap.relay_up && snap.game_up && snap.loc == "play" &&
           snap.select >= 0 && !snap.options.empty();
}

bool PanelUI::waiting() const {
    return snap.relay_up && snap.game_up && snap.loc == "play" &&
           !menu_open && !snap.typing && !choice_mode() && !snap.auto_mode &&
           !snap.skip_mode && scroll <= 0.5f && !blocks.empty() &&
           blocks.back().kind == 2;
}

void PanelUI::update(const Snapshot &s, uint64_t now) {
    int prev_last = snap.history.empty() ? 0 : snap.history.back().id;
    bool prev_typing = snap.typing;
    int prev_sel = snap.select;
    bool was_choice = choice_mode();

    snap = s;
    layout();

    int last = snap.history.empty() ? 0 : snap.history.back().id;
    if (scroll > 0.5f && (last > prev_last || (snap.typing && !prev_typing))) {
        new_below = true;
    }
    if (snap.note_seq != note_seq) {
        note_seq = snap.note_seq;
        if (!snap.note.empty()) {
            show_toast(snap.note, now);
        }
    }
    if (snap.select != prev_sel) {
        chose_idx = -1;
        focus = -1;
    }
    if (choice_mode()) {
        layout_choices();
        if (focus >= (int)bars.size() ||
            (focus >= 0 && !bars[focus].enabled)) {
            focus = -1;
        }
        if (!was_choice) {
            scroll = 0;
            flinging = animating = false;
        }
    } else {
        bars.clear();
    }
    if (armed_until && !(snap.buttons & relay::BTN_QLOAD)) {
        armed_until = 0;
    }
    // the 菜单 sheet belongs to the story; it closes when the game leaves it
    if (menu_open && !(snap.relay_up && snap.game_up && snap.loc == "play")) {
        menu_open = false;
        tile_armed = -1;
    }
    bool is_title = title_mode();
    if (is_title != was_title) {
        was_title = is_title;
        tile_focus = -1;
        tile_armed = -1;
    }
    layout_tiles();
    dirty = true;
}

// ----------------------------------------------------------- layout ----

PanelUI::Block PanelUI::make_history(const relay::HistoryEntry &e) {
    Block b;
    b.id = e.id;
    b.name = e.name;
    b.text = e.text;
    if (e.divider) {
        b.kind = 1;
        b.height = f.tiny->line_height();
        return b;
    }
    b.kind = 0;
    if (!e.name.empty()) {
        b.name_w = f.small->text_width((e.name + "：").c_str());
    }
    b.lines = wrap_text(*f.small, e.text, kTextW, kTextW - b.name_w, kHang);
    b.height = (int)b.lines.size() * f.small->line_height();
    return b;
}

PanelUI::Block PanelUI::make_current(int id, const std::string &name,
                                     const std::string &text) {
    Block b;
    b.id = id;
    b.kind = 2;
    b.name = name;
    b.text = text;
    b.lines = wrap_text(*f.body, text, kTextW, kTextW, kHang);
    // An unusually long line drops to the smaller face rather than pushing
    // its own beginning off the top.
    if (b.lines.size() > 5) {
        b.big = false;
        b.lines = wrap_text(*f.small, text, kTextW, kTextW, kHang);
    }
    PanelFont &bf = b.big ? *f.body : *f.small;
    b.height = (name.empty() ? 0 : kChipH + 3) +
               (int)b.lines.size() * bf.line_height();
    return b;
}

int PanelUI::content_top() const {
    // bottom-anchored: with no scroll the content's end meets the view's end
    return kTextTop + kViewH - content_h + (int)(scroll + 0.5f);
}

void PanelUI::clamp_scroll() {
    if (scroll < 0) {
        scroll = 0;
    }
    if (scroll > max_scroll) {
        scroll = (float)max_scroll;
    }
}

void PanelUI::layout() {
    // While scrolled back, keep the block at the top of the view where it is,
    // so text arriving below does not move what is being read.
    int anchor_id = -1, anchor_y = 0;
    if (scroll > 0.5f) {
        int top = content_top();
        for (const Block &b : blocks) {
            if (b.id >= 0 && top + b.y + b.height > kTextTop) {
                anchor_id = b.id;
                anchor_y = top + b.y;
                break;
            }
        }
    }
    if (snap.history_epoch != hist_epoch) {
        hist_epoch = snap.history_epoch;
        hist_cache.clear();
        scroll = 0;
        anchor_id = -1;
    }

    const std::vector<relay::HistoryEntry> &h = snap.history;
    size_t n_hist = h.size();
    int cur_id = -2;
    std::string cur_name, cur_text;
    if (snap.typing) {
        cur_id = -1;
        cur_name = snap.cur_name;
        cur_text = snap.cur_text;
    } else if (n_hist && !h.back().divider) {
        // the newest finished line is the one on screen in the game
        cur_id = h.back().id;
        cur_name = h.back().name;
        cur_text = h.back().text;
        n_hist--;
    }

    blocks.clear();
    blocks.reserve(n_hist + 1);
    for (size_t i = 0; i < n_hist; i++) {
        auto it = hist_cache.find(h[i].id);
        if (it == hist_cache.end()) {
            it = hist_cache.emplace(h[i].id, make_history(h[i])).first;
        }
        blocks.push_back(it->second);
    }
    if (cur_id != -2) {
        blocks.push_back(make_current(cur_id, cur_name, cur_text));
    }
    if (hist_cache.size() > h.size() + 32) {
        std::unordered_map<int, Block> keep;
        for (const Block &b : blocks) {
            if (b.kind != 2) {
                keep.emplace(b.id, b);
            }
        }
        hist_cache.swap(keep);
    }

    int y = kPadTop;
    for (size_t i = 0; i < blocks.size(); i++) {
        if (i > 0) {
            const Block &p = blocks[i - 1], &b = blocks[i];
            y += b.kind == 2 ? kGapCurrent
                 : (b.kind == 1 || p.kind == 1) ? kGapDivider
                                                : kGapHist;
        }
        blocks[i].y = y;
        y += blocks[i].height;
    }
    content_h = y + kPadBottom;
    max_scroll = std::max(0, content_h - kViewH);

    if (anchor_id >= 0) {
        for (const Block &b : blocks) {
            if (b.id == anchor_id) {
                scroll = (float)(anchor_y - (kTextTop + kViewH - content_h + b.y));
                break;
            }
        }
    }
    clamp_scroll();
}

void PanelUI::layout_choices() {
    bars.clear();
    int n = (int)snap.options.size();
    int bw = W - 28;
    int lh = f.small->line_height();
    std::vector<std::vector<std::string>> caps;
    for (const relay::Option &o : snap.options) {
        std::string text = o.enabled ? o.text : o.text + "（条件不足）";
        std::vector<std::string> lines =
            wrap_text(*f.small, text, bw - 24, bw - 24, 0);
        if (lines.size() > 2) {
            std::string rest;
            for (size_t i = 1; i < lines.size(); i++) {
                rest += lines[i];
            }
            lines.resize(2);
            lines[1] = ellipsize(*f.small, rest, bw - 24);
        }
        caps.push_back(lines);
    }
    int top = kTextTop + 24, avail = kTextBottom - 6 - top;
    auto total = [&](int pad, int minh, int gap) {
        int t = gap * (n - 1);
        for (const auto &c : caps) {
            t += std::max(minh, (int)c.size() * lh + pad);
        }
        return t;
    };
    int pad = 12, minh = 36, gap = 8;
    if (total(pad, minh, gap) > avail) {
        minh = 0;
        gap = 5;
    }
    if (total(pad, minh, gap) > avail) {
        pad = 4;
        gap = 3;
    }
    int y = top + std::max(0, (avail - total(pad, minh, gap)) / 2);
    for (int i = 0; i < n; i++) {
        ChoiceBar bar;
        bar.index = snap.options[i].index;
        bar.enabled = snap.options[i].enabled;
        bar.read = snap.options[i].read;
        bar.lines = caps[i];
        bar.r.x = 14;
        bar.r.y = y;
        bar.r.w = bw;
        bar.r.h = std::max(minh, (int)caps[i].size() * lh + pad);
        y += bar.r.h + gap;
        bars.push_back(bar);
    }
}

// ------------------------------------------------------------ input ----

PanelUI::Rect PanelUI::button_rect(int b) const {
    // spread the leftover pixels so the strip fills the width exactly
    int x0 = 4 + b * (W - 8) / B_COUNT;
    int x1 = 4 + (b + 1) * (W - 8) / B_COUNT;
    Rect r;
    r.x = x0 + 2;
    r.y = kTextBottom + 4;
    r.w = x1 - x0 - 4;
    r.h = kStripH - 8;
    return r;
}

bool PanelUI::pill_visible() const { return !choice_mode() && scroll > 0.5f; }

PanelUI::Rect PanelUI::pill_rect() const {
    const char *t = new_below ? "有新台词" : "回到最新";
    Rect r;
    r.w = f.tiny->text_width(t) + 30;
    r.h = 22;
    r.x = (W - r.w) / 2;
    r.y = kTextBottom - r.h - 6;
    return r;
}

bool PanelUI::button_enabled(int b) const {
    if (b == B_HISTORY) {
        return true;
    }
    if (!snap.relay_up || !snap.game_up) {
        return false;
    }
    if (b == B_MENU) {
        return snap.loc == "play" && (snap.buttons & kMenuBits) != 0;
    }
    // a mode that is on can always be switched off again
    if ((b == B_AUTO && snap.auto_mode) || (b == B_SKIP && snap.skip_mode)) {
        return true;
    }
    return (snap.buttons & kBtnBit[b]) != 0;
}

bool PanelUI::button_active(int b) const {
    switch (b) {
    case B_HISTORY:
        return scroll > 0.5f;
    case B_AUTO:
        return snap.game_up && snap.auto_mode;
    case B_SKIP:
        return snap.game_up && snap.skip_mode;
    case B_MENU:
        return menu_open;
    default:
        return false;
    }
}

const char *PanelUI::disabled_reason(int b) const {
    if (!snap.relay_up) {
        return "还没连上电脑上的中继";
    }
    if (!snap.game_up) {
        return "游戏没有在运行";
    }
    if (snap.loc == "dialog") {
        return "请先处理电脑上的确认框";
    }
    if (b == B_MENU && snap.loc == "title") {
        return "标题菜单就在上面";
    }
    if (snap.loc != "play") {
        return "请先回到游戏正文";
    }
    if (choice_mode() && (b == B_AUTO || b == B_SKIP || b == B_VOICE)) {
        return "请先做出选择";
    }
    if (b == B_AUTO && snap.skip_mode) {
        return "请先停止快进";
    }
    if (b == B_SKIP && snap.auto_mode) {
        return "请先停止自动";
    }
    if (b == B_VOICE) {
        return "这句没有语音";
    }
    if (b == B_QLOAD) {
        return "还没有快速存档";
    }
    return "现在不能用";
}

void PanelUI::show_toast(const std::string &text, uint64_t now) {
    toast_text = text;
    toast_until = now + kToastMs;
    dirty = true;
}

void PanelUI::scroll_to(float t) {
    anim_target = std::max(0.0f, std::min((float)max_scroll, t));
    animating = true;
    flinging = false;
    dirty = true;
}

void PanelUI::press_button(int b, uint64_t now) {
    if (b == B_MENU && menu_open) {
        set_menu(false, -1);
        return;
    }
    if (b == B_MENU && button_enabled(b)) {
        set_menu(true, -1);
        return;
    }
    if (b == B_HISTORY && menu_open) {
        set_menu(false, -1);   // back to the log, then history as usual
    }
    if (b == B_HISTORY) {
        if (scroll > 0.5f) {
            scroll_to(0);
        } else if (max_scroll > 0) {
            scroll_to(scroll + kViewH * 0.8f);
        } else {
            show_toast("还没有更早的台词", now);
        }
        return;
    }
    if (!button_enabled(b)) {
        show_toast(disabled_reason(b), now);
        return;
    }
    if (b == B_QLOAD) {
        // Loading throws away everything since the save: ask once more.
        if (now >= armed_until) {
            armed_until = now + kArmMs;
            show_toast("再点一次读取快速存档", now);
            return;
        }
        armed_until = 0;
    }
    send(kBtnCmd[b], 0, -1);
    flash_btn = b;
    flash_until = now + kFlashMs;
}

void PanelUI::press_choice(int i, uint64_t now) {
    if (i < 0 || i >= (int)bars.size()) {
        return;
    }
    if (chose_idx >= 0 && now < chose_until) {
        return;   // already sent; waiting for the game to move on
    }
    const ChoiceBar &bar = bars[i];
    if (!bar.enabled) {
        show_toast("条件不足，选不了这个", now);
        return;
    }
    send("choose", bar.index, snap.select);
    chose_idx = bar.index;
    chose_until = now + kChooseLockMs;
}

void PanelUI::tap_text(uint64_t now) {
    if (scroll > 0.5f) {
        scroll_to(0);
        return;
    }
    if (!snap.relay_up) {
        // the relay link backs off while nobody answers; a tap retries now
        send("@retry", 0, -1);
        return;
    }
    if (!snap.game_up) {
        return;   // the card already explains what is wrong
    }
    if (menu_open) {
        set_menu(false, -1);   // a tap beside the menu closes it
        return;
    }
    if (snap.loc == "dialog" || snap.loc == "menu") {
        // the banner says so: a game screen or PC dialog is open, and the
        // way to answer it is to operate the game screen itself
        send("@mirror", 0, -1);
        return;
    }
    if (snap.loc != "play" || choice_mode()) {
        return;
    }
    send("click", 0, -1);
}

void PanelUI::touch_down(int x, int y, uint64_t now) {
    focus = -1;   // the finger is in charge now
    tile_focus = -1;
    touching = true;
    dragging = false;
    flinging = animating = false;
    velocity = 0;
    down_y = last_y = y;
    down_t = last_move_t = now;
    target = T_NONE;
    target_idx = -1;
    if (y >= kTextBottom) {
        for (int b = 0; b < B_COUNT; b++) {
            if (button_rect(b).hit(x, y, 2)) {
                target = T_BUTTON;
                target_idx = b;
                break;
            }
        }
    } else if (y >= kTextTop) {
        if (tile_mode()) {
            target = T_TEXT;   // beside the tiles: closes the menu
            for (size_t i = 0; i < tiles.size(); i++) {
                if (tiles[i].r.hit(x, y, 2)) {
                    target = T_TILE;
                    target_idx = (int)i;
                    break;
                }
            }
        } else if (choice_mode()) {
            for (size_t i = 0; i < bars.size(); i++) {
                if (bars[i].r.hit(x, y, 2)) {
                    target = T_CHOICE;
                    target_idx = (int)i;
                    break;
                }
            }
        } else if (pill_visible() && pill_rect().hit(x, y, 4)) {
            target = T_PILL;
        } else {
            target = T_TEXT;
        }
    }
    target_inside = true;
    dirty = true;
}

void PanelUI::touch_move(int x, int y, uint64_t now) {
    if (!touching) {
        return;
    }
    if (target == T_TEXT) {
        if (tile_mode()) {
            // nothing to scroll; a drag beside the menu is not a tap either
            if (std::abs(y - down_y) > kDragSlop) {
                target_inside = false;
            }
        } else if (!dragging && std::abs(y - down_y) > kDragSlop) {
            dragging = true;
            down_y = y;   // start 1:1 from here, without the slop jump
            drag_origin = scroll;
        }
        if (dragging) {
            float dt = (float)(now - last_move_t);
            if (dt > 0) {
                velocity = velocity * 0.6f + ((float)(y - last_y) / dt) * 0.4f;
            }
            scroll = drag_origin + (float)(y - down_y);
            clamp_scroll();
            dirty = true;
        }
    } else if (target != T_NONE) {
        bool in = false;
        if (target == T_BUTTON) {
            in = button_rect(target_idx).hit(x, y, 10);
        } else if (target == T_CHOICE && target_idx < (int)bars.size()) {
            in = bars[target_idx].r.hit(x, y, 10);
        } else if (target == T_PILL) {
            in = pill_rect().hit(x, y, 10);
        } else if (target == T_TILE && target_idx < (int)tiles.size()) {
            in = tiles[target_idx].r.hit(x, y, 10);
        }
        if (in != target_inside) {
            target_inside = in;
            dirty = true;
        }
    }
    last_y = y;
    last_move_t = now;
}

void PanelUI::touch_up(uint64_t now) {
    if (!touching) {
        return;
    }
    touching = false;
    dirty = true;
    if (dragging) {
        dragging = false;
        target = T_NONE;
        if (now - last_move_t < 80 && std::fabs(velocity) > 0.25f) {
            flinging = true;
        }
        return;
    }
    Target t = target;
    int idx = target_idx;
    bool in = target_inside;
    target = T_NONE;
    target_idx = -1;
    if (!in) {
        return;
    }
    switch (t) {
    case T_BUTTON:
        press_button(idx, now);
        break;
    case T_CHOICE:
        press_choice(idx, now);
        break;
    case T_PILL:
        scroll_to(0);
        break;
    case T_TILE:
        press_tile(idx, now);
        break;
    case T_TEXT:
        if (now - down_t <= kTapMaxMs) {
            tap_text(now);
        }
        break;
    default:
        break;
    }
}

// ------------------------------------------------- menu and title ----

bool PanelUI::title_mode() const {
    return snap.relay_up && snap.game_up && snap.loc == "title";
}

void PanelUI::set_menu(bool open, int focus_to) {
    menu_open = open;
    tile_focus = focus_to;
    tile_armed = -1;
    if (open) {
        scroll = 0;
        flinging = animating = false;
    }
    layout_tiles();
    dirty = true;
}

void PanelUI::layout_tiles() {
    tiles.clear();
    bool title = title_mode();
    if (!title && !menu_open) {
        return;
    }
    size_t n = title ? sizeof(kTitleTiles) / sizeof(kTitleTiles[0])
                     : sizeof(kMenuTiles) / sizeof(kMenuTiles[0]);
    int top = kTextTop + 22, bottom = kTextBottom - 6;
    int gap = 5;
    int th = std::min(34, (bottom - top - gap * ((int)n - 1)) / (int)n);
    int y = top + std::max(0, (bottom - top - (int)n * th - gap * ((int)n - 1)) / 2);
    for (size_t i = 0; i < n; i++) {
        Tile t;
        if (title) {
            t.label = kTitleTiles[i].label;
            t.cmd = kTitleTiles[i].cmd;
            t.bit = kTitleTiles[i].bit;
            t.confirm = kTitleTiles[i].confirm;
        } else {
            t.label = kMenuTiles[i].label;
            t.cmd = kMenuTiles[i].cmd;
            t.bit = kMenuTiles[i].bit;
            t.confirm = kMenuTiles[i].confirm;
        }
        t.r.x = 24;
        t.r.y = y;
        t.r.w = W - 48;
        t.r.h = th;
        y += th + gap;
        tiles.push_back(t);
    }
    if (tile_focus >= (int)tiles.size()) {
        tile_focus = -1;
    }
}

bool PanelUI::tile_enabled(const Tile &t) const {
    return t.bit == 0 || (snap.buttons & t.bit) != 0;
}

void PanelUI::press_tile(int i, uint64_t now) {
    if (i < 0 || i >= (int)tiles.size()) {
        return;
    }
    const Tile &t = tiles[i];
    if (!tile_enabled(t)) {
        const char *why = t.bit == relay::BTN_CONTINUE ? "没有可以继续的进度"
                          : (t.bit & (relay::BTN_SAVE | relay::BTN_LOAD)) &&
                                  !title_mode()
                              ? "回想中不能存读档"
                              : title_mode() ? "标题画面还没准备好"
                                             : "现在不能用";
        show_toast(why, now);
        return;
    }
    if (t.confirm && !(tile_armed == i && now < tile_armed_until)) {
        tile_armed = i;
        tile_armed_until = now + kArmMs;
        show_toast(std::string("再点一次") + t.label, now);
        dirty = true;
        return;
    }
    tile_armed = -1;
    send(t.cmd, 0, -1);
    if (menu_open) {
        set_menu(false, -1);
    }
}

void PanelUI::draw_tiles(Canvas &c, uint64_t now) {
    c.clip(0, kTextTop, W, kViewH);
    bool title = title_mode();
    c.text(*f.tiny, 24, kTextTop + 4, title ? "标题画面" : "菜单", kTextHist);
    const char *hint = title ? "也可在 HOME → MIRROR 里点" : "B 返回";
    int hw = f.tiny->text_width(hint);
    c.text(*f.tiny, W - 24 - hw, kTextTop + 4, hint, kDividerText);
    for (size_t i = 0; i < tiles.size(); i++) {
        const Tile &t = tiles[i];
        const Rect &r = t.r;
        bool en = tile_enabled(t);
        bool pressed = touching && target == T_TILE && target_idx == (int)i &&
                       target_inside;
        bool armed = tile_armed == (int)i && now < tile_armed_until;
        if (tile_focus == (int)i) {
            c.fill_round(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 10, kName);
            c.fill_round(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 8, kBg);
        }
        Color bg = armed ? kWarn : pressed ? kBtnPress : en ? kBtn : kChoiceOff;
        c.fill_round(r.x, r.y, r.w, r.h, 7, bg);
        const char *label = armed ? "再点一次确认" : t.label;
        int tw = f.small->text_width(label);
        c.text(*f.small, r.x + (r.w - tw) / 2,
               r.y + (r.h - f.small->line_height()) / 2, label,
               en ? kBtnText : kBtnDim);
    }
    c.unclip();
}

// ---------------------------------------------------- console buttons ----

void PanelUI::move_focus(int dir) {
    int n = (int)bars.size();
    if (n == 0) {
        return;
    }
    int i = focus;
    if (i < 0) {
        // the first press only lights an option up, so a player mashing
        // through text cannot pick one by accident
        i = dir > 0 ? -1 : n;
    }
    for (int k = 0; k < n; k++) {
        i += dir;
        if (i < 0 || i >= n) {
            break;   // stop at the ends rather than wrap
        }
        if (bars[i].enabled) {
            focus = i;
            dirty = true;
            return;
        }
    }
    if (focus < 0) {
        // nothing enabled in that direction: settle on any enabled option
        for (int k = 0; k < n; k++) {
            if (bars[k].enabled) {
                focus = k;
                dirty = true;
                return;
            }
        }
    }
}

void PanelUI::step_key(PanelKey key, uint64_t now) {
    int dir = key == PANEL_KEY_UP ? -1 : 1;
    if (tile_mode()) {
        int n = (int)tiles.size();
        tile_focus = tile_focus < 0 ? (dir > 0 ? 0 : n - 1)
                                    : std::max(0, std::min(n - 1, tile_focus + dir));
        dirty = true;
        return;
    }
    if (choice_mode()) {
        move_focus(dir);
        return;
    }
    // up = further back into history
    float from = animating ? anim_target : scroll;
    scroll_to(from - dir * (float)(f.small->line_height() * 2));
}

void PanelUI::key_down(PanelKey key, uint64_t now) {
    if (key == PANEL_KEY_MENU) {
        if (menu_open) {
            set_menu(false, -1);
        } else if (button_enabled(B_MENU)) {
            set_menu(true, 0);   // from a button, start on the first entry
        } else if (!title_mode()) {
            show_toast(disabled_reason(B_MENU), now);
        }
        flash_btn = B_MENU;
        flash_until = now + kFlashMs;
        dirty = true;
        return;
    }
    if (tile_mode()) {
        switch (key) {
        case PANEL_KEY_ADVANCE:
            if (tile_focus < 0) {
                tile_focus = 0;
            } else {
                press_tile(tile_focus, now);
            }
            dirty = true;
            return;
        case PANEL_KEY_BACK:
            if (menu_open) {
                set_menu(false, -1);
            }
            return;
        case PANEL_KEY_UP:
        case PANEL_KEY_DOWN:
            step_key(key, now);
            held_key = key;
            repeat_at = now + 350;
            return;
        default:
            break;   // L/R/X/Y keep their meaning
        }
    }
    switch (key) {
    case PANEL_KEY_ADVANCE:
        if (choice_mode()) {
            if (focus < 0) {
                move_focus(1);
            } else {
                press_choice(focus, now);
            }
        } else {
            tap_text(now);
        }
        break;
    case PANEL_KEY_BACK:
        if (scroll > 0.5f || animating) {
            scroll_to(0);
        }
        break;
    case PANEL_KEY_UP:
    case PANEL_KEY_DOWN:
        step_key(key, now);
        held_key = key;
        repeat_at = now + 350;
        break;
    case PANEL_KEY_HISTORY:
        press_button(B_HISTORY, now);
        flash_btn = B_HISTORY;
        flash_until = now + kFlashMs;
        break;
    case PANEL_KEY_SKIP:
    case PANEL_KEY_AUTO:
    case PANEL_KEY_VOICE: {
        int b = key == PANEL_KEY_SKIP ? B_SKIP
                : key == PANEL_KEY_AUTO ? B_AUTO
                                        : B_VOICE;
        press_button(b, now);
        // show which on-screen button the key stands for, even when it
        // could not act (press_button has put up the reason then)
        flash_btn = b;
        flash_until = now + kFlashMs;
        break;
    }
    default:
        break;
    }
    dirty = true;
}

void PanelUI::key_up(PanelKey key, uint64_t now) {
    if ((int)key == held_key) {
        held_key = -1;
    }
}

void PanelUI::tick(uint64_t now) {
    uint64_t dt = last_tick ? now - last_tick : 0;
    last_tick = now;
    if (held_key >= 0 && now >= repeat_at) {
        step_key((PanelKey)held_key, now);
        repeat_at = now + 90;
    }
    if (dt > 100) {
        dt = 100;
    }
    if (flinging && dt) {
        scroll += velocity * (float)dt;
        velocity *= std::exp(-(float)dt / 325.0f);
        if (std::fabs(velocity) < 0.02f || scroll <= 0 || scroll >= max_scroll) {
            flinging = false;
        }
        clamp_scroll();
        dirty = true;
    }
    if (animating && dt) {
        float k = std::min(1.0f, (float)dt / 90.0f);
        scroll += (anim_target - scroll) * k;
        if (std::fabs(anim_target - scroll) < 0.5f) {
            scroll = anim_target;
            animating = false;
        }
        clamp_scroll();
        dirty = true;
    }
    if (scroll <= 0.5f && new_below) {
        new_below = false;
        dirty = true;
    }
    if (toast_until) {
        if (now >= toast_until) {
            toast_until = 0;
            dirty = true;
        } else if (toast_until - now < 300) {
            dirty = true;   // fading out
        }
    }
    if (armed_until && now >= armed_until) {
        armed_until = 0;
        dirty = true;
    }
    if (flash_btn >= 0 && now >= flash_until) {
        flash_btn = -1;
        dirty = true;
    }
    if (tile_armed >= 0 && now >= tile_armed_until) {
        tile_armed = -1;
        dirty = true;
    }
    if (chose_idx >= 0 && now >= chose_until) {
        chose_idx = -1;
        dirty = true;
    }
    bool blink = (now % 1000) < 600;
    if (blink != blink_on) {
        blink_on = blink;
        if (waiting()) {
            dirty = true;
        }
    }
}

// ---------------------------------------------------------- drawing ----

std::string PanelUI::status_label() const {
    if (!snap.relay_up) {
        return "未连接中继";
    }
    if (snap.proto_mismatch) {
        return "版本不匹配";
    }
    if (!snap.game_up) {
        return snap.loc_label.empty() ? "游戏未运行" : snap.loc_label;
    }
    if (snap.loc == "play") {
        return menu_open ? "菜单" : choice_mode() ? "等待选择" : "已连接";
    }
    return snap.loc_label;
}

void PanelUI::draw_status(Canvas &c) {
    c.fill(0, 0, W, kBarH, kBar);
    c.hline(0, kBarH - 1, W, kBarLine);
    Color dot = !snap.relay_up || snap.proto_mismatch ? kBad
                : !snap.game_up                       ? kMid
                                                      : kOk;
    c.fill_round(6, 5, 7, 7, 3, dot);
    c.text(*f.tiny, 18, 1, status_label().c_str(), kTextHist);

    int x = W - 6;
    if (snap.game_up && (snap.auto_mode || snap.skip_mode)) {
        const char *t = snap.auto_mode ? "自动中" : "快进中";
        int tw = f.tiny->text_width(t);
        x -= tw + 12;
        c.fill_round(x, 2, tw + 12, 14, 7, snap.auto_mode ? kBtnOn : kSkipOn);
        c.text(*f.tiny, x + 6, 1, t, kText);
        x -= 6;
    }
    if (scroll > 0.5f && !choice_mode()) {
        const char *t = "回看中";
        int tw = f.tiny->text_width(t);
        x -= tw + 12;
        c.fill_round(x, 2, tw + 12, 14, 7, kBtn);
        c.text(*f.tiny, x + 6, 1, t, kTextHist);
    }
}

void PanelUI::draw_log(Canvas &c) {
    c.clip(0, kTextTop, W, kViewH);
    int top = content_top();
    bool wait = waiting();
    for (size_t bi = 0; bi < blocks.size(); bi++) {
        const Block &b = blocks[bi];
        int sy = top + b.y;
        if (sy >= kTextBottom || sy + b.height <= kTextTop) {
            continue;
        }
        if (b.kind == 1) {
            int mid = sy + f.tiny->line_height() / 2;
            if (b.text.empty()) {
                c.hline(W / 2 - 40, mid, 80, kDivider);
            } else {
                int tw = f.tiny->text_width(b.text.c_str());
                int tx = (W - tw) / 2;
                c.hline(kTextX + 20, mid, tx - 8 - (kTextX + 20), kDivider);
                c.hline(tx + tw + 8, mid, W - kTextX - 20 - (tx + tw + 8), kDivider);
                c.text(*f.tiny, tx, sy, b.text.c_str(), kDividerText);
            }
        } else if (b.kind == 0) {
            int y = sy;
            for (size_t i = 0; i < b.lines.size(); i++) {
                int x = kTextX;
                if (i == 0 && b.name_w) {
                    c.text(*f.small, x, y, (b.name + "：").c_str(), kNameHist);
                    x += b.name_w;
                }
                c.text(*f.small, x, y, b.lines[i].c_str(), kTextHist);
                y += f.small->line_height();
            }
        } else {
            int y = sy;
            if (!b.name.empty()) {
                int tw = f.tiny->text_width(b.name.c_str());
                c.fill_round(kTextX - 3, y, tw + 14, kChipH, 5, kChip);
                c.text(*f.tiny, kTextX + 4, y + 1, b.name.c_str(), kName);
                y += kChipH + 3;
            }
            PanelFont &bf = b.big ? *f.body : *f.small;
            for (const std::string &line : b.lines) {
                c.text(bf, kTextX, y, line.c_str(), kText);
                y += bf.line_height();
            }
            if (wait && blink_on && bi + 1 == blocks.size() && !b.lines.empty()) {
                // "click to continue", after the last character
                int lw = bf.text_width(b.lines.back().c_str());
                int mx = std::min(kTextX + lw + 9, W - 8);
                int my = y - bf.line_height() + bf.baseline() - 7;
                c.triangle_down(mx, my, 4, kName);
            }
        }
    }
    // soften the edge where older text is cut off
    for (int j = 0; j < 10; j++) {
        c.blend(0, kTextTop + j, W, 1, kBg, 230 - j * 23);
    }
    if (max_scroll > 0 && (scroll > 0.5f || dragging || flinging)) {
        int track = kViewH - 12;
        int th = std::max(16, track * kViewH / std::max(content_h, 1));
        float frac = max_scroll ? scroll / (float)max_scroll : 0;
        int ty = kTextTop + 6 + (int)((track - th) * (1.0f - frac));
        c.fill_round(W - 6, ty, 3, th, 1, kScroll);
    }
    c.unclip();
}

void PanelUI::draw_choices(Canvas &c, uint64_t now) {
    c.clip(0, kTextTop, W, kViewH);
    c.text(*f.tiny, 14, kTextTop + 4, "请选择", kTextHist);
    bool any_read = false;
    for (const ChoiceBar &bar : bars) {
        any_read = any_read || bar.read;
    }
    if (any_read) {
        const char *hint = "橙色为选过的";
        int tw = f.tiny->text_width(hint);
        c.text(*f.tiny, W - 14 - tw, kTextTop + 4, hint, kChoiceRead);
    }
    int lh = f.small->line_height();
    for (size_t i = 0; i < bars.size(); i++) {
        const ChoiceBar &bar = bars[i];
        const Rect &r = bar.r;
        bool pressed = touching && target == T_CHOICE &&
                       target_idx == (int)i && target_inside;
        bool chosen = chose_idx == bar.index && now < chose_until;
        Color edge = bar.enabled ? kChoiceEdge : kCardEdge;
        Color bg = (pressed || chosen) ? kChoicePress
                   : bar.enabled       ? kChoice
                                       : kChoiceOff;
        if (focus == (int)i) {
            // D-pad highlight: a ring outside the bar
            c.fill_round(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 11, kName);
            c.fill_round(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 9, kBg);
        }
        c.fill_round(r.x, r.y, r.w, r.h, 8, edge);
        c.fill_round(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, bg);
        Color fg = !bar.enabled ? kBtnDim : bar.read ? kChoiceRead : kText;
        int ty = r.y + (r.h - (int)bar.lines.size() * lh) / 2;
        for (const std::string &line : bar.lines) {
            int tw = f.small->text_width(line.c_str());
            c.text(*f.small, r.x + (r.w - tw) / 2, ty, line.c_str(), fg);
            ty += lh;
        }
    }
    c.unclip();
}

void PanelUI::draw_card(Canvas &c, const std::string &title,
                        const std::vector<std::string> &body) {
    int lt = f.small->line_height(), lb = f.tiny->line_height();
    int w = W - 32;
    int h = 12 + lt + 6 + (int)body.size() * lb + 12;
    int x = 16, y = kTextTop + (kViewH - h) / 2;
    c.fill_round(x, y, w, h, 8, kCardEdge);
    c.fill_round(x + 1, y + 1, w - 2, h - 2, 7, kCard);
    int ty = y + 12;
    int tw = f.small->text_width(title.c_str());
    c.text(*f.small, x + (w - tw) / 2, ty, title.c_str(), kText);
    ty += lt + 6;
    for (const std::string &line : body) {
        std::string s = ellipsize(*f.tiny, line, w - 20);
        int lw = f.tiny->text_width(s.c_str());
        c.text(*f.tiny, x + (w - lw) / 2, ty, s.c_str(), kTextHist);
        ty += lb;
    }
}

void PanelUI::draw_banner(Canvas &c, const std::string &text) {
    int y = kTextTop + 5;
    c.fill_round(8, y, W - 16, 22, 6, kBanner);
    std::string s = ellipsize(*f.tiny, text, W - 32);
    int tw = f.tiny->text_width(s.c_str());
    c.text(*f.tiny, (W - tw) / 2, y + 3, s.c_str(), kName);
}

void PanelUI::draw_pill(Canvas &c) {
    Rect r = pill_rect();
    bool pressed = touching && target == T_PILL && target_inside;
    c.fill_round(r.x, r.y, r.w, r.h, r.h / 2, pressed ? kBtnPress : kPill);
    c.triangle_down(r.x + 12, r.y + 8, 4, kText);
    c.text(*f.tiny, r.x + 21, r.y + 3, new_below ? "有新台词" : "回到最新",
           kText);
}

void PanelUI::draw_buttons(Canvas &c, uint64_t now) {
    c.fill(0, kTextBottom, W, kStripH, kBar);
    c.hline(0, kTextBottom, W, kBarLine);
    for (int b = 0; b < B_COUNT; b++) {
        Rect r = button_rect(b);
        bool en = button_enabled(b);
        bool act = button_active(b);
        bool pressed = (touching && target == T_BUTTON && target_idx == b &&
                        target_inside) ||
                       (flash_btn == b && now < flash_until);
        bool armed = b == B_QLOAD && now < armed_until;
        Color bg = armed ? kWarn
                   : pressed ? kBtnPress
                   : act ? (b == B_SKIP ? kSkipOn : kBtnOn)
                         : kBtn;
        Color fg = (en || act || armed) ? kBtnText : kBtnDim;
        const char *label = armed ? "确认" : kBtnLabel[b];
        c.fill_round(r.x, r.y, r.w, r.h, 6, bg);
        int tw = f.small->text_width(label);
        c.text(*f.small, r.x + (r.w - tw) / 2,
               r.y + (r.h - f.small->line_height()) / 2, label, fg);
    }
}

void PanelUI::draw_toast(Canvas &c, uint64_t now) {
    if (now >= toast_until) {
        return;
    }
    uint64_t left = toast_until - now;
    int alpha = left < 300 ? (int)(left * 255 / 300) : 255;
    std::string s = ellipsize(*f.small, toast_text, W - 48);
    int tw = f.small->text_width(s.c_str());
    int w = tw + 24, h = 26;
    int x = (W - w) / 2;
    // At the top, over the oldest (faded) text, so it never hides the line
    // being read or the choice header; below the banner when one is shown.
    bool banner = snap.relay_up && snap.game_up && snap.loc != "play" &&
                  !title_mode();
    int y = (choice_mode() || tile_mode()) ? kTextTop + 1
                                           : kTextTop + (banner ? 32 : 6);
    Color bg = mix(kBg, kToastBg, alpha);
    c.fill_round(x, y, w, h, h / 2, bg);
    c.text(*f.small, x + 12, y + (h - f.small->line_height()) / 2, s.c_str(),
           mix(bg, kToastText, alpha));
}

void PanelUI::render(uint16_t *fb, uint64_t now) {
    Canvas c(fb, W, H);
    c.fill(0, 0, W, H, kBg);
    dirty = false;
    if (!f.body || !f.body->ready() || !f.small->ready() || !f.tiny->ready()) {
        // without a font there is nothing legible to draw; a red band at
        // least tells the user the panel is alive but broken
        c.fill(0, H / 2 - 2, W, 4, kBad);
        return;
    }

    draw_status(c);
    if (tile_mode()) {
        draw_tiles(c, now);
    } else if (choice_mode()) {
        draw_choices(c, now);
    } else {
        draw_log(c);
        if (!snap.relay_up) {
            c.blend(0, kTextTop, W, kViewH, kBg, 190);
            draw_card(c, "连不上中继",
                      {"正在连接 " + relay_label,
                       "请在电脑上打开 KiriPanel，",
                       "并确认 3DS 和电脑在同一个网络。",
                       "打开后点这里立即重连。"});
        } else if (snap.proto_mismatch) {
            c.blend(0, kTextTop, W, kViewH, kBg, 190);
            draw_card(c, "版本不匹配",
                      {"电脑上的 KiriPanel 和本程序版本不同，", "请一起更新。"});
        } else if (!snap.game_up) {
            c.blend(0, kTextTop, W, kViewH, kBg, 190);
            if (snap.loc == "hookold") {
                draw_card(c, snap.loc_label,
                          {"游戏目录里的 AfterInit2.tjs 是旧版，",
                           "换成新版后重启游戏。"});
            } else {
                draw_card(c, snap.loc_label.empty() ? "游戏未运行" : snap.loc_label,
                          {"请在电脑上启动游戏。",
                           "台词会在开始游戏后出现在这里。"});
            }
        } else if (snap.loc == "dialog") {
            draw_banner(c, "游戏弹出了确认框 · 点这里直接操作");
        } else if (snap.loc != "play") {
            draw_banner(c, snap.loc_label + " · 点这里直接操作");
        } else if (blocks.empty()) {
            const char *t = "台词会显示在这里";
            int tw = f.tiny->text_width(t);
            c.text(*f.tiny, (W - tw) / 2, kTextTop + kViewH / 2 - 8, t,
                   kDividerText);
        }
        if (pill_visible()) {
            draw_pill(c);
        }
    }
    draw_toast(c, now);
    draw_buttons(c, now);

    f.body->trim();
    f.small->trim();
    f.tiny->trim();
}

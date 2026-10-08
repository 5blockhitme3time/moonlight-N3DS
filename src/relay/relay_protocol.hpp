/*
 * Wire format between the PC relay (relay.py) and the console, and the state
 * the console keeps from it. No networking here, so it builds and is tested
 * on the PC as well (_dev/tools/panel_preview).
 *
 * GET /panel.txt?epoch=E&rev=R&hist=H returns tab-separated lines:
 *
 *   v       2                      protocol version
 *   epoch   <n>                    changes when the relay restarts
 *   rev     <n>                    changes whenever anything visible changes
 *   same                           nothing changed since (E, R): stop here
 *   link    0|1                    is the game hook alive
 *   loc     play|title|menu|dialog|off  <label>
 *   mode    auto|skip|-
 *   btn     <bits>                 1 auto 2 skip 4 voice 8 qsave 16 qload
 *                                  32 settings 64 save 128 load 256 title
 *                                  512 continue 1024 new game
 *   cur     <name> <text>          the line being typed, if any
 *   hreset                         drop local history before the h lines
 *   h       <id> L <name> <text>   history line, ids only grow
 *   h       <id> D <text>          divider (load, backlog jump, restart)
 *   sel     <num>                  a choice is on screen
 *   opt     <idx> <flags> <text>   flags: e = enabled, r = read before
 *   note    <id> <text>            short message to show once
 *   end                            the response is complete
 *
 * Text fields escape backslash, tab and newline as \\ \t \n.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace relay {

const int kProtocol = 2;
const size_t kHistoryCap = 200;

enum ButtonBits {
    BTN_AUTO = 1,
    BTN_SKIP = 2,
    BTN_VOICE = 4,
    BTN_QSAVE = 8,
    BTN_QLOAD = 16,
    BTN_CONFIG = 32,     // the game's own screens...
    BTN_SAVE = 64,
    BTN_LOAD = 128,
    BTN_TITLE = 256,     // ...back to the title from the story
    BTN_CONTINUE = 512,  // title menu
    BTN_NEWGAME = 1024,
};

struct HistoryEntry {
    int id = 0;
    bool divider = false;
    std::string name;   // empty for narration
    std::string text;
};

struct Option {
    int index = 0;
    bool enabled = true;
    bool read = false;
    std::string text;
};

// What the console knows. `rev` here is local and bumps on every change,
// including the relay becoming unreachable, so the UI can compare one number.
struct Snapshot {
    uint32_t rev = 0;
    bool relay_up = false;
    bool proto_mismatch = false;
    bool game_up = false;
    std::string loc = "off";
    std::string loc_label;
    bool auto_mode = false;
    bool skip_mode = false;
    int buttons = 0;

    bool typing = false;
    std::string cur_name;
    std::string cur_text;

    std::vector<HistoryEntry> history;   // oldest first, ids increasing
    uint32_t history_epoch = 0;          // bumps on hreset

    int select = -1;                     // choice number, -1 when none
    std::vector<Option> options;

    uint32_t note_seq = 0;               // bumps when a new note arrives
    std::string note;
};

// One parsed response.
struct Update {
    bool complete = false;
    int proto = 0;
    long long epoch = -1;
    long long rev = -1;
    bool same = false;
    bool game_up = false;
    std::string loc = "off";
    std::string loc_label;
    std::string mode = "-";
    int buttons = 0;
    bool has_cur = false;
    std::string cur_name, cur_text;
    bool hreset = false;
    std::vector<HistoryEntry> history;
    int select = -1;
    std::vector<Option> options;
    int note_id = 0;
    std::string note;
};

// Where the console is in the relay's sequence; sent back on each poll.
struct SyncState {
    long long epoch = -1;
    long long rev = -1;
    int hist = 0;
    int note_id = 0;
};

std::string unescape(const std::string &s);
std::vector<std::string> split_tabs(const std::string &line);
Update parse(const std::string &body);

// Merges an update into the snapshot. Returns true if anything changed.
// Incomplete or mismatched responses are rejected without touching state.
bool apply(Snapshot &snap, SyncState &sync, const Update &u);

}  // namespace relay

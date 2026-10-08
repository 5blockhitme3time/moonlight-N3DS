/*
 * Wire format between the PC relay and the console - implementation.
 */
#include "relay_protocol.hpp"

#include <cstdlib>

namespace relay {

std::string unescape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out += n == 't' ? '\t' : n == 'n' ? '\n' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        size_t t = line.find('\t', start);
        f.push_back(line.substr(start, t == std::string::npos ? t : t - start));
        if (t == std::string::npos) {
            return f;
        }
        start = t + 1;
    }
}

static long long num(const std::vector<std::string> &f, size_t i,
                     long long dflt = 0) {
    return i < f.size() ? strtoll(f[i].c_str(), nullptr, 10) : dflt;
}

static std::string field(const std::vector<std::string> &f, size_t i) {
    return i < f.size() ? unescape(f[i]) : std::string();
}

Update parse(const std::string &body) {
    Update u;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos
                                                ? std::string::npos
                                                : nl - pos);
        pos = nl == std::string::npos ? body.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> f = split_tabs(line);
        const std::string &k = f[0];
        if (k == "v") {
            u.proto = (int)num(f, 1);
        } else if (k == "epoch") {
            u.epoch = num(f, 1, -1);
        } else if (k == "rev") {
            u.rev = num(f, 1, -1);
        } else if (k == "same") {
            u.same = true;
        } else if (k == "link") {
            u.game_up = num(f, 1) != 0;
        } else if (k == "loc") {
            u.loc = field(f, 1);
            u.loc_label = field(f, 2);
        } else if (k == "mode") {
            u.mode = field(f, 1);
        } else if (k == "btn") {
            u.buttons = (int)num(f, 1);
        } else if (k == "cur") {
            u.has_cur = true;
            u.cur_name = field(f, 1);
            u.cur_text = field(f, 2);
        } else if (k == "hreset") {
            u.hreset = true;
        } else if (k == "h") {
            HistoryEntry e;
            e.id = (int)num(f, 1);
            e.divider = field(f, 2) == "D";
            if (e.divider) {
                e.text = field(f, 3);
            } else {
                e.name = field(f, 3);
                e.text = field(f, 4);
            }
            if (e.id > 0) {
                u.history.push_back(e);
            }
        } else if (k == "sel") {
            u.select = (int)num(f, 1, -1);
        } else if (k == "opt") {
            Option o;
            o.index = (int)num(f, 1);
            std::string flags = field(f, 2);
            o.enabled = flags.find('e') != std::string::npos;
            o.read = flags.find('r') != std::string::npos;
            o.text = field(f, 3);
            u.options.push_back(o);
        } else if (k == "note") {
            u.note_id = (int)num(f, 1);
            u.note = field(f, 2);
        } else if (k == "end") {
            u.complete = true;
        }
        // unknown keys are ignored, so the relay can add fields freely
    }
    return u;
}

bool apply(Snapshot &snap, SyncState &sync, const Update &u) {
    if (!u.complete) {
        return false;   // truncated read; keep what we have
    }
    bool changed = false;
    if (u.proto != kProtocol) {
        if (!snap.proto_mismatch || !snap.relay_up) {
            snap.proto_mismatch = true;
            snap.relay_up = true;
            snap.game_up = false;
            snap.rev++;
        }
        sync = SyncState();
        return true;
    }
    if (!snap.relay_up || snap.proto_mismatch) {
        snap.relay_up = true;
        snap.proto_mismatch = false;
        changed = true;
    }
    if (u.same && u.epoch == sync.epoch) {
        if (changed) {
            snap.rev++;
        }
        return changed;
    }
    if (u.epoch != sync.epoch) {
        sync.note_id = 0;   // a new relay numbers its notes from 1 again
    }
    sync.epoch = u.epoch;
    sync.rev = u.rev;

    std::string mode = u.mode;
    bool auto_mode = mode == "auto", skip_mode = mode == "skip";
    if (snap.game_up != u.game_up || snap.loc != u.loc ||
        snap.loc_label != u.loc_label || snap.auto_mode != auto_mode ||
        snap.skip_mode != skip_mode || snap.buttons != u.buttons ||
        snap.typing != u.has_cur || snap.cur_name != u.cur_name ||
        snap.cur_text != u.cur_text || snap.select != u.select) {
        changed = true;
    }
    snap.game_up = u.game_up;
    snap.loc = u.loc;
    snap.loc_label = u.loc_label;
    snap.auto_mode = auto_mode;
    snap.skip_mode = skip_mode;
    snap.buttons = u.buttons;
    snap.typing = u.has_cur;
    snap.cur_name = u.has_cur ? u.cur_name : std::string();
    snap.cur_text = u.has_cur ? u.cur_text : std::string();
    snap.select = u.select;

    if (snap.options.size() != u.options.size()) {
        changed = true;
    } else {
        for (size_t i = 0; i < u.options.size(); i++) {
            const Option &a = snap.options[i], &b = u.options[i];
            if (a.index != b.index || a.enabled != b.enabled ||
                a.read != b.read || a.text != b.text) {
                changed = true;
            }
        }
    }
    snap.options = u.options;

    if (u.hreset) {
        snap.history.clear();
        snap.history_epoch++;
        sync.hist = 0;
        changed = true;
    }
    for (const HistoryEntry &e : u.history) {
        if (e.id <= sync.hist) {
            continue;
        }
        snap.history.push_back(e);
        sync.hist = e.id;
        changed = true;
    }
    if (snap.history.size() > kHistoryCap) {
        snap.history.erase(snap.history.begin(),
                           snap.history.end() - kHistoryCap);
    }

    if (u.note_id > sync.note_id && !u.note.empty()) {
        sync.note_id = u.note_id;
        snap.note = u.note;
        snap.note_seq++;
        changed = true;
    }

    if (changed) {
        snap.rev++;
    }
    return changed;
}

}  // namespace relay

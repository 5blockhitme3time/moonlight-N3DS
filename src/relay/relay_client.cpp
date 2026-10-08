/*
 * Talks to the PC relay from a background thread - implementation.
 */
#include "relay_client.hpp"

#include <cstdio>

#include "../diag.hpp"

#include <curl/curl.h>

namespace {

size_t collect(char *ptr, size_t size, size_t nmemb, void *userdata) {
    std::string *out = (std::string *)userdata;
    out->append(ptr, size * nmemb);
    // a panel response is a few KB; anything far larger is not ours
    return out->size() > (512u << 10) ? 0 : size * nmemb;
}

const s64 kMs = 1000000LL;

// While a scene is playing the panel follows typing as it happens; on menus
// and title screens nothing changes quickly, so it polls less.
const int kPollPlayingMs = 120;
const int kPollIdleMs = 400;
const int kPollMaxBackoffMs = 2000;
// Every failed attempt costs the stream a freeze: connect() is a synchronous
// request to the console's serial socket service, and a PC that does not run
// the relay often answers nothing until the connect timeout. So after a few
// quick retries (the relay restarting, a WiFi hiccup) the client only checks
// now and then; a tap on the panel's "cannot reach" card retries at once.
const int kQuickRetries = 6;
const int kPollAbsentMs = 20000;

// Only needs to be done once per process. Moonlight's own HTTP code relies on
// curl_easy_init()'s implicit initialisation, which is not thread-safe; doing
// it here, before the worker exists, removes that race.
void ensure_curl_global() {
    static bool done = false;
    if (!done) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        done = true;
    }
}

}  // namespace

RelayClient::RelayClient() {
    LightLock_Init(&lock);
    LightEvent_Init(&wake, RESET_ONESHOT);
}

RelayClient::~RelayClient() { stop(); }

void RelayClient::start(const std::string &host, int port) {
    if (running) {
        return;
    }
    ensure_curl_global();
    char buf[300];
    snprintf(buf, sizeof(buf), "%s:%d", host.c_str(), port);
    label = buf;
    base_url = std::string("http://") + buf;

    CURL *c = curl_easy_init();
    curl = c;
    if (c) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 2500L);
        curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    }

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // just below the caller: network waits should never delay the stream loop
    prio = prio + 1 > 0x3F ? 0x3F : prio + 1;
    running = true;
    worker = threadCreate(thread_main, this, 0x8000, prio, -1, false);
    if (!worker) {
        running = false;
    }
}

void RelayClient::stop() {
    if (worker) {
        running = false;
        LightEvent_Signal(&wake);
        threadJoin(worker, U64_MAX);
        threadFree(worker);
        worker = nullptr;
    }
    if (curl) {
        curl_easy_cleanup((CURL *)curl);
        curl = nullptr;
    }
}

void RelayClient::thread_main(void *self) { ((RelayClient *)self)->run(); }

void RelayClient::send(const std::string &cmd, int arg, int sel) {
    LightLock_Lock(&lock);
    // A burst of taps on a stalled link should not replay as a burst later.
    if (queue.size() < 8) {
        queue.push_back(Command{cmd, arg, sel});
    }
    LightLock_Unlock(&lock);
    LightEvent_Signal(&wake);
}

bool RelayClient::snapshot_if_changed(uint32_t have_rev, relay::Snapshot &out) {
    LightLock_Lock(&lock);
    bool changed = snap.rev != have_rev;
    if (changed) {
        out = snap;
    }
    LightLock_Unlock(&lock);
    return changed;
}

bool RelayClient::http_get(const std::string &path, std::string &body) {
    CURL *c = (CURL *)curl;
    if (!c) {
        return false;
    }
    body.clear();
    std::string url = base_url + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    return rc == CURLE_OK && code > 0;
}

void RelayClient::set_local_note(const char *text) {
    LightLock_Lock(&lock);
    snap.note = text;
    snap.note_seq++;
    snap.rev++;
    LightLock_Unlock(&lock);
}

void RelayClient::deliver(const Command &c) {
    diag::Timer cmd_timer(diag::CMD);
    char path[96];
    snprintf(path, sizeof(path), "/cmd?c=%s&i=%d&sel=%d", c.cmd.c_str(),
             c.arg, c.sel);
    std::string body;
    if (!http_get(path, body)) {
        set_local_note("连不上中继，命令没有送达");
        return;
    }
    // "ok\t<seq>" or "err\t<reason>". A missing hook is reported by the relay
    // as a note on the next poll, which is the more useful message.
    // The hook acts within ~100 ms; poll quickly for a moment to pick up the
    // effect and its acknowledgement.
    fast_polls = 4;
}

bool RelayClient::poll() {
    diag::Timer poll_timer(diag::POLL);
    char path[112];
    int hide = reading ? 1 : 0;
    if (sync.epoch >= 0) {
        snprintf(path, sizeof(path),
                 "/panel.txt?epoch=%lld&rev=%lld&hist=%d&hide=%d", sync.epoch,
                 sync.rev, sync.hist, hide);
    } else {
        snprintf(path, sizeof(path), "/panel.txt?hide=%d", hide);
    }
    std::string body;
    bool ok = http_get(path, body);
    relay::Update u;
    if (ok) {
        u = relay::parse(body);
        ok = u.complete;
    }
    LightLock_Lock(&lock);
    if (ok) {
        relay::apply(snap, sync, u);
    } else if (snap.relay_up) {
        snap.relay_up = false;
        snap.game_up = false;
        snap.rev++;
        sync = relay::SyncState();   // resync from scratch once it is back
    }
    LightLock_Unlock(&lock);
    return ok;
}

void RelayClient::run() {
    while (running) {
        Command cmd;
        bool have = false;
        LightLock_Lock(&lock);
        if (!queue.empty()) {
            cmd = queue.front();
            queue.pop_front();
            have = true;
        }
        bool playing = snap.game_up && snap.loc == "play";
        LightLock_Unlock(&lock);

        if (have) {
            deliver(cmd);
            continue;   // drain commands first, then poll straight away
        }

        bool ok = poll();
        int wait_ms;
        if (ok) {
            failures = 0;
            wait_ms = playing ? kPollPlayingMs : kPollIdleMs;
        } else {
            failures++;
            wait_ms = 250 << (failures < 4 ? failures : 4);
            if (wait_ms > kPollMaxBackoffMs) {
                wait_ms = kPollMaxBackoffMs;
            }
            if (failures > kQuickRetries) {
                wait_ms = kPollAbsentMs;
            }
        }
        if (ok && fast_polls > 0) {
            fast_polls--;
            wait_ms = 50;
        }
        if (wait_ms > 0) {
            LightEvent_WaitTimeout(&wake, wait_ms * kMs);
        }
    }
}

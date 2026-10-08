/*
 * Talks to the PC relay (relay.py) from a background thread.
 *
 * One worker owns one curl handle, so the TCP connection is reused across
 * polls instead of being set up several times a second. Commands from the
 * touch screen are queued and go out before the next poll, and the poll right
 * after a command runs immediately, so a tap's effect shows up as fast as the
 * game produces it.
 *
 * Polls carry the console's position in the relay's sequence; an unchanged
 * state costs a few dozen bytes, and new history arrives incrementally.
 *
 * libctru primitives (LightLock/LightEvent/threadCreate) rather than std
 * threads: they are what this platform's runtime is built on, and they give
 * a timed wait without depending on the C++ clock plumbing.
 */
#pragma once

#include <3ds.h>

#include <deque>
#include <string>

#include "relay_protocol.hpp"

class RelayClient {
  public:
    RelayClient();
    ~RelayClient();

    // host is the stream host's address; the relay runs on the same PC.
    void start(const std::string &host, int port);
    void stop();

    // cmd is one of: click auto skip voice qsave qload choose
    void send(const std::string &cmd, int arg = 0, int sel = -1);

    // Poll now instead of waiting out the backoff (the player tapped the
    // "cannot reach the relay" card).
    void retry_now() { LightEvent_Signal(&wake); }

    // Whether the panel is on screen. Polls say so, and while they do the
    // relay hides the game's own textbox on the PC.
    void set_reading(bool on) { reading = on; }

    // Copies the snapshot if it changed since `have_rev`; returns whether it
    // did. Cheap when nothing changed.
    bool snapshot_if_changed(uint32_t have_rev, relay::Snapshot &out);

    const std::string &address() const { return label; }

  private:
    struct Command {
        std::string cmd;
        int arg;
        int sel;
    };

    static void thread_main(void *self);
    void run();
    bool poll();
    void deliver(const Command &c);
    void set_local_note(const char *text);
    bool http_get(const std::string &path, std::string &body);

    Thread worker = nullptr;
    volatile bool running = false;
    LightLock lock;
    LightEvent wake;

    std::string base_url;
    std::string label;      // "host:port", shown when the relay is down
    void *curl = nullptr;   // CURL*, kept opaque here

    relay::Snapshot snap;   // guarded by lock
    relay::SyncState sync;  // worker thread only
    std::deque<Command> queue;   // guarded by lock
    int failures = 0;
    int fast_polls = 0;
    volatile bool reading = false;
};

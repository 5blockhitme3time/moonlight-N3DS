/*
 * Timing diagnostics - header only.
 *
 * The stream stutters about once every two or three seconds, and the suspects
 * are all on this side: the relay's HTTP polling on its own thread, and the
 * panel's redraw and blit on the stream thread. None of that can be looked at
 * from a debugger easily, so each suspect gets a counter here and the numbers
 * go to the SD card once a second - one line per second, so a few minutes of
 * play is a few hundred lines.
 *
 * Everything is a few instructions when a sample is taken, and the file is
 * written from whichever thread happens to cross the second boundary. Indexes
 * are fixed at compile time, so nothing is registered at run time and no two
 * threads race to be the first.
 *
 * Off in normal builds: writing the SD card once a second from the stream
 * threads is not free. `make DIAG=1` defines KP_DIAG and turns it on; without
 * it every call below compiles to nothing.
 */
#pragma once

#include <3ds.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace diag {

// Every metric is one column of the log, in this order.
enum Index {
    LOOP,      // stream thread: one pass of the main loop
    FRAME,     // video thread: interval between decoded frames
    GAP,       // video thread: interval between packets handed to it
    DECODE,    // video thread: mvd decode + framebuffer write
    PRESENT,   // video thread: display transfer + copy to the screen
    PANEL,     // stream thread: PanelController::tick
    RENDER,    // stream thread: panel redraw into its buffer
    BLIT,      // stream thread: panel buffer -> bottom screen
    POLL,      // relay thread: one HTTP poll
    CMD,       // relay thread: one command delivery
    PROBE,     // probe thread: one answer from the relay (count matters)
    METRIC_COUNT
};

inline const char *const *names() {
    static const char *const n[METRIC_COUNT] = {
        "loop", "frame", "gap", "decode", "present",
        "panel", "render", "blit", "poll", "cmd", "probe",
    };
    return n;
}

#ifdef KP_DIAG

// Samples are taken from three threads (stream, video, relay), so every
// counter is atomic and a flush subtracts what it reported instead of zeroing.
struct Acc {
    std::atomic<long> max_us{0};
    std::atomic<long> sum_us{0};
    std::atomic<unsigned> count{0};
};

inline Acc *accs() {
    static Acc a[METRIC_COUNT];
    return a;
}

// Called once, before any thread that samples exists.
inline void init() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    FILE *f = fopen("/3ds/moonlight/diag.log", "w");
    if (!f) {
        return;
    }
    fprintf(f, "# ms");
    const char *const *n = names();
    for (int i = 0; i < METRIC_COUNT; i++) {
        fprintf(f, " %s", n[i]);
    }
    fprintf(f, "   (worst_us|samples|avg_us)\n");
    fclose(f);
}

// us < 0 is ignored: a missing interval should not look like a stall.
inline void add(Index idx, long us) {
    if (us < 0) {
        return;
    }
    Acc *a = &accs()[idx];
    long prev = a->max_us.load(std::memory_order_relaxed);
    while (us > prev &&
           !a->max_us.compare_exchange_weak(prev, us,
                                            std::memory_order_relaxed)) {
        // prev is refreshed by the failed exchange
    }
    a->sum_us.fetch_add(us, std::memory_order_relaxed);
    a->count.fetch_add(1, std::memory_order_relaxed);
}

// Flush the accumulated second and start a new one. Called on every sample;
// only does work when the second has rolled over.
inline void flush_if_due() {
    static u64 last_ms = 0;
    u64 now_ms = svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
    if (last_ms == 0) {
        last_ms = now_ms;
        return;
    }
    if (now_ms - last_ms < 1000) {
        return;
    }
    // one line per second: only the thread that sees the boundary writes it
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    if (busy.test_and_set(std::memory_order_acquire)) {
        return;
    }
    last_ms = now_ms;
    Acc *a = accs();
    long sum_before[METRIC_COUNT];
    for (int i = 0; i < METRIC_COUNT; i++) {
        sum_before[i] = a[i].sum_us.load(std::memory_order_relaxed);
    }
    FILE *f = fopen("/3ds/moonlight/diag.log", "a");
    if (!f) {
        busy.clear(std::memory_order_release);
        return;
    }
    fprintf(f, "%llu", (unsigned long long)now_ms);
    for (int i = 0; i < METRIC_COUNT; i++) {
        unsigned n = a[i].count.load(std::memory_order_relaxed);
        if (n == 0) {
            fprintf(f, " -");
        } else {
            long mx = a[i].max_us.load(std::memory_order_relaxed);
            long sum = a[i].sum_us.load(std::memory_order_relaxed);
            fprintf(f, " %ld|%u|%ld", mx, n, (sum - sum_before[i]) / (long)n);
            a[i].max_us.store(0, std::memory_order_relaxed);
            a[i].sum_us.fetch_sub(sum, std::memory_order_relaxed);
            a[i].count.fetch_sub(n, std::memory_order_relaxed);
        }
    }
    fprintf(f, "\n");
    fclose(f);
    busy.clear(std::memory_order_release);
}

// ---- sampling helpers ---------------------------------------------------

inline long us_since(u64 start) {
    return (long)((svcGetSystemTick() - start) * 1000000ULL / SYSCLOCK_ARM11);
}

// Time one scope.
struct Timer {
    u64 start;
    Index idx;
    Timer(Index i) : start(svcGetSystemTick()), idx(i) {}
    ~Timer() {
        add(idx, us_since(start));
        flush_if_due();
    }
};

// Interval between successive calls (e.g. video frames).
struct Interval {
    Index idx;
    u64 last = 0;
    explicit Interval(Index i) : idx(i) {}
    void tick() {
        u64 t = svcGetSystemTick();
        if (last) {
            add(idx, (long)((t - last) * 1000000ULL / SYSCLOCK_ARM11));
        }
        last = t;
    }
};

#else  // !KP_DIAG: the same interface, doing nothing

inline void init() {}
inline void add(Index, long) {}
inline void flush_if_due() {}
inline long us_since(u64) { return 0; }

struct Timer {
    explicit Timer(Index) {}
};

struct Interval {
    explicit Interval(Index) {}
    void tick() {}
};

#endif  // KP_DIAG

}  // namespace diag

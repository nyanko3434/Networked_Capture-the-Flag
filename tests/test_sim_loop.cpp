#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include <vector>

#include "protocol.h"
#include "queues.h"
#include "sim.h"

using namespace ctf;

namespace {

int64_t ms_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

} // namespace

// README §4.8/§10 "tick rate stability": the TIMER_ABSTIME loop must hold
// its pace with no accumulated drift. Shortened window at 500 Hz so the
// suite stays fast; the invariant (no drift) is rate-independent.
TEST_CASE("tick pacing holds rate with no drift (run for N ticks)") {
    InboundQueue in;
    OutboundQueue out;
    Sim sim(in, out);
    constexpr int kHz = 500;                       // 2ms ticks
    sim.set_tick_rate(kHz);

    std::vector<int64_t> stamps;
    sim.debug_pre_tick = [&] {
        stamps.push_back(ms_since(std::chrono::steady_clock::time_point{}));
    };

    constexpr uint32_t kTicks = 1000;              // ~2s wall time
    const auto t0 = std::chrono::steady_clock::now();
    sim.run(kTicks);
    const int64_t elapsed_ms = ms_since(t0);

    REQUIRE(stamps.size() == kTicks);

    // Mean interval within 5% of target.
    const double mean_interval =
        static_cast<double>(stamps.back() - stamps.front()) / (kTicks - 1);
    CHECK(mean_interval > 1.9); // >= 95% of 2ms
    CHECK(mean_interval < 2.3); // <= ~115% of 2ms

    // No drift: average interval over the first quarter vs last quarter.
    const int64_t early = stamps[kTicks / 4] - stamps[0];
    const int64_t late = stamps.back() - stamps[(3 * kTicks) / 4];
    const double early_avg = static_cast<double>(early) / (kTicks / 4);
    const double late_avg =
        static_cast<double>(late) / (kTicks - (3 * kTicks) / 4);
    CHECK(late_avg / early_avg > 0.9); // within +-10%
    CHECK(late_avg / early_avg < 1.1);

    // Total wall time sanity: ~2000ms, allow scheduling slack.
    CHECK(elapsed_ms > 1900);
    CHECK(elapsed_ms < 2400);
    (void)elapsed_ms;
}

// README §3.2: ">3 ticks behind => resync next to now rather than catch
// up". A stall longer than 3 tick periods must NOT produce a burst of
// back-to-back catch-up ticks afterwards.
TEST_CASE("stall longer than 3 ticks resyncs instead of catching up") {
    InboundQueue in;
    OutboundQueue out;
    Sim sim(in, out);
    constexpr int kHz = 200; // 5ms ticks; 3 ticks = 15ms
    sim.set_tick_rate(kHz);

    std::vector<int64_t> stamps;
    bool stalled = false;
    sim.debug_pre_tick = [&] {
        stamps.push_back(ms_since(std::chrono::steady_clock::time_point{}));
        if (!stalled) {
            stalled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(60)); // 12 ticks
        }
    };

    sim.run(100); // ~500ms of ticks + one 60ms stall

    REQUIRE(stamps.size() == 100);

    // With a resync, every consecutive gap stays near the 5ms period even
    // right after the stall. Catch-up would show gaps near zero.
    int64_t min_gap = INT64_MAX;
    for (size_t i = 1; i < stamps.size(); ++i) {
        const int64_t gap = stamps[i] - stamps[i - 1];
        if (gap >= 0 && gap < min_gap) min_gap = gap;
    }
    // Nothing may run faster than ~40% above the period would allow:
    // gaps must be at least 2ms (well under 5ms target, but far from the
    // ~0 microsecond back-to-back burst that catch-up produces).
    CHECK(min_gap >= 2);
}

// README §9 --snapshot-rate: UDP WORLD_SNAPSHOT sends decimate while the
// simulation keeps ticking at full rate.
TEST_CASE("snapshot-rate decimation reduces UDP publishes, not ticks") {
    InboundQueue in;
    OutboundQueue out;
    Sim sim(in, out);
    sim.set_tick_rate(30);
    sim.set_snapshot_rate(10); // one snapshot per 3 ticks

    auto count_snaps = [&] {
        OutboundEvent ev;
        int n = 0;
        while (out.pop(ev)) {
            // Count publishes of either kind: decimation governs publish
            // frequency, not full-vs-delta encoding.
            if (ev.type == OutboundEventType::UdpSnapshot ||
                ev.type == OutboundEventType::UdpDeltaSnapshot) {
                ++n;
            }
        }
        return n;
    };

    for (int i = 0; i < 12; ++i) {
        sim.tick();
    }
    // 12 ticks at one-per-3 -> exactly 4 snapshot publishes.
    CHECK(count_snaps() == 4);

    // Full rate (default) publishes every tick.
    sim.set_snapshot_rate(30);
    for (int i = 0; i < 6; ++i) {
        sim.tick();
    }
    CHECK(count_snaps() == 6);
}

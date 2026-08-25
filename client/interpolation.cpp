#include "interpolation.h"

#include <algorithm>
#include <cmath>

#include "game_config.h"

// Interpolation for every player other than the local one (README §6.4,
// §6.5). Cosmetic/display-only smoothing: unlike shared/movement.cpp, this
// file is not part of the desync-critical authoritative path (its output
// never feeds back into simulation or reconciliation), so ordinary double
// arithmetic is used for the lerp/angle math below rather than fixed-point -
// README §5.1's "never floats" rule is scoped to positions that participate
// in movement_step/reconciliation, which this is not.

namespace ctf {

namespace {

const PlayerState* find_player(const WorldSnapshot& snap, uint8_t player_id) {
    for (uint8_t i = 0; i < snap.player_count; ++i) {
        if (snap.players[i].id == player_id) return &snap.players[i];
    }
    return nullptr;
}

// Shortest-arc interpolation between two u16 angles (README §6.4): compute
// the difference as a signed 16-bit delta rather than naive linear
// interpolation, which would spin almost a full turn on e.g. 65000 -> 500.
uint16_t lerp_angle(uint16_t a, uint16_t b, double t) {
    const int16_t delta = static_cast<int16_t>(static_cast<uint16_t>(b - a));
    double result = static_cast<double>(a) + static_cast<double>(delta) * t;
    // Wrap into [0, 65536).
    result = std::fmod(result, 65536.0);
    if (result < 0.0) result += 65536.0;
    return static_cast<uint16_t>(result + 0.5);
}

int32_t lerp_i32(int32_t a, int32_t b, double t) {
    return static_cast<int32_t>(std::lround(
        static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * t));
}

} // namespace

Interpolation::Interpolation() = default;

void Interpolation::push_snapshot(const WorldSnapshot& snap) {
    if (!ring_.empty() && snap.tick <= ring_.back().tick) {
        return; // reordered/duplicate - drop (keeps the ring tick-ascending)
    }

    if (!bootstrapped_) {
        const double target =
            static_cast<double>(snap.tick) - config::kInterpolationDelayTicks;
        render_tick_ = target > 0.0 ? target : 0.0;
        bootstrapped_ = true;
    }

    ring_.push_back(snap);
    // RingBuffer automatically drops oldest when full — no erase needed.
}

void Interpolation::advance(double dt_seconds) {
    if (ring_.empty()) return;

    const double newest_tick = static_cast<double>(ring_.back().tick);
    const double depth = newest_tick - render_tick_;

    // Clock drift correction (README §6.4): nudge the rate, never jump.
    double rate = 1.0;
    if (depth > config::kInterpolationDelayTicks) {
        rate = 1.02; // buffer deeper than target - catch up
    } else if (depth < config::kInterpolationDelayTicks) {
        rate = 0.98; // buffer shallower than target - hold back
    }

    render_tick_ += rate * config::kTickRateHz * dt_seconds;

    // Never render ahead of the newest data we actually have - on stall,
    // this clamp is exactly what makes sample() hold the last known
    // position instead of extrapolating (see the comment in sample()).
    if (render_tick_ > newest_tick) render_tick_ = newest_tick;
    const double oldest_tick = static_cast<double>(ring_.front().tick);
    if (render_tick_ < oldest_tick) render_tick_ = oldest_tick;
}

PlayerState Interpolation::sample(uint8_t player_id) const {
    if (ring_.empty()) return PlayerState{};

    // Find the bracketing pair: a = last snapshot with tick <= render_tick_,
    // b = first snapshot with tick > render_tick_ (or a itself if
    // render_tick_ has reached the newest tick - the advance() clamp above
    // guarantees it never exceeds it). This degenerate a==b case is exactly
    // the "hold on stall" behavior: the lerp below collapses to t=0 and
    // returns a's value verbatim, every frame, until a new snapshot arrives.
    size_t a_idx = 0;
    for (size_t i = 0; i < ring_.size(); ++i) {
        if (static_cast<double>(ring_[i].tick) <= render_tick_) a_idx = i;
        else break;
    }
    const size_t b_idx = (a_idx + 1 < ring_.size()) ? a_idx + 1 : a_idx;

    const WorldSnapshot& a = ring_[a_idx];
    const WorldSnapshot& b = ring_[b_idx];

    const PlayerState* pa = find_player(a, player_id);
    const PlayerState* pb = find_player(b, player_id);
    if (pa == nullptr && pb == nullptr) return PlayerState{};
    if (pa == nullptr) return *pb;
    if (pb == nullptr) return *pa;

    double t = 0.0;
    if (b.tick != a.tick) {
        t = (render_tick_ - static_cast<double>(a.tick)) /
            static_cast<double>(b.tick - a.tick);
        t = std::clamp(t, 0.0, 1.0);
    }

    // Discrete fields (health/alive/team/etc.) don't interpolate - take
    // whichever bracket render_tick_ is closer to so state changes show up
    // as soon as they're known rather than always lagging one snapshot.
    PlayerState out = (t < 0.5) ? *pa : *pb;
    out.motion.position.x = lerp_i32(pa->motion.position.x, pb->motion.position.x, t);
    out.motion.position.y = lerp_i32(pa->motion.position.y, pb->motion.position.y, t);
    out.motion.velocity = Vec2Fixed{}; // cosmetic only - never carried/needed
    out.aim_angle = lerp_angle(pa->aim_angle, pb->aim_angle, t);

    return out;
}

} // namespace ctf

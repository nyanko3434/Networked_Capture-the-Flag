#pragma once

// Interpolation for every player other than the local one (README §6,
// §6.4). Keeps a ring of recent snapshots and samples a fractional
// render_tick that trails the newest received tick by
// config::kInterpolationDelayTicks.

#include <cstdint>
#include <vector>

#include "game_types.h"

namespace ctf {

class Interpolation {
public:
    Interpolation();

    // Appends a newly-decoded snapshot. Snapshots must arrive in strictly
    // increasing tick order to keep the ring sorted for bracket search (a
    // UDP-reordered snapshot older than or equal to the newest one already
    // held is dropped here - this mirrors the staleness guard prediction.cpp
    // applies for the local player, README §6.3, applied to the ring
    // instead of a single "last applied" value). Evicts the oldest entry
    // once config::kSnapshotRingSize is exceeded.
    //
    // On the very first snapshot ever pushed, render_tick_ is bootstrapped
    // to `tick - config::kInterpolationDelayTicks` (clamped at 0) instead of
    // ticking up from its 0.0 default, which would otherwise take many
    // seconds to catch up to a mid-match join tick. README §6.4 doesn't
    // cover bootstrapping explicitly - this is a one-time initialization,
    // not a mid-stream correction, so it isn't the "hard jump" the drift
    // rule (advance()) warns against: there is nothing rendered yet to pop.
    void push_snapshot(const WorldSnapshot& snap);

    // Advances render_tick by one frame's worth of wall time, nudging its
    // rate to track buffer depth against the target (README §6.4).
    void advance(double dt_seconds);

    // Samples player_id's position/aim at the current render_tick,
    // bracketing the two nearest snapshots and taking the shortest-arc
    // angle delta. Holds last known position on stall; never extrapolates.
    // Returns a default-constructed PlayerState (id left at 0) if
    // player_id isn't present in either bracketing snapshot.
    PlayerState sample(uint8_t player_id) const;

    double render_tick() const { return render_tick_; }
    size_t snapshot_count() const { return ring_.size(); }

private:
    std::vector<WorldSnapshot> ring_;
    double render_tick_ = 0.0;
    bool bootstrapped_ = false;
};

} // namespace ctf

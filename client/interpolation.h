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

    void push_snapshot(const WorldSnapshot& snap);

    // Advances render_tick by one frame's worth of wall time, nudging its
    // rate to track buffer depth against the target (README §6.4).
    void advance(double dt_seconds);

    // Samples player_id's position/aim at the current render_tick,
    // bracketing the two nearest snapshots and taking the shortest-arc
    // angle delta. Holds last known position on stall; never extrapolates.
    PlayerState sample(uint8_t player_id) const;

private:
    std::vector<WorldSnapshot> ring_;
    double render_tick_ = 0.0;
};

} // namespace ctf

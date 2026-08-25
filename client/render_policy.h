#pragma once

// Pure decision logic that render.cpp's draw_frame() uses to pick what to
// draw - deliberately kept raylib-free so it's unit-testable without a
// window/GPU context. The GPU drawing calls themselves (render.cpp) are the
// part that genuinely needs a live/visual check.

#include "game_types.h"
#include "interpolation.h"

namespace ctf {
namespace render_policy {

// README §6.6 F1: which position to draw for the local player.
//   enabled=true  (default) -> predicted position (README §6.2's local
//                 prediction).
//   enabled=false -> raw authoritative position ("shows what the network
//                 actually feels like").
inline Vec2Fixed resolve_local_position(bool prediction_enabled,
                                        Vec2Fixed predicted_position,
                                        Vec2Fixed raw_authoritative_position) {
    return prediction_enabled ? predicted_position : raw_authoritative_position;
}

// README §6.6 F2: which state to draw for a remote player.
//   enabled=true  (default) -> Interpolation::sample() (smoothed).
//   enabled=false -> the raw per-tick snapshot entry, unsmoothed ("remote
//                 players snap between raw snapshots").
// Falls back to `raw` if the player isn't present in the interpolation
// ring yet (e.g. just joined), rather than an empty/garbage state.
inline PlayerState resolve_remote_state(bool interpolation_enabled,
                                        const PlayerState& raw,
                                        const Interpolation& interpolation) {
    if (!interpolation_enabled) return raw;
    const PlayerState interp = interpolation.sample(raw.id);
    return (interp.id == raw.id) ? interp : raw;
}

// README §5.5's WorldSnapshot has no position field for a Dropped flag -
// only FLAG_DROPPED (TCP, one-time) carries it (README §5.4). This resolves
// where to draw a team's flag for any of the three states (§7.4) given
// whatever position was last cached from a FLAG_DROPPED event.
inline Vec2Fixed resolve_flag_position(FlagState state, uint8_t carrier_id,
                                       const WorldSnapshot& latest,
                                       Vec2Fixed base_position,
                                       Vec2Fixed dropped_cache) {
    if (state == FlagState::AtBase) return base_position;
    if (state == FlagState::Carried) {
        for (uint8_t i = 0; i < latest.player_count; ++i) {
            if (latest.players[i].id == carrier_id) {
                return latest.players[i].motion.position;
            }
        }
        return base_position; // carrier not found (shouldn't happen) - fall back
    }
    return dropped_cache; // Dropped
}

} // namespace render_policy
} // namespace ctf

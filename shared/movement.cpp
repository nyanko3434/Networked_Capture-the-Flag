#include "movement.h"

#include "game_config.h"

// README §7.2: THE physics step. No acceleration or momentum — input maps
// directly to velocity each tick; releasing a key stops instantly, so there
// is no state to carry between ticks beyond what's passed in. Diagonal
// movement is normalized by 181/256 (config::kDiagonalFactorNum/Den) when
// both axes are non-zero, or it would be ~41% faster than axis-aligned
// movement. Collision resolves X fully, then Y, in that fixed order — this
// function is the only place that order is decided, so every caller (sim,
// prediction) gets it for free and can never diverge from it.
//
// `position` uses the same top-left-corner convention as Map::aabb_collides
// (README §7.1's collision query — documented there since that's where the
// convention is consumed for the AABB test itself).

namespace ctf {

// Q24.8-style fixed-point multiply/divide (README §5.1): both operands are
// assumed to be in the 1/256 px internal representation
// (config::kFixedShift == 8, config::kFixedScale == 256). This same pair of
// operations is used both to scale a position/velocity by another
// fixed-point quantity, and to scale by a plain fraction like the diagonal
// factor (181 here is exactly 181/256 in this representation, so
// fp_mul(x, 181) == x * 181 / 256).
int32_t fp_mul(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >>
                                 config::kFixedShift);
}

int32_t fp_div(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) << config::kFixedShift) / b);
}

PlayerMotion movement_step(const PlayerMotion& in, const InputCmd& cmd, const Map& map) {
    // Input maps directly to velocity -- no acceleration, no carrying over
    // `in.velocity` from the previous tick (README §7.2).
    const int32_t dir_x = (cmd.buttons & kInputRight ? 1 : 0) - (cmd.buttons & kInputLeft ? 1 : 0);
    const int32_t dir_y = (cmd.buttons & kInputDown ? 1 : 0) - (cmd.buttons & kInputUp ? 1 : 0);

    const bool diagonal = dir_x != 0 && dir_y != 0;
    const int32_t speed = diagonal
                               ? fp_mul(config::kMoveSpeedFpPerTick, config::kDiagonalFactorNum)
                               : config::kMoveSpeedFpPerTick;

    PlayerMotion out;
    out.velocity.x = dir_x * speed;
    out.velocity.y = dir_y * speed;

    // Resolve X fully, then Y (README §7.2, and §6.7 desync-checklist item
    // 6 -- the axis order must be identical on both call sites by
    // construction, since there is only ever this one function). Each axis
    // is a simple move-then-revert: if the new position after moving that
    // axis alone collides, that axis's movement is discarded and the other
    // axis is still attempted from the pre-move position on that axis.
    Vec2Fixed pos = in.position;

    Vec2Fixed after_x = pos;
    after_x.x += out.velocity.x;
    if (map.aabb_collides(after_x)) {
        after_x.x = pos.x;
    }

    Vec2Fixed after_y = after_x;
    after_y.y += out.velocity.y;
    if (map.aabb_collides(after_y)) {
        after_y.y = after_x.y;
    }

    out.position = after_y;
    return out;
}

} // namespace ctf

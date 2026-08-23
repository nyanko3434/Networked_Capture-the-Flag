#include "movement.h"

// THE physics step (README §7.2). Integer-only, deterministic, no I/O, no
// randomness, no static mutable state. Instant velocity: input maps directly
// to velocity, releasing keys stops immediately. Collision resolves X fully,
// then Y — the fixed order both client and server inherit by calling this
// one function.

namespace ctf {

int32_t fp_mul(int32_t a, int32_t b) {
    return static_cast<int32_t>(
        (static_cast<int64_t>(a) * static_cast<int64_t>(b)) >>
        config::kFixedShift);
}

int32_t fp_div(int32_t a, int32_t b) {
    return static_cast<int32_t>(
        (static_cast<int64_t>(a) << config::kFixedShift) /
        static_cast<int64_t>(b));
}

PlayerMotion movement_step(const PlayerMotion& in, const InputCmd& cmd,
                           const Map& map) {
    const int32_t speed = config::kMoveSpeedFpPerTick;

    int32_t vx = 0;
    int32_t vy = 0;
    if (cmd.buttons & kInputLeft) {
        vx -= speed;
    }
    if (cmd.buttons & kInputRight) {
        vx += speed;
    }
    if (cmd.buttons & kInputUp) {
        vy -= speed;
    }
    if (cmd.buttons & kInputDown) {
        vy += speed;
    }

    // Diagonal normalization (README §7.2): 181/256 ~ 1/sqrt(2), keeping
    // everything in integer math.
    if (vx != 0 && vy != 0) {
        vx = fp_mul(vx, config::kDiagonalFactorNum);
        vy = fp_mul(vy, config::kDiagonalFactorNum);
    }

    Vec2Fixed pos = in.position;

    // Resolve X fully, then Y from the post-X position.
    const int32_t nx = pos.x + vx;
    if (!map.aabb_collides(Vec2Fixed{nx, pos.y})) {
        pos.x = nx;
    }
    const int32_t ny = pos.y + vy;
    if (!map.aabb_collides(Vec2Fixed{pos.x, ny})) {
        pos.y = ny;
    }

    PlayerMotion out;
    out.position = pos;
    out.velocity = Vec2Fixed{vx, vy};
    return out;
}

} // namespace ctf

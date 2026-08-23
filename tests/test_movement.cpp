#include <doctest.h>

#include <cstring>
#include <cstdint>
#include <random>

#include "game_config.h"
#include "game_types.h"
#include "map.h"
#include "movement.h"

using ctf::InputButton;
using ctf::InputCmd;
using ctf::Map;
using ctf::PlayerMotion;
using ctf::Vec2Fixed;

namespace {

constexpr int32_t kTileFp = ctf::config::kTileSizePx * ctf::config::kFixedScale;
constexpr int32_t kSpeed = ctf::config::kMoveSpeedFpPerTick;

bool same_motion(const PlayerMotion& a, const PlayerMotion& b) {
    return std::memcmp(&a, &b, sizeof(PlayerMotion)) == 0;
}

// Open area: tile (2,12) pocket is 3+ open tiles wide; center of map room.
Vec2Fixed open_spot() { return {kTileFp * 2 + 128, kTileFp * 12 + 128}; }

} // namespace

TEST_CASE("fp_mul/fp_div basic fixed-point arithmetic") {
    // fp_mul(a, b): (a * b) >> kFixedShift. fp_div(a, b): (a << kFixedShift)/b.
    CHECK(ctf::fp_mul(1 << ctf::config::kFixedShift,
                      1 << ctf::config::kFixedShift) ==
          1 << ctf::config::kFixedShift);
    CHECK(ctf::fp_mul(kSpeed, ctf::config::kDiagonalFactorNum) > 0);
    // speed/speed == 1.0 in fixed-point.
    CHECK(ctf::fp_div(kSpeed, kSpeed) == 1 << ctf::config::kFixedShift);
    // half of speed, in fixed-point form.
    CHECK(ctf::fp_div(kSpeed, 2) == (kSpeed << ctf::config::kFixedShift) / 2);
}

TEST_CASE("single-axis movement moves exactly move_speed per tick") {
    Map map;

    InputCmd cmd;
    cmd.buttons = InputButton::kInputRight;
    PlayerMotion in;
    in.position = open_spot();

    PlayerMotion out = ctf::movement_step(in, cmd, map);
    CHECK(out.position.x == in.position.x + kSpeed);
    CHECK(out.position.y == in.position.y);

    cmd.buttons = InputButton::kInputLeft;
    out = ctf::movement_step(in, cmd, map);
    CHECK(out.position.x == in.position.x - kSpeed);

    cmd.buttons = InputButton::kInputUp;
    out = ctf::movement_step(in, cmd, map);
    CHECK(out.position.y == in.position.y - kSpeed);

    cmd.buttons = InputButton::kInputDown;
    out = ctf::movement_step(in, cmd, map);
    CHECK(out.position.y == in.position.y + kSpeed);
}

TEST_CASE("releasing keys stops instantly: zero input means no movement") {
    Map map;
    PlayerMotion in;
    in.position = open_spot();
    in.velocity = Vec2Fixed{kSpeed, kSpeed};

    PlayerMotion out = ctf::movement_step(in, InputCmd{}, map);
    CHECK(out.position.x == in.position.x);
    CHECK(out.position.y == in.position.y);
    CHECK(out.velocity.x == 0);
    CHECK(out.velocity.y == 0);
}

TEST_CASE("diagonal movement is normalized by 181/256, not 41% faster") {
    Map map;
    PlayerMotion in;
    in.position = open_spot();

    InputCmd diag;
    diag.buttons =
        static_cast<uint8_t>(InputButton::kInputRight | InputButton::kInputDown);

    PlayerMotion out = ctf::movement_step(in, diag, map);

    const int32_t dx = out.position.x - in.position.x;
    const int32_t dy = out.position.y - in.position.y;

    // Expected per-axis displacement: speed * 181/256.
    const int32_t expected_axis =
        kSpeed * ctf::config::kDiagonalFactorNum / ctf::config::kDiagonalFactorDen;
    CHECK(dx == expected_axis);
    CHECK(dy == expected_axis);

    // Magnitude check vs. straight-line speed: dx^2+dy^2 must be <= speed^2
    // (integer arithmetic; strict equality holds for the exact factor).
    const int64_t diag_sq =
        static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
    const int64_t straight_sq = static_cast<int64_t>(kSpeed) * kSpeed;
    CHECK(diag_sq <= straight_sq);
    CHECK(diag_sq >= straight_sq * 90 / 100); // ~95% of straight speed
}

TEST_CASE("determinism: 10,000 random inputs run twice are byte-identical") {
    Map map;
    std::mt19937 rng(0xC7F12345u);
    std::uniform_int_distribution<int> button_dist(0, 31);

    for (int i = 0; i < 10000; ++i) {
        PlayerMotion in;
        in.position =
            Vec2Fixed{static_cast<int32_t>(rng() % (40 * kTileFp)),
                      static_cast<int32_t>(rng() % (25 * kTileFp))};
        in.velocity = Vec2Fixed{};
        InputCmd cmd;
        cmd.buttons = static_cast<uint8_t>(button_dist(rng));
        cmd.aim_angle = static_cast<uint16_t>(rng() & 0xFFFF);

        PlayerMotion first = ctf::movement_step(in, cmd, map);
        PlayerMotion second = ctf::movement_step(in, cmd, map);
        REQUIRE(same_motion(first, second));
    }
}

TEST_CASE("wall blocks movement along an axis while the other axis still slides")
{
    Map map;

    // Start flush against the west border wall interior, heading left into
    // it: X must be clamped to the wall edge, Y unaffected by X blockage.
    PlayerMotion in;
    in.position = Vec2Fixed{kTileFp * 1, kTileFp * 12};

    InputCmd left;
    left.buttons = InputButton::kInputLeft;
    PlayerMotion out = ctf::movement_step(in, left, map);
    CHECK(out.position.x == in.position.x); // clamped, no penetration
    CHECK(out.position.y == in.position.y);

    // Same spot moving down instead: unobstructed.
    InputCmd down;
    down.buttons = InputButton::kInputDown;
    out = ctf::movement_step(in, down, map);
    CHECK(out.position.x == in.position.x);
    CHECK(out.position.y == in.position.y + kSpeed);
}

TEST_CASE("corner slide: X resolves fully before Y") {
    Map map;

    // Position just above-left of a wall corner such that moving right would
    // enter the wall row, and moving down would enter the wall column. With
    // X-then-Y order, X is resolved first (blocked or clamped), then Y is
    // attempted from the post-X position.
    //
    // Use the pillar at tiles cols 8-11, rows 11-13. Stand mid-tile-7 on
    // row 10 (open), then hold Right+Down: X slides into col 8's airspace
    // while staying on open row 10; Y then hits the pillar top and blocks.
    PlayerMotion in;
    in.position = Vec2Fixed{kTileFp * 7 + 2048, kTileFp * 10 + 2048};

    InputCmd diag;
    diag.buttons =
        static_cast<uint8_t>(InputButton::kInputRight | InputButton::kInputDown);
    PlayerMotion out = ctf::movement_step(in, diag, map);

    // X slid freely (diagonal-scaled 1365*181>>8 = 965); Y hit the pillar.
    const int32_t diag_step =
        kSpeed * ctf::config::kDiagonalFactorNum / ctf::config::kDiagonalFactorDen;
    CHECK(out.position.x == in.position.x + diag_step);
    CHECK(out.position.y == in.position.y);
}

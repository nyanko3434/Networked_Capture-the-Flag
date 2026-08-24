// shared/movement tests (implementation_guide.md §1.5 acceptance criteria):
// determinism (named in README §10), diagonal speed normalization, and a
// no-float sweep (done via grep, since it's a static source property).

#include "game_config.h"
#include "map.h"
#include "movement.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace ctf;

namespace {

void test_determinism() {
    // 10,000 random (PlayerMotion, InputCmd) pairs, run each through
    // movement_step twice, assert byte-identical output both times.
    Map map;
    std::mt19937 rng(12345); // fixed seed for a reproducible test run
    std::uniform_int_distribution<int32_t> pos_x_dist(0, config::kMapWidthPx * config::kFixedScale);
    std::uniform_int_distribution<int32_t> pos_y_dist(0, config::kMapHeightPx * config::kFixedScale);
    std::uniform_int_distribution<int32_t> vel_dist(-2000, 2000);
    std::uniform_int_distribution<int> buttons_dist(0, 31); // 5 button bits
    std::uniform_int_distribution<int> angle_dist(0, 65535);

    for (int i = 0; i < 10000; ++i) {
        PlayerMotion in;
        in.position.x = pos_x_dist(rng);
        in.position.y = pos_y_dist(rng);
        in.velocity.x = vel_dist(rng);
        in.velocity.y = vel_dist(rng);

        InputCmd cmd;
        cmd.buttons = static_cast<uint8_t>(buttons_dist(rng));
        cmd.aim_angle = static_cast<uint16_t>(angle_dist(rng));

        PlayerMotion out1 = movement_step(in, cmd, map);
        PlayerMotion out2 = movement_step(in, cmd, map);

        assert(std::memcmp(&out1, &out2, sizeof(PlayerMotion)) == 0);
    }
    printf("test_determinism: OK (10000/10000 byte-identical pairs)\n");
}

void test_diagonal_speed_not_41_percent_faster() {
    // Feed both movement axes held for one tick from a fixed start position
    // with no walls nearby (open field per the current map layout).
    Map map;
    PlayerMotion in;
    in.position = Vec2Fixed{15 * config::kTileSizePx * config::kFixedScale,
                             15 * config::kTileSizePx * config::kFixedScale};
    in.velocity = Vec2Fixed{0, 0};

    InputCmd cmd;
    cmd.buttons = kInputRight | kInputDown; // diagonal

    PlayerMotion out = movement_step(in, cmd, map);
    int32_t dx = out.position.x - in.position.x;
    int32_t dy = out.position.y - in.position.y;

    // Magnitude compared against straight-axis speed via quadrature (a
    // floating comparison in the *test* is fine -- movement.cpp itself
    // never uses float, verified separately by the grep sweep below).
    double magnitude = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
    double straight_speed = static_cast<double>(config::kMoveSpeedFpPerTick);

    // Within fixed-point rounding tolerance of straight-axis speed -- not
    // straight_speed * 1.41 (the 41%-too-fast bug this normalization
    // prevents).
    double ratio = magnitude / straight_speed;
    assert(ratio > 0.95 && ratio < 1.05);
    printf("test_diagonal_speed_not_41_percent_faster: OK (ratio=%.4f, dx=%d, dy=%d)\n",
           ratio, dx, dy);
}

} // namespace

int main() {
    test_determinism();
    test_diagonal_speed_not_41_percent_faster();
    printf("All movement tests passed.\n");
    return 0;
}

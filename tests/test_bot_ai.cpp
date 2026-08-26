#include <doctest.h>

#include <cmath>
#include <cstdint>

#include "bot_ai.h"
#include "flow_field.h"
#include "game_config.h"
#include "game_types.h"
#include "map.h"
#include "movement.h"

using namespace ctf;

namespace {

constexpr int32_t kTileFp = config::kTileSizePx * config::kFixedScale;

bool tile_passable_simple(const Map& map, int32_t tx, int32_t ty) {
    if (tx < 0 || ty < 0 || tx >= config::kMapWidthTiles ||
        ty >= config::kMapHeightTiles) {
        return false;
    }
    return !map.is_wall(tx, ty);
}

// One-player snapshot with both flags at base.
WorldSnapshot solo_snap(uint8_t id, Team team, const Vec2Fixed& pos,
                        bool carrying = false) {
    WorldSnapshot s;
    s.player_count = 1;
    PlayerState& p = s.players[0];
    p.id = id;
    p.team = team;
    p.motion.position = pos;
    p.aim_angle = 0;
    p.health = config::kMaxHealth;
    p.alive = true;
    p.carrying_flag = carrying;
    s.flag_state_red = FlagState::AtBase;
    s.flag_state_blue = FlagState::AtBase;
    s.flag_carrier_red = 0xFF;
    s.flag_carrier_blue = 0xFF;
    return s;
}

} // namespace

TEST_CASE("flow field: BFS distances satisfy the descent property") {
    const Map& map = Map::instance();
    FlowField field;
    field.build(map, kRedBasePosition);

    // Every empty tile must be reachable (the arena is fully connected),
    // and every reachable non-source tile must have a passable cardinal
    // neighbor exactly one step closer - i.e. walking downhill always
    // exists, so navigation can never dead-end.
    int reachable = 0;
    for (int32_t ty = 0; ty < config::kMapHeightTiles; ++ty) {
        for (int32_t tx = 0; tx < config::kMapWidthTiles; ++tx) {
            if (!tile_passable_simple(map, tx, ty)) continue;

            // Empty tile: the centered 24x24 box must also fit (tile size
            // 32 > player 24), so reachability should cover it.
            const uint16_t d = field.dist_at(tx, ty);
            REQUIRE(d != FlowField::kUnreachable);
            ++reachable;

            if (d == 0) continue; // source tile
            bool has_pred = false;
            const int32_t nx[4] = {tx + 1, tx - 1, tx, tx};
            const int32_t ny[4] = {ty, ty, ty + 1, ty - 1};
            for (int i = 0; i < 4; ++i) {
                if (field.dist_at(nx[i], ny[i]) ==
                    static_cast<uint16_t>(d - 1)) {
                    has_pred = true;
                }
            }
            CHECK(has_pred);
        }
    }
    CHECK(reachable > 500); // sane open arena
}

TEST_CASE("flow field: steer returns zero at the source") {
    const Map& map = Map::instance();
    FlowField field;
    field.build(map, kRedBasePosition);
    CHECK(field.steer(map, kRedBasePosition) == 0);
}

TEST_CASE("bot AI navigates from every red spawn to the blue base "
          "through the real movement/collision code without sticking") {
    const Map& map = Map::instance();

    for (int spawn = 0; spawn < config::kMaxPlayers; ++spawn) {
        CAPTURE(spawn);
        BotAI ai(/*seed=*/1234 + static_cast<uint32_t>(spawn),
                 /*player_id=*/1, Team::Red);

        WorldSnapshot snap =
            solo_snap(1, Team::Red, kRedSpawnPoints[spawn]);
        PlayerMotion motion;
        motion.position = snap.players[0].motion.position;

        const Vec2Fixed& goal = kBlueBasePosition;
        bool arrived = false;
        const int32_t tol = kTileFp; // within one tile of the flag
        for (int t = 0; t < 1800 && !arrived; ++t) {
            const InputCmd cmd = ai.tick(snap);
            motion = movement_step(motion, cmd, map);
            snap.players[0].motion.position = motion.position;
            arrived = std::abs(motion.position.x - goal.x) < tol &&
                      std::abs(motion.position.y - goal.y) < tol;
        }
        CHECK(arrived);
        if (!arrived) {
            WARN_MESSAGE(false,
                         "spawn ", spawn, " stuck at ",
                         motion.position.x / config::kFixedScale, ",",
                         motion.position.y / config::kFixedScale);
        }
    }
}

TEST_CASE("bot AI carrying the flag runs home from behind the map") {
    const Map& map = Map::instance();
    BotAI ai(/*seed=*/42, /*player_id=*/3, Team::Red);

    WorldSnapshot snap = solo_snap(3, Team::Red,
                                   kBlueSpawnPoints[0], /*carrying=*/true);
    PlayerMotion motion;
    motion.position = snap.players[0].motion.position;

    const Vec2Fixed& home = kRedBasePosition;
    bool arrived = false;
    const int32_t tol = kTileFp;
    for (int t = 0; t < 1800 && !arrived; ++t) {
        const InputCmd cmd = ai.tick(snap);
        motion = movement_step(motion, cmd, map);
        snap.players[0].motion.position = motion.position;
        arrived = std::abs(motion.position.x - home.x) < tol &&
                  std::abs(motion.position.y - home.y) < tol;
    }
    CHECK(arrived);
}

TEST_CASE("bot AI hunts an enemy raider carrying our flag") {
    const Map& map = Map::instance();
    BotAI ai(/*seed=*/7, /*player_id=*/1, Team::Red);

    // Raider on the far side of our half, holding our flag.
    WorldSnapshot snap = solo_snap(1, Team::Red, kRedSpawnPoints[0]);
    snap.player_count = 2;
    PlayerState& raider = snap.players[1];
    raider.id = 9;
    raider.team = Team::Blue;
    raider.motion.position = Vec2Fixed{10 * kTileFp, 12 * kTileFp};
    raider.alive = true;
    snap.flag_state_red = FlagState::Carried;
    snap.flag_carrier_red = 9;

    PlayerMotion motion;
    motion.position = snap.players[0].motion.position;
    const float start_dist =
        std::fabs(static_cast<float>(motion.position.x - raider.motion.position.x)) /
            config::kFixedScale +
        std::fabs(static_cast<float>(motion.position.y - raider.motion.position.y)) /
            config::kFixedScale;

    bool closed_in = false;
    for (int t = 0; t < 600 && !closed_in; ++t) {
        const InputCmd cmd = ai.tick(snap);
        motion = movement_step(motion, cmd, map);
        snap.players[0].motion.position = motion.position;
        const float now_dist =
            std::fabs(static_cast<float>(motion.position.x -
                                         raider.motion.position.x)) /
                config::kFixedScale +
            std::fabs(static_cast<float>(motion.position.y -
                                         raider.motion.position.y)) /
                config::kFixedScale;
        if (now_dist < start_dist / 2) closed_in = true;
    }
    CHECK(closed_in);
}

TEST_CASE("bot AI is deterministic for a given seed") {
    const Map& map = Map::instance();
    BotAI a(/*seed=*/99, 1, Team::Red);
    BotAI b(/*seed=*/99, 1, Team::Red);

    WorldSnapshot snap = solo_snap(1, Team::Red, kRedSpawnPoints[0]);
    PlayerMotion ma;
    ma.position = snap.players[0].motion.position;
    PlayerMotion mb = ma;

    for (int t = 0; t < 300; ++t) {
        const InputCmd ca = a.tick(snap);
        const InputCmd cb = b.tick(snap);
        CHECK(ca.buttons == cb.buttons);
        CHECK(ca.aim_angle == cb.aim_angle);
        ma = movement_step(ma, ca, map);
        mb = movement_step(mb, cb, map);
        snap.players[0].motion.position = ma.position;
    }
}

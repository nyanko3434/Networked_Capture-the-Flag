#include <doctest.h>

#include <cstdint>

#include "game_config.h"
#include "game_types.h"
#include "map.h"

using ctf::kBlueBasePosition;
using ctf::kBlueSpawnPoints;
using ctf::kRedBasePosition;
using ctf::kRedSpawnPoints;
using ctf::Map;
using ctf::TileType;
using ctf::Vec2Fixed;

namespace {

constexpr int32_t kTileFp = ctf::config::kTileSizePx * ctf::config::kFixedScale;
constexpr int32_t kPlayerFp =
    ctf::config::kPlayerSizePx * ctf::config::kFixedScale;

} // namespace

TEST_CASE("map grid is exactly mirrored left-right") {
    Map map;
    for (int32_t y = 0; y < ctf::config::kMapHeightTiles; ++y) {
        for (int32_t x = 0; x < ctf::config::kMapWidthTiles; ++x) {
            CHECK_MESSAGE(map.tile_at(x, y) == map.tile_at(39 - x, y),
                          "asymmetry at (" << x << "," << y << ")");
        }
    }
}

TEST_CASE("out-of-bounds queries are walls") {
    Map map;
    CHECK(map.is_wall(-1, 0));
    CHECK(map.is_wall(0, -1));
    CHECK(map.is_wall(ctf::config::kMapWidthTiles, 5));
    CHECK(map.is_wall(5, ctf::config::kMapHeightTiles));
}

TEST_CASE("known geometry: borders, lane walls, gaps") {
    Map map;
    // Full wall border.
    CHECK(map.is_wall(0, 0));
    CHECK(map.is_wall(39, 0));
    CHECK(map.is_wall(0, 24));
    CHECK(map.is_wall(39, 24));
    CHECK(map.is_wall(19, 0));

    // Interior open near bases.
    CHECK_FALSE(map.is_wall(1, 1));
    CHECK_FALSE(map.is_wall(2, 12));

    // Lane walls (row 8) with gaps (cols 11-14 and their mirrors 25-28).
    CHECK(map.is_wall(5, 8));
    CHECK(map.is_wall(20, 8));
    CHECK_FALSE(map.is_wall(12, 8));
    CHECK_FALSE(map.is_wall(27, 8));

    // Row 16 mirrors row 8.
    CHECK(map.is_wall(5, 16));
    CHECK_FALSE(map.is_wall(12, 16));
}

TEST_CASE("AABB fully inside an open tile does not collide") {
    Map map;
    // Tile (2,12) is open; place a 24px box well inside it.
    Vec2Fixed pos{kTileFp * 2 + 100, kTileFp * 12 + 100};
    CHECK_FALSE(map.aabb_collides(pos));
}

TEST_CASE("AABB overlapping a wall collides") {
    Map map;
    // Tile (0,1) is the west border wall. Box starting 1 fp-unit into open
    // space right of it overlaps nothing; shift left 1 unit -> overlap.
    int32_t wall_right_edge_fp = kTileFp * 1;
    Vec2Fixed touching{wall_right_edge_fp, kTileFp * 1};
    CHECK_FALSE(map.aabb_collides(touching)); // flush against wall, not in it

    Vec2Fixed overlapping{wall_right_edge_fp - 1, kTileFp * 1};
    CHECK(map.aabb_collides(overlapping)); // 1 fp-unit of penetration
}

TEST_CASE("AABB X-only and Y-only overlap are both detected") {
    Map map;

    // X-only: box vertically centered on row 12 (open), slid horizontally
    // into the border wall at col 0.
    Vec2Fixed x_only{-100, kTileFp * 12 + 100};
    CHECK(map.aabb_collides(x_only));

    // Y-only: box horizontally centered on col 2 (open), pushed up into the
    // north border wall at row 0.
    Vec2Fixed y_only{kTileFp * 2 + 100, -100};
    CHECK(map.aabb_collides(y_only));
}

TEST_CASE("bases sit on opposite sides, red left, blue right") {
    const int32_t mid_x =
        ctf::config::kMapWidthPx * ctf::config::kFixedScale / 2;
    CHECK(kRedBasePosition.x < kBlueBasePosition.x);
    CHECK(kRedBasePosition.x < mid_x);
    CHECK(kBlueBasePosition.x > mid_x);
}

TEST_CASE("spawn points are inside the map on open tiles") {
    Map map;
    auto check_spawns = [&](const Vec2Fixed* spawns) {
        for (int i = 0; i < ctf::config::kMaxPlayers; ++i) {
            const int32_t tx = spawns[i].x / kTileFp;
            const int32_t ty = spawns[i].y / kTileFp;
            REQUIRE(tx >= 0);
            REQUIRE(tx < ctf::config::kMapWidthTiles);
            REQUIRE(ty >= 0);
            REQUIRE(ty < ctf::config::kMapHeightTiles);
            CHECK_FALSE(map.is_wall(tx, ty));
        }
    };
    check_spawns(kRedSpawnPoints);
    check_spawns(kBlueSpawnPoints);

    // A spawn point must fit its whole 24px box without overlapping a wall.
    for (int i = 0; i < ctf::config::kMaxPlayers; ++i) {
        CHECK_FALSE(map.aabb_collides(kRedSpawnPoints[i]));
        CHECK_FALSE(map.aabb_collides(kBlueSpawnPoints[i]));
    }
}

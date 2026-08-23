#include "map.h"

// Hardcoded 40x25 tile grid, mirrored left-right (README §7.1): every row is
// a palindrome, so tile(x,y) == tile(39-x,y) holds by construction. Two tile
// types only; bases and spawn points are coordinate constants below, never
// tile types. All collision math is integer fixed-point only.

namespace ctf {

namespace {

// clang-format off
// Layout: open north/south lanes separated by two horizontal walls with
// mirrored gaps (rows 8 and 16), pillar blocks for cover, and clear base
// pockets at the left/right midlines.
const char* kMapRows[config::kMapHeightTiles] = {
    "########################################",
    "#......................................#",
    "#......................................#",
    "#...####...........##...........####...#",
    "#...####...........##...........####...#",
    "#...####...........##...........####...#",
    "#...####...........##...........####...#",
    "#......................................#",
    "###########....##########....###########",
    "#......................................#",
    "#......................................#",
    "#.......####................####.......#",
    "#.......####................####.......#",
    "#.......####................####.......#",
    "#......................................#",
    "#......................................#",
    "###########....##########....###########",
    "#......................................#",
    "#...####...........##...........####...#",
    "#...####...........##...........####...#",
    "#...####...........##...........####...#",
    "#...####...........##...........####...#",
    "#......................................#",
    "#......................................#",
    "########################################",
};
// clang-format on

Vec2Fixed tile_to_fp(int32_t tile_x, int32_t tile_y) {
    return Vec2Fixed{tile_x * config::kTileSizePx * config::kFixedScale,
                     tile_y * config::kTileSizePx * config::kFixedScale};
}

} // namespace

Map::Map() = default;

TileType Map::tile_at(int32_t tile_x, int32_t tile_y) const {
    if (tile_x < 0 || tile_x >= config::kMapWidthTiles ||
        tile_y < 0 || tile_y >= config::kMapHeightTiles) {
        return TileType::Wall;
    }
    return kMapRows[tile_y][tile_x] == '#' ? TileType::Wall : TileType::Empty;
}

bool Map::is_wall(int32_t tile_x, int32_t tile_y) const {
    return tile_at(tile_x, tile_y) == TileType::Wall;
}

bool Map::aabb_collides(Vec2Fixed position) const {
    // Half-open AABB [x, x+size) x [y, y+size); the -1 keeps an exactly
    // flush box from bleeding into the next tile.
    const int32_t size_fp = config::kPlayerSizePx * config::kFixedScale;
    const int32_t tile_fp = config::kTileSizePx * config::kFixedScale;

    const int32_t min_tx = position.x / tile_fp;
    const int32_t max_tx = (position.x + size_fp - 1) / tile_fp;
    const int32_t min_ty = position.y / tile_fp;
    const int32_t max_ty = (position.y + size_fp - 1) / tile_fp;

    for (int32_t ty = min_ty; ty <= max_ty; ++ty) {
        for (int32_t tx = min_tx; tx <= max_tx; ++tx) {
            if (is_wall(tx, ty)) {
                return true;
            }
        }
    }
    return false;
}

// Bases at the left/right midline pockets (README §7.1: red left).
const Vec2Fixed kRedBasePosition = tile_to_fp(2, 12);
const Vec2Fixed kBlueBasePosition = tile_to_fp(37, 12);

const Vec2Fixed kRedSpawnPoints[config::kMaxPlayers] = {
    tile_to_fp(2, 5),  tile_to_fp(2, 7),  tile_to_fp(2, 9),
    tile_to_fp(2, 11), tile_to_fp(2, 13), tile_to_fp(1, 12),
    tile_to_fp(3, 10), tile_to_fp(3, 14), tile_to_fp(1, 9),
    tile_to_fp(1, 15),
};

const Vec2Fixed kBlueSpawnPoints[config::kMaxPlayers] = {
    tile_to_fp(37, 5),  tile_to_fp(37, 7),  tile_to_fp(37, 9),
    tile_to_fp(37, 11), tile_to_fp(37, 13), tile_to_fp(38, 12),
    tile_to_fp(36, 10), tile_to_fp(36, 14), tile_to_fp(38, 9),
    tile_to_fp(38, 15),
};

} // namespace ctf

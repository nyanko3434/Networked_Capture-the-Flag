#include "map.h"

// README §7.1: 40 x 25 tiles at 32 px, two tile types, hardcoded and
// mirrored left-right (tile(x, y) == tile(39 - x, y) for every tile). Red
// base left, blue base right. Bases and spawn points are coordinate
// constants, not tile types — they sit on open (non-wall) tiles below.
//
// Layout: a bordered arena with four symmetric interior wall clusters (two
// small blocks near each base's approach, two larger pillars flanking
// mid-field, plus a short center-line divider) giving the movement/collision
// tests and later combat/DDA-march code real geometry to interact with,
// while staying trivially verifiable as left-right symmetric.

namespace ctf {

namespace {

// clang-format off
const char* kMapRows[config::kMapHeightTiles] = {
    "########################################",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#....##..........................##....#",
    "#....##..........................##....#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#.......###..................###.......#",
    "#.......###........##........###.......#",
    "#.......###........##........###.......#",
    "#.......###........##........###.......#",
    "#.......###..................###.......#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#....##..........................##....#",
    "#....##..........................##....#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "########################################",
};
// clang-format on

// Converts a tile coordinate to a fixed-point pixel position (top-left
// corner of that tile), in the 1/256 px internal representation (README
// §5.1, config::kFixedScale).
constexpr int32_t tile_to_fixed(int32_t tile_coord) {
    return tile_coord * config::kTileSizePx * config::kFixedScale;
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

// 24x24 AABB vs. the tile grid (README §7.2). `position` is the fixed-point
// top-left corner of the collider. No sqrt / floating point — the box's
// pixel extent is truncated down to a tile range and every tile it overlaps
// is checked directly.
bool Map::aabb_collides(Vec2Fixed position) const {
    // Fixed-point (1/256 px) -> integer pixel, truncating toward zero. Player
    // positions are expected non-negative (inside or approaching the map),
    // so a right-shift is exact for this domain.
    const int32_t left_px = position.x >> config::kFixedShift;
    const int32_t top_px = position.y >> config::kFixedShift;
    const int32_t right_px = left_px + config::kPlayerSizePx - 1;
    const int32_t bottom_px = top_px + config::kPlayerSizePx - 1;

    const int32_t tile_x0 = left_px / config::kTileSizePx;
    const int32_t tile_x1 = right_px / config::kTileSizePx;
    const int32_t tile_y0 = top_px / config::kTileSizePx;
    const int32_t tile_y1 = bottom_px / config::kTileSizePx;

    for (int32_t ty = tile_y0; ty <= tile_y1; ++ty) {
        for (int32_t tx = tile_x0; tx <= tile_x1; ++tx) {
            if (is_wall(tx, ty)) {
                return true;
            }
        }
    }
    return false;
}

// Bases: red left, blue right, mirrored about the map's centerline, both on
// open ground clear of the wall clusters above.
const Vec2Fixed kRedBasePosition{tile_to_fixed(2), tile_to_fixed(12)};
const Vec2Fixed kBlueBasePosition{tile_to_fixed(37), tile_to_fixed(12)}; // 39-2=37

// Spawn points: one lane per side (column 3 / column 36, both clear for
// their full height in the layout above), 10 points spread down the lane,
// mirrored left-right per player slot.
const Vec2Fixed kRedSpawnPoints[config::kMaxPlayers] = {
    {tile_to_fixed(3), tile_to_fixed(2)},
    {tile_to_fixed(3), tile_to_fixed(4)},
    {tile_to_fixed(3), tile_to_fixed(6)},
    {tile_to_fixed(3), tile_to_fixed(8)},
    {tile_to_fixed(3), tile_to_fixed(10)},
    {tile_to_fixed(3), tile_to_fixed(12)},
    {tile_to_fixed(3), tile_to_fixed(14)},
    {tile_to_fixed(3), tile_to_fixed(16)},
    {tile_to_fixed(3), tile_to_fixed(18)},
    {tile_to_fixed(3), tile_to_fixed(20)},
};
const Vec2Fixed kBlueSpawnPoints[config::kMaxPlayers] = {
    {tile_to_fixed(36), tile_to_fixed(2)}, // 39-3=36
    {tile_to_fixed(36), tile_to_fixed(4)},
    {tile_to_fixed(36), tile_to_fixed(6)},
    {tile_to_fixed(36), tile_to_fixed(8)},
    {tile_to_fixed(36), tile_to_fixed(10)},
    {tile_to_fixed(36), tile_to_fixed(12)},
    {tile_to_fixed(36), tile_to_fixed(14)},
    {tile_to_fixed(36), tile_to_fixed(16)},
    {tile_to_fixed(36), tile_to_fixed(18)},
    {tile_to_fixed(36), tile_to_fixed(20)},
};

} // namespace ctf

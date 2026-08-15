#include "map.h"

// Scaffolding only — the real map layout, collision resolution, and base /
// spawn placement are designed together in the shared/ pairing session
// (README §11 week 1). The placeholder grid below is a bordered empty room:
// symmetric by construction, so "mirrored left-right" holds trivially until
// a real layout replaces it.

namespace ctf {

namespace {

// clang-format off
const char* kMapRows[config::kMapHeightTiles] = {
    "########################################",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "#......................................#",
    "########################################",
};
// clang-format on

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

// Collision resolution (AABB vs. grid, X-then-Y) is written together in the
// shared/ pairing session (README §7.2).
bool Map::aabb_collides(Vec2Fixed) const { return false; }

// Placeholder coordinates — real base/spawn layout designed together with
// the map (README §7.1).
const Vec2Fixed kRedBasePosition{0, 0};
const Vec2Fixed kBlueBasePosition{0, 0};
const Vec2Fixed kRedSpawnPoints[config::kMaxPlayers] = {};
const Vec2Fixed kBlueSpawnPoints[config::kMaxPlayers] = {};

} // namespace ctf

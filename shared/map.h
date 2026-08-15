#pragma once

// Tile grid and collision queries (README §7.1). 40 x 25 tiles at 32 px,
// two tile types, hardcoded and mirrored left-right. Red base left, blue
// base right. Bases and spawn points are coordinate constants, not tile
// types.

#include <cstdint>

#include "game_config.h"
#include "game_types.h"

namespace ctf {

enum class TileType : uint8_t {
    Empty = 0,
    Wall = 1,
};

class Map {
public:
    Map();

    TileType tile_at(int32_t tile_x, int32_t tile_y) const;
    bool is_wall(int32_t tile_x, int32_t tile_y) const;

    // 24x24 AABB vs. the tile grid (README §7.2).
    bool aabb_collides(Vec2Fixed position) const;
};

// Coordinate constants — not tile types (README §7.1).
extern const Vec2Fixed kRedBasePosition;
extern const Vec2Fixed kBlueBasePosition;
extern const Vec2Fixed kRedSpawnPoints[config::kMaxPlayers];
extern const Vec2Fixed kBlueSpawnPoints[config::kMaxPlayers];

} // namespace ctf

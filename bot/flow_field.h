#pragma once

// BFS flow-field navigation for the bot (docs/optimization.md §navigation).
//
// The tile grid is static, so a breadth-first distance field from a source
// tile is computed once and reused forever: dist[t] = fewest tile steps
// from the source to t through non-wall tiles. Walking strictly downhill
// in this field is guaranteed to reach the source — no greedy local
// minima, no oscillation, no stuck bots in front of walls. The row-8/16
// walls with their narrow gaps are routed around by construction.
//
// Integer-only: BFS over 40x25 tiles, uint16 distances.

#include <array>
#include <cstdint>

#include "game_config.h"
#include "game_types.h"
#include "map.h"

namespace ctf {

class FlowField {
public:
    static constexpr uint16_t kUnreachable = 0xFFFF;

    // Rebuilds the field from the tile containing `source_fp` (a base or
    // flag position). If the source tile itself is solid the whole field
    // stays unreachable — callers only pass known-open positions.
    void build(const Map& map, const Vec2Fixed& source_fp);

    uint16_t dist_at(int32_t tx, int32_t ty) const;

    // Tile coordinates of a fixed-point position, clamped into the grid.
    static void tile_of(const Vec2Fixed& pos_fp, int32_t* tx, int32_t* ty);

    // Movement buttons (InputButton bits) that descend the field toward
    // the source. Each axis must be both field-descending AND physically
    // feasible (an AABB probe of the actual next step must be free); while
    // moving, the box is actively re-centered in its corridor so it never
    // clips wall-gap corners. Returns 0 at the source tile; if wedged,
    // wiggles toward the current tile's center until descent resumes.
    uint8_t steer(const Map& map, const Vec2Fixed& pos_fp) const;

private:
    // A tile is navigable if it is not a wall AND the 24x24 player box,
    // centered in the tile, fits without overlapping any wall.
    bool tile_passable(const Map& map, int32_t tx, int32_t ty) const;

    std::array<std::array<uint16_t, config::kMapWidthTiles>,
               config::kMapHeightTiles> dist_{};
};

} // namespace ctf

// shared/map tests (implementation_guide.md §1.4 acceptance criteria):
// mirror symmetry, AABB-vs-wall collision (baseline + X-only + Y-only
// overlap), and a no-sqrt/no-floating-point sweep is done separately via
// grep since it's a static property of the source, not a runtime check.

#include "game_config.h"
#include "map.h"

#include <cassert>
#include <cstdio>

using namespace ctf;

namespace {

constexpr int32_t kTilePx = config::kTileSizePx;    // 32
constexpr int32_t kFixed = config::kFixedScale;      // 256, 1/256 px units
constexpr int32_t kPxToFixed = kFixed;               // multiply px by this to get fixed units

void test_mirror_symmetry() {
    Map map;
    int mismatches = 0;
    for (int32_t y = 0; y < config::kMapHeightTiles; ++y) {
        for (int32_t x = 0; x < config::kMapWidthTiles; ++x) {
            if (map.tile_at(x, y) != map.tile_at(39 - x, y)) {
                ++mismatches;
            }
        }
    }
    assert(mismatches == 0);
    printf("test_mirror_symmetry: OK (0/%d mismatches)\n",
           config::kMapWidthTiles * config::kMapHeightTiles);
}

void test_aabb_fully_inside_empty_tile_no_collision() {
    Map map;
    // Tile (15, 15) is open field in the current layout. Place the 24x24
    // box at the tile's top-left corner -- with a 24px box inside a 32px
    // tile it never leaves that single (empty) tile.
    Vec2Fixed pos{15 * kTilePx * kPxToFixed, 15 * kTilePx * kPxToFixed};
    assert(!map.aabb_collides(pos));
    printf("test_aabb_fully_inside_empty_tile_no_collision: OK\n");
}

void test_aabb_x_only_overlap() {
    Map map;
    // Wall cluster occupies tile x in [8,10], y in [10,14]. Tile (7,10) is
    // open, tile (8,10) is wall. Keep the box fully within row 10's single
    // tile band vertically (no Y overlap into adjacent rows) throughout.
    const int32_t top_px = 10 * kTilePx; // row 10 top, box stays inside this row

    // Baseline: box fully within tile (7,10) -> no collision.
    {
        const int32_t left_px = 7 * kTilePx; // 224
        Vec2Fixed pos{left_px * kPxToFixed, top_px * kPxToFixed};
        assert(!map.aabb_collides(pos));
    }

    // Shifted right so the box's rightmost pixel just enters tile (8,10),
    // which is a wall -- X-axis overlap only, Y stays within one tile row.
    {
        const int32_t left_px = 8 * kTilePx - config::kPlayerSizePx + 1; // 233
        Vec2Fixed pos{left_px * kPxToFixed, top_px * kPxToFixed};
        assert(map.aabb_collides(pos));
    }
    printf("test_aabb_x_only_overlap: OK\n");
}

void test_aabb_y_only_overlap() {
    Map map;
    // Column 8: tile (8,9) is open (above the wall cluster), tile (8,10) is
    // wall (top row of the cluster). Keep the box fully within tile column 8
    // horizontally throughout (no X overlap into adjacent columns).
    const int32_t left_px = 8 * kTilePx + 4; // comfortably inside column 8 (256..287)

    // Baseline: box fully within tile (8,9) -> no collision.
    {
        const int32_t top_px = 9 * kTilePx; // 288
        Vec2Fixed pos{left_px * kPxToFixed, top_px * kPxToFixed};
        assert(!map.aabb_collides(pos));
    }

    // Shifted down so the box's bottom pixel just enters tile (8,10), which
    // is a wall -- Y-axis overlap only, X stays within one tile column.
    {
        const int32_t top_px = 10 * kTilePx - config::kPlayerSizePx + 1; // 297
        Vec2Fixed pos{left_px * kPxToFixed, top_px * kPxToFixed};
        assert(map.aabb_collides(pos));
    }
    printf("test_aabb_y_only_overlap: OK\n");
}

} // namespace

int main() {
    test_mirror_symmetry();
    test_aabb_fully_inside_empty_tile_no_collision();
    test_aabb_x_only_overlap();
    test_aabb_y_only_overlap();
    printf("All map tests passed.\n");
    return 0;
}

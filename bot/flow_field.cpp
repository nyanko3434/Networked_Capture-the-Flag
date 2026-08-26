#include "flow_field.h"

#include <deque>

namespace ctf {

namespace {

constexpr int32_t kTileFp = config::kTileSizePx * config::kFixedScale;
constexpr int32_t kBoxFp = config::kPlayerSizePx * config::kFixedScale;
// Margin that centers the player box inside a 32px tile: (32-24)/2 = 4 px.
constexpr int32_t kCenterMarginFp = (kTileFp - kBoxFp) / 2;

struct Tile {
    int32_t x;
    int32_t y;
};

} // namespace

void FlowField::tile_of(const Vec2Fixed& pos_fp, int32_t* tx, int32_t* ty) {
    int32_t x = pos_fp.x / kTileFp;
    int32_t y = pos_fp.y / kTileFp;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= config::kMapWidthTiles) x = config::kMapWidthTiles - 1;
    if (y >= config::kMapHeightTiles) y = config::kMapHeightTiles - 1;
    *tx = x;
    *ty = y;
}

bool FlowField::tile_passable(const Map& map, int32_t tx, int32_t ty) const {
    if (tx < 0 || ty < 0 || tx >= config::kMapWidthTiles ||
        ty >= config::kMapHeightTiles) {
        return false;
    }
    if (map.is_wall(tx, ty)) return false;
    const Vec2Fixed centered{tx * kTileFp + kCenterMarginFp,
                             ty * kTileFp + kCenterMarginFp};
    return !map.aabb_collides(centered);
}

void FlowField::build(const Map& map, const Vec2Fixed& source_fp) {
    for (auto& row : dist_) {
        row.fill(kUnreachable);
    }

    int32_t sx;
    int32_t sy;
    tile_of(source_fp, &sx, &sy);
    if (!tile_passable(map, sx, sy)) return;

    // BFS over at most W*H tiles; the ring keeps this allocation-free.
    std::deque<Tile> queue;
    dist_[static_cast<size_t>(sy)][static_cast<size_t>(sx)] = 0;
    queue.push_back({sx, sy});

    while (!queue.empty()) {
        const Tile cur = queue.front();
        queue.pop_front();
        const uint16_t next =
            dist_[static_cast<size_t>(cur.y)][static_cast<size_t>(cur.x)] + 1;

        const Tile neighbors[4] = {{cur.x + 1, cur.y}, {cur.x - 1, cur.y},
                                   {cur.x, cur.y + 1}, {cur.x, cur.y - 1}};
        for (const Tile& n : neighbors) {
            if (n.x < 0 || n.y < 0 || n.x >= config::kMapWidthTiles ||
                n.y >= config::kMapHeightTiles) {
                continue;
            }
            auto& d = dist_[static_cast<size_t>(n.y)]
                           [static_cast<size_t>(n.x)];
            if (d != kUnreachable) continue; // visited
            if (!tile_passable(map, n.x, n.y)) continue;
            d = next;
            queue.push_back(n);
        }
    }
}

uint16_t FlowField::dist_at(int32_t tx, int32_t ty) const {
    if (tx < 0 || ty < 0 || tx >= config::kMapWidthTiles ||
        ty >= config::kMapHeightTiles) {
        return kUnreachable;
    }
    return dist_[static_cast<size_t>(ty)][static_cast<size_t>(tx)];
}

uint8_t FlowField::steer(const Map& map, const Vec2Fixed& pos_fp) const {
    int32_t cx;
    int32_t cy;
    tile_of(pos_fp, &cx, &cy);
    const uint16_t here = dist_at(cx, cy);
    if (here == 0 || here == kUnreachable) return 0;

    const int32_t step = config::kMoveSpeedFpPerTick;
    // Physical feasibility: a descending neighbor tile is useless if the
    // actual player box cannot legally take a step in that direction
    // right now (it may be hugging a wall edge and need re-centering
    // first).
    auto axis_feasible = [&](int32_t dx, int32_t dy) {
        const Vec2Fixed probe{pos_fp.x + dx * step, pos_fp.y + dy * step};
        return !map.aabb_collides(probe);
    };

    bool have_x = false, have_y = false;
    uint16_t best_x = kUnreachable, best_y = kUnreachable;
    uint8_t btn_x = 0, btn_y = 0;

    const struct {
        int32_t dx;
        int32_t dy;
        uint8_t bit;
    } cardinals[4] = {
        {1, 0, kInputRight}, {-1, 0, kInputLeft},
        {0, 1, kInputDown},  {0, -1, kInputUp},
    };

    for (const auto& c : cardinals) {
        const int32_t nx = cx + c.dx;
        const int32_t ny = cy + c.dy;
        if (!tile_passable(map, nx, ny)) continue;
        const uint16_t nd = dist_at(nx, ny);
        if (nd >= here) continue;
        if (!axis_feasible(c.dx, c.dy)) continue;
        if (c.dy == 0) {
            if (nd < best_x) {
                best_x = nd;
                btn_x = c.bit;
            }
            have_x = true;
        } else {
            if (nd < best_y) {
                best_y = nd;
                btn_y = c.bit;
            }
            have_y = true;
        }
    }

    // Re-centering: while travelling horizontally, align the box with the
    // middle of its row (and vice versa). A centered box never clips the
    // corners of wall gaps — the root cause of wedge-stuck bots.
    const Vec2Fixed center{
        cx * kTileFp + kCenterMarginFp, cy * kTileFp + kCenterMarginFp};
    constexpr int32_t kAlignTol = kTileFp / 8; // ~4 px

    auto press_toward_center_y = [&]() -> uint8_t {
        if (pos_fp.y < center.y - kAlignTol &&
            axis_feasible(0, 1)) return kInputDown;
        if (pos_fp.y > center.y + kAlignTol &&
            axis_feasible(0, -1)) return kInputUp;
        return 0;
    };
    auto press_toward_center_x = [&]() -> uint8_t {
        if (pos_fp.x < center.x - kAlignTol &&
            axis_feasible(1, 0)) return kInputRight;
        if (pos_fp.x > center.x + kAlignTol &&
            axis_feasible(-1, 0)) return kInputLeft;
        return 0;
    };

    if (have_x && have_y) return static_cast<uint8_t>(btn_x | btn_y);
    if (have_x) {
        // Horizontal descent: correct vertical alignment while moving.
        if (const uint8_t c = press_toward_center_y()) return btn_x | c;
        return btn_x;
    }
    if (have_y) {
        if (const uint8_t c = press_toward_center_x()) return btn_y | c;
        return btn_y;
    }

    // Wedged: every descending axis is physically blocked right now.
    // Wiggle toward the center of the current tile on whatever axis is
    // feasible — once the box clears the wall edge, descent resumes.
    if (const uint8_t c = press_toward_center_y()) return c;
    if (const uint8_t c = press_toward_center_x()) return c;
    return 0;
}

} // namespace ctf

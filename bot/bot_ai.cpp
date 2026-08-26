#include "bot_ai.h"

#include <cmath>

namespace ctf {

BotAI::BotAI(uint32_t seed, uint8_t player_id, Team team)
    : rng_(seed), my_id_(player_id), my_team_(team),
      aim_spread_rng_(0.0f, 0.3f), // ~17 degrees of random spread
      fire_chance_(0.4) {          // 40% chance to fire per visible tick
    own_base_field_.build(map_, my_team_ == Team::Blue ? kBlueBasePosition
                                                       : kRedBasePosition);
    enemy_base_field_.build(map_, my_team_ == Team::Blue ? kRedBasePosition
                                                         : kBlueBasePosition);
}

const PlayerState* BotAI::find_self(const WorldSnapshot& snap) const {
    for (uint8_t i = 0; i < snap.player_count; ++i)
        if (snap.players[i].id == my_id_) return &snap.players[i];
    return nullptr;
}

// Greedy candidate stepping (the old movement core). Fine in the open and
// for the last few pixels; never used for corridor routing.
uint8_t BotAI::greedy_buttons(const Vec2Fixed& my_pos, const Vec2Fixed&,
                              float target_x, float target_y) {
    const float my_x =
        static_cast<float>(my_pos.x) / config::kFixedScale;
    const float my_y =
        static_cast<float>(my_pos.y) / config::kFixedScale;
    const float dx = target_x - my_x;
    const float dy = target_y - my_y;
    if (std::abs(dx) <= 4.0f && std::abs(dy) <= 4.0f) return 0; // arrived

    const int32_t step = config::kMoveSpeedFpPerTick;
    struct Candidate {
        uint8_t btn;
        float dist;
        bool ok;
    };
    Candidate best = {0, 1e9f, false};

    const uint8_t dirs[] = {kInputRight, kInputLeft, kInputDown, kInputUp};
    const int32_t offsets_x[] = {step, -step, 0, 0};
    const int32_t offsets_y[] = {0, 0, step, -step};

    for (int d = 0; d < 4; ++d) {
        Vec2Fixed test = my_pos;
        test.x += offsets_x[d];
        test.y += offsets_y[d];
        if (map_.aabb_collides(test)) continue;

        const float nx =
            my_x + static_cast<float>(offsets_x[d]) / config::kFixedScale;
        const float ny =
            my_y + static_cast<float>(offsets_y[d]) / config::kFixedScale;
        const float ndx = target_x - nx;
        const float ndy = target_y - ny;
        const float ndist = ndx * ndx + ndy * ndy;
        if (ndist < best.dist) best = {dirs[d], ndist, true};
    }

    // Diagonal combo when both axes still have distance to cover.
    if (std::abs(dx) > 4.0f && std::abs(dy) > 4.0f) {
        const int32_t sx = dx > 0 ? step : -step;
        const int32_t sy = dy > 0 ? step : -step;
        Vec2Fixed diag = my_pos;
        diag.x += sx;
        diag.y += sy;
        if (!map_.aabb_collides(diag)) {
            const float nx = my_x + static_cast<float>(sx) / config::kFixedScale;
            const float ny = my_y + static_cast<float>(sy) / config::kFixedScale;
            const float ndx = target_x - nx;
            const float ndy = target_y - ny;
            const float ndist = ndx * ndx + ndy * ndy;
            if (ndist < best.dist) {
                uint8_t combo = 0;
                if (dx > 0) combo |= kInputRight; else combo |= kInputLeft;
                if (dy > 0) combo |= kInputDown; else combo |= kInputUp;
                best = {combo, ndist, true};
            }
        }
    }

    return best.ok ? best.btn : 0;
}

InputCmd BotAI::tick(const WorldSnapshot& snap) {
    const PlayerState* self = find_self(snap);
    if (self == nullptr) return InputCmd{0, 0};

    // Nearest living enemy, for combat.
    const float my_x =
        static_cast<float>(self->motion.position.x) / config::kFixedScale;
    const float my_y =
        static_cast<float>(self->motion.position.y) / config::kFixedScale;
    float nearest_dist = 99999.0f;
    float enemy_dx = 0, enemy_dy = 0;
    bool have_enemy = false;
    for (uint8_t i = 0; i < snap.player_count; ++i) {
        const PlayerState& p = snap.players[i];
        if (p.id == my_id_ || p.team == my_team_ || !p.alive) continue;
        const float ex =
            static_cast<float>(p.motion.position.x) / config::kFixedScale;
        const float ey =
            static_cast<float>(p.motion.position.y) / config::kFixedScale;
        const float dx = ex - my_x;
        const float dy = ey - my_y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < nearest_dist) {
            nearest_dist = dist;
            enemy_dx = dx;
            enemy_dy = dy;
            have_enemy = true;
        }
    }

    const bool red_is_mine = my_team_ == Team::Red;
    const Vec2Fixed& own_base =
        red_is_mine ? kRedBasePosition : kBlueBasePosition;
    const Vec2Fixed& enemy_base =
        red_is_mine ? kBlueBasePosition : kRedBasePosition;
    // Our flag = the one sitting in our base; enemies steal it.
    const FlagState own_flag_state =
        red_is_mine ? snap.flag_state_red : snap.flag_state_blue;
    const uint8_t own_flag_carrier =
        red_is_mine ? snap.flag_carrier_red : snap.flag_carrier_blue;

    // Navigation priorities:
    //   1. carrying -> run home via field
    //   2. enemy raids WITH our flag -> hunt that carrier down (killing
    //      them drops our flag where they stand)
    //   3. otherwise -> push to the enemy base (steal the flag there; a
    //      dropped enemy flag auto-returns home after 450 ticks, so
    //      loitering near its base stays correct)
    Vec2Fixed nav_target = enemy_base;
    const FlowField* field = &enemy_base_field_;
    bool chase_directly = false;

    if (self->carrying_flag) {
        field = &own_base_field_;
        nav_target = own_base;
    } else if (own_flag_state == FlagState::Carried &&
               own_flag_carrier < config::kMaxPlayers) {
        const PlayerState* carrier = nullptr;
        for (uint8_t i = 0; i < snap.player_count; ++i) {
            if (snap.players[i].id == own_flag_carrier) {
                carrier = &snap.players[i];
                break;
            }
        }
        if (carrier != nullptr && carrier->team != my_team_) {
            field = nullptr;
            nav_target = carrier->motion.position;
            chase_directly = true;
        }
    }
    // Everything else keeps the default: flow to the enemy base.

    uint8_t buttons = 0;
    if (chase_directly) {
        buttons = greedy_buttons(
            self->motion.position, nav_target,
            static_cast<float>(nav_target.x) / config::kFixedScale,
            static_cast<float>(nav_target.y) / config::kFixedScale);
    } else {
        buttons = field->steer(map_, self->motion.position);
        // Final approach: inside the last couple of tiles, close the exact
        // gap to the objective with direct steering (the field only knows
        // tile centers).
        int32_t tx;
        int32_t ty;
        FlowField::tile_of(self->motion.position, &tx, &ty);
        if (field->dist_at(tx, ty) <= 2 || buttons == 0) {
            buttons = greedy_buttons(
                self->motion.position, nav_target,
                static_cast<float>(nav_target.x) / config::kFixedScale,
                static_cast<float>(nav_target.y) / config::kFixedScale);
        }
    }

    // Aim and shoot with imperfection.
    uint16_t aim = 0;
    if (have_enemy && nearest_dist < 350.0f) {
        const float spread = aim_spread_rng_(rng_);
        const float angle = std::atan2(enemy_dy, enemy_dx) + spread;
        aim = static_cast<uint16_t>(
            angle / (2.0f * 3.14159265f) * 65536.0f);
        if (fire_chance_(rng_)) buttons |= kInputFire;
    } else if (buttons &
               (kInputLeft | kInputRight | kInputUp | kInputDown)) {
        float mx = 0, my = 0;
        if (buttons & kInputRight) mx += 1;
        if (buttons & kInputLeft) mx -= 1;
        if (buttons & kInputDown) my += 1;
        if (buttons & kInputUp) my -= 1;
        aim = static_cast<uint16_t>(
            std::atan2(my, mx) / (2.0f * 3.14159265f) * 65536.0f);
    }

    return InputCmd{buttons, aim};
}

} // namespace ctf

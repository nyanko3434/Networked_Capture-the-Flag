#pragma once

// Bot AI, extracted from main.cpp so tests can drive it headlessly
// (docs/optimization.md §navigation).
//
// Navigation uses precomputed BFS flow fields instead of the old memoryless
// greedy step-steering: walking downhill in a distance field cannot get
// stuck against walls or oscillate in front of obstacles — every step lands
// on a strictly smaller distance-to-target.
//
// Target selection given the enemy flag's state (the wire format carries
// flag STATE and CARRIER only — never a dropped-flag position):
//   carrying the flag        -> flow field to our own base
//   enemy flag at base/dropped -> flow field to the enemy base (a dropped
//                               flag auto-returns after 450 ticks, so
//                               loitering near its home is correct play)
//   enemy flag carried by teammate -> defend our own base (capture needs
//                               our flag home)
//   enemy flag carried by an enemy -> direct chase of the carrier (a moving
//                               target needs no field)
// Combat: hitscan aim with spread at the nearest living enemy, imperfect
// fire discipline so bots don't feel robotic.

#include <cstdint>
#include <random>

#include "flow_field.h"
#include "game_types.h"
#include "map.h"

namespace ctf {

class BotAI {
public:
    BotAI(uint32_t seed, uint8_t player_id, Team team);

    // One decision from the latest world snapshot.
    InputCmd tick(const WorldSnapshot& snap);

private:
    const PlayerState* find_self(const WorldSnapshot& snap) const;

    // Old-style greedy candidate stepping: probes the 4 cardinal + 1
    // diagonal single-step moves and keeps whichever lands closest to the
    // target without colliding. Used only for short-range final approach
    // and chasing a moving flag carrier, never for corridor routing.
    uint8_t greedy_buttons(const Vec2Fixed& my_pos, const Vec2Fixed& target,
                           float target_x, float target_y);

    std::mt19937 rng_;
    uint8_t my_id_;
    Team my_team_;
    const Map& map_ = Map::instance();
    std::uniform_real_distribution<float> aim_spread_rng_;
    std::bernoulli_distribution fire_chance_;

    // Static, built once in the constructor.
    FlowField own_base_field_;
    FlowField enemy_base_field_;
};

} // namespace ctf

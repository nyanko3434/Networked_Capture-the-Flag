#pragma once

// Core value types shared by server, client, and bot (README §5.5, §7.2).
//
// Positions are always fixed-point integers, never floats: internal
// representation is int32 in 1/256 px units (README §5.1). Float divergence
// between client and server — including from different compiler
// optimization flags — breaks reconciliation, so no floating-point type may
// enter this header or shared/movement.h.

#include <array>
#include <cstdint>

#include "game_config.h"

namespace ctf {

// A fixed-point 2D position or velocity, in 1/256 px units.
struct Vec2Fixed {
    int32_t x = 0;
    int32_t y = 0;
};

enum class Team : uint8_t {
    Red = 0,
    Blue = 1,
};

enum class FlagState : uint8_t {
    AtBase = 0,
    Carried = 1,
    Dropped = 2,
};

// Input buttons bitmask (README §5.4 PLAYER_INPUT payload).
enum InputButton : uint8_t {
    kInputUp = 1u << 0,
    kInputDown = 1u << 1,
    kInputLeft = 1u << 2,
    kInputRight = 1u << 3,
    kInputFire = 1u << 4,
};

// One input sample as carried on the wire: buttons + aim angle
// (README §5.4). `aim_angle` covers 0..65535 -> 0..2*pi.
struct InputCmd {
    uint8_t buttons = 0;
    uint16_t aim_angle = 0;
};

// The only state movement_step touches: position, velocity in; position,
// velocity out. No weapon state, no health, no team, no timers (README §7.2).
struct PlayerMotion {
    Vec2Fixed position;
    Vec2Fixed velocity;
};

// Full per-player simulation state, as carried in a WorldSnapshot.
struct PlayerState {
    uint8_t id = 0;
    Team team = Team::Red;
    PlayerMotion motion;
    uint16_t aim_angle = 0;
    uint8_t health = config::kMaxHealth;
    bool alive = true;
    bool carrying_flag = false;
    bool firing = false;
};

struct FlagInfo {
    FlagState state = FlagState::AtBase;
    Vec2Fixed position;
    uint8_t carrier_id = 0xFF; // 0xFF = none
};

// WORLD_SNAPSHOT body (README §5.5). `last_input_seq` is per-recipient: the
// body is serialized once per tick, then those 4 bytes are patched per
// recipient before each sendto.
struct WorldSnapshot {
    uint32_t tick = 0;
    uint32_t last_input_seq = 0;

    uint8_t player_count = 0;
    uint8_t flag_carrier_red = 0xFF;
    uint8_t flag_carrier_blue = 0xFF;
    FlagState flag_state_red = FlagState::AtBase;
    FlagState flag_state_blue = FlagState::AtBase;
    uint8_t score_red = 0;
    uint8_t score_blue = 0;
    uint16_t seconds_remaining = 0;

    std::array<PlayerState, config::kMaxPlayers> players{};
};

} // namespace ctf

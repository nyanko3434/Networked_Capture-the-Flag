#pragma once

// Simulation (README §3.2, §4). Owns all game state — positions, health,
// flags, scores, active roster. Never touches a socket; communicates
// exclusively through the two queues.
//
// run() is a thin wrapper around the pure single-step tick(), which is
// public so tests can drive matches deterministically without threads or
// real time: push InboundCommands, call tick(), inspect snapshot()/events.
//
// Fixed order inside tick() (README §3.2):
//   drain inbound -> apply commands -> pop one input per player -> movement
//   -> combat -> flags -> win check -> publish snapshot and events

#include "game_types.h"
#include "map.h"
#include "queues.h"

namespace ctf {

class Sim {
public:
    Sim(InboundQueue& inbound, OutboundQueue& outbound);

    // Runs the 30 Hz clock_nanosleep(TIMER_ABSTIME) loop until stop().
    void run();
    void stop();

    // One fixed-tick step of the whole simulation. Public for tests.
    void tick();

    // Last published snapshot (updated at the end of every tick).
    const WorldSnapshot& snapshot() const { return snapshot_; }

    uint32_t current_tick() const { return current_tick_; }
    bool match_over() const { return match_over_; }

    // Flag state as seen by tests (mirrors what snapshots publish).
    FlagState flag_state(Team team) const;
    uint8_t flag_carrier(Team team) const;

    // Test-only: teleport a joined player to an exact fixed-point position
    // (clamped inside the map). Lets flag/combat tests arrange geometry
    // deterministically instead of navigating the map tick by tick.
    void debug_place(uint8_t id, const Vec2Fixed& pos);

private:
    struct Player {
        bool active = false;
        Team team = Team::Red;
        Vec2Fixed position;
        uint16_t aim_angle = 0;
        int32_t health = config::kMaxHealth;
        bool alive = true;
        bool carrying_flag = false;
        uint8_t spawn_index = 0;

        int32_t respawn_timer = 0;   // ticks until respawn while dead
        int32_t fire_cooldown = 0;   // ticks until next shot allowed

        // Per-player input ring buffer of 8 (README §6.1).
        InputCmd ring[config::kInputRingSize] = {};
        uint32_t seqs[config::kInputRingSize] = {};
        int ring_head = 0; // next pop position
        int ring_count = 0;
        uint32_t newest_seq_ = 0;    // dedup for redundant/reordered inputs
        uint32_t last_input_seq = 0; // ack published in snapshots
    };

    // --- per-phase helpers, in tick() order ---
    void drain_inbound();
    void apply_join(uint8_t id, Team team, uint8_t spawn_index);
    void apply_leave(uint8_t id);
    void pop_inputs();
    void move_players();
    void combat();
    void handle_death(Player& victim);
    void flags_phase();
    void win_check();
    void publish();

    // Hitscan support: integer ray march from origin along aim_angle up to
    // the first wall; returns true if an enemy AABB was hit first and
    // writes the enemy index. No floats anywhere.
    bool cast_ray(const Vec2Fixed& origin, uint16_t aim_angle,
                  Team shooter_team, size_t* out_enemy);

    Player* find_player(uint8_t id);
    const Vec2Fixed& base_of(Team team) const;

    InboundQueue& inbound_;
    OutboundQueue& outbound_;
    Map map_;

    Player players_[config::kMaxPlayers];
    int player_count_ = 0;

    // Flags indexed by [0]=Red, [1]=Blue.
    FlagInfo flags_[2] = {};
    uint32_t drop_tick_[2] = {}; // for 450-tick auto-return

    uint8_t score_[2] = {};
    WorldSnapshot snapshot_;
    InputCmd applied_cmds_[config::kMaxPlayers]; // input applied this tick
    uint32_t player_acks_[config::kMaxPlayers] = {};
    Vec2Fixed last_ray_hit_;
    uint32_t current_tick_ = 0;
    bool match_over_ = false;
    bool running_ = false;
};

} // namespace ctf

#include "sim.h"

#include <ctime>
#include <cmath>
#include <algorithm>
#include <vector>

#include "movement.h"
#include "protocol.h"

// Simulation thread body (README §3.2). All game state lives here; the only
// things crossing the thread boundary are the two queues. tick() is the
// whole game in one pure-ish step so tests can drive matches without
// threads, sockets, or real time.

namespace ctf {

namespace {

// Integer-friendly direction table for hitscan rays (README §7.3). Built
// once at startup; server-only combat math never reaches the client, so
// this does not touch the desync-critical movement path.
constexpr int32_t kTrigScale = 16384; // Q14
struct TrigTable {
    int32_t sin_tab[256];
    int32_t cos_tab[256];
    TrigTable() {
        for (int i = 0; i < 256; ++i) {
            const double a = i * 3.14159265358979323846 / 128.0;
            sin_tab[i] = static_cast<int32_t>(std::sin(a) * kTrigScale);
            cos_tab[i] = static_cast<int32_t>(std::cos(a) * kTrigScale);
        }
    }
};
const TrigTable kTrig;

constexpr int32_t kRayStepFp = 4; // ~1/64 px per march step
constexpr int32_t kMaxRaySteps =
    (config::kMapWidthPx > config::kMapHeightPx ? config::kMapWidthPx
                                                : config::kMapHeightPx) *
    2 * config::kFixedScale / kRayStepFp;

int flag_index(Team team) { return team == Team::Blue ? 1 : 0; }

} // namespace

Sim::Sim(InboundQueue& inbound, OutboundQueue& outbound)
    : inbound_(inbound), outbound_(outbound) {
    flags_[0].position = kRedBasePosition;
    flags_[1].position = kBlueBasePosition;
}

void Sim::run() {
    running_ = true;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (running_) {
        next.tv_nsec += config::kTickDurationNs;
        while (next.tv_nsec >= 1'000'000'000L) {
            next.tv_nsec -= 1'000'000'000L;
            next.tv_sec += 1;
        }
        // Resync instead of catch-up if more than 3 ticks behind.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t behind_ns =
            (next.tv_sec - now.tv_sec) * 1'000'000'000L +
            (next.tv_nsec - now.tv_nsec);
        if (-behind_ns > 3 * config::kTickDurationNs) {
            next = now;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        tick();
    }
}

void Sim::stop() { running_ = false; }

Sim::Player* Sim::find_player(uint8_t id) {
    if (id >= config::kMaxPlayers) return nullptr;
    return players_[id].active ? &players_[id] : nullptr;
}

const Vec2Fixed& Sim::base_of(Team team) const {
    return team == Team::Blue ? kBlueBasePosition : kRedBasePosition;
}

FlagState Sim::flag_state(Team team) const {
    return flags_[flag_index(team)].state;
}

uint8_t Sim::flag_carrier(Team team) const {
    return flags_[flag_index(team)].carrier_id;
}

void Sim::debug_place(uint8_t id, const Vec2Fixed& pos) {
    Player* p = find_player(id);
    if (p == nullptr) return;
    const int32_t max_x = config::kMapWidthPx * config::kFixedScale -
                          config::kPlayerSizePx * config::kFixedScale;
    const int32_t max_y = config::kMapHeightPx * config::kFixedScale -
                          config::kPlayerSizePx * config::kFixedScale;
    p->position.x = std::max(0, std::min(pos.x, max_x));
    p->position.y = std::max(0, std::min(pos.y, max_y));
}

// ---------------------------------------------------------------------------
// inbound command application
// ---------------------------------------------------------------------------

void Sim::apply_join(uint8_t id, Team team, uint8_t spawn_index) {
    if (match_over_) return;
    Player* existing = find_player(id);
    if (existing != nullptr) return;
    if (id >= config::kMaxPlayers) return;

    Player& p = players_[id];
    p.active = true;
    p.team = team;
    const Vec2Fixed* spawns = (team == Team::Blue) ? kBlueSpawnPoints
                                                   : kRedSpawnPoints;
    p.spawn_index =
        spawn_index < config::kMaxPlayers ? spawn_index : 0;
    p.position = spawns[p.spawn_index];
    p.health = config::kMaxHealth;
    p.alive = true;
    p.carrying_flag = false;
    p.respawn_timer = 0;
    p.fire_cooldown = 0;
    p.ring_head = 0;
    p.ring_count = 0;
    p.last_input_seq = 0;
    ++player_count_;
}

void Sim::apply_leave(uint8_t id) {
    Player* p = find_player(id);
    if (p == nullptr) return;
    if (p->carrying_flag) {
        const int fi = p->team == Team::Blue ? 0 : 1; // enemy flag index
        flags_[fi].state = FlagState::Dropped;
        flags_[fi].position = p->position;
        flags_[fi].carrier_id = 0xFF;
        drop_tick_[fi] = current_tick_;
    }
    *p = Player{};
    --player_count_;
}

void Sim::drain_inbound() {
    InboundCommand cmd;
    while (inbound_.pop(cmd)) {
        if (match_over_) continue; // frozen world ignores commands
        switch (cmd.type) {
            case InboundCommandType::PlayerJoined:
                apply_join(cmd.player_id, cmd.team, cmd.spawn_index);
                break;
            case InboundCommandType::PlayerLeft:
                apply_leave(cmd.player_id);
                break;
            case InboundCommandType::PlayerInput:
                if (Player* p = find_player(cmd.player_id)) {
                    if (!p->alive) break;
                    // Input redundancy re-sends and reordered datagrams:
                    // strictly increasing seqs only (README §5.4).
                    if (cmd.seq <= p->newest_seq_) break;
                    p->newest_seq_ = cmd.seq;
                    if (p->ring_count == config::kInputRingSize) {
                        // Full: drop the oldest (README §6.1).
                        p->ring_head =
                            (p->ring_head + 1) % config::kInputRingSize;
                        --p->ring_count;
                    }
                    const int idx =
                        (p->ring_head + p->ring_count) %
                        config::kInputRingSize;
                    p->ring[idx] = cmd.input;
                    p->seqs[idx] = cmd.seq;
                    ++p->ring_count;
                }
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// input / movement
// ---------------------------------------------------------------------------

void Sim::pop_inputs() {
    for (int id = 0; id < config::kMaxPlayers; ++id) {
        Player& p = players_[id];
        if (!p.active) continue;

        InputCmd applied{};
        if (p.alive && p.ring_count > 0) {
            applied = p.ring[p.ring_head];
            p.last_input_seq = p.seqs[p.ring_head];
            p.ring_head = (p.ring_head + 1) % config::kInputRingSize;
            --p.ring_count;
        }
        // Empty buffer: zero-movement input, ack unchanged (README §6.1).
        applied_cmds_[id] = applied;
    }
}

void Sim::move_players() {
    for (int id = 0; id < config::kMaxPlayers; ++id) {
        Player& p = players_[id];
        if (!p.active || !p.alive) continue;

        PlayerMotion in;
        in.position = p.position;
        const PlayerMotion out = movement_step(in, applied_cmds_[id], map_);
        p.position = out.position;
        p.aim_angle = applied_cmds_[id].aim_angle;
    }
}

// ---------------------------------------------------------------------------
// combat (hitscan, README §7.3)
// ---------------------------------------------------------------------------

bool Sim::cast_ray(const Vec2Fixed& origin, uint16_t aim_angle,
                   Team shooter_team, size_t* out_enemy) {
    const int idx = (aim_angle >> 8) & 0xFF;
    const int32_t dir_x = (kTrig.cos_tab[idx] * kRayStepFp) / kTrigScale;
    const int32_t dir_y = (kTrig.sin_tab[idx] * kRayStepFp) / kTrigScale;

    int32_t px = origin.x;
    int32_t py = origin.y;
    const int32_t box = config::kPlayerSizePx * config::kFixedScale;

    for (int step = 0; step < kMaxRaySteps; ++step) {
        px += dir_x;
        py += dir_y;

        if (map_.is_wall(px / (config::kTileSizePx * config::kFixedScale),
                         py / (config::kTileSizePx * config::kFixedScale))) {
            last_ray_hit_ = Vec2Fixed{px, py};
            return false;
        }

        for (int id = 0; id < config::kMaxPlayers; ++id) {
            const Player& e = players_[id];
            if (!e.active || !e.alive || e.team == shooter_team) continue;
            if (px >= e.position.x && px < e.position.x + box &&
                py >= e.position.y && py < e.position.y + box) {
                last_ray_hit_ = Vec2Fixed{px, py};
                *out_enemy = static_cast<size_t>(id);
                return true;
            }
        }
    }
    last_ray_hit_ = Vec2Fixed{px, py};
    return false;
}

namespace {

// One encode overload per payload type so ser_event can dispatch.
void write_msg(ByteWriter& w, const protocol::MsgFlagPickedUp& m) { encode_flag_picked_up(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgFlagDropped& m) { encode_flag_dropped(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgFlagReturned& m) { encode_flag_returned(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgFlagCaptured& m) { encode_flag_captured(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgPlayerKilled& m) { encode_player_killed(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgPlayerRespawned& m) { encode_player_respawned(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgMatchEnd& m) { encode_match_end(w, m); }
void write_msg(ByteWriter& w, const protocol::MsgShotFired& m) { encode_shot_fired(w, m); }

// Wire form of an event: [u8 type][encoded fields]; network side frames it.
template <typename Msg>
void ser_event(OutboundEvent& ev, protocol::MessageType type, const Msg& msg) {
    ev.payload.push_back(static_cast<uint8_t>(type));
    constexpr size_t kMaxPayload = 96;
    const size_t start = ev.payload.size();
    ev.payload.resize(start + kMaxPayload);
    ByteWriter w(ev.payload.data() + start, kMaxPayload);
    write_msg(w, msg);
    ev.payload.resize(start + w.size());
}

} // namespace

void Sim::handle_death(Sim::Player& victim) {
    victim.alive = false;
    victim.health = 0;
    victim.respawn_timer = config::kRespawnDelayTicks;

    // Drop carried flag at the death spot (README §7.4).
    if (victim.carrying_flag) {
        const int fi = victim.team == Team::Blue ? 0 : 1;
        flags_[fi].state = FlagState::Dropped;
        flags_[fi].position = victim.position;
        flags_[fi].carrier_id = 0xFF;
        drop_tick_[fi] = current_tick_;

        OutboundEvent ev;
        ev.type = OutboundEventType::TcpBroadcast;
        protocol::MsgFlagDropped msg{victim.team == Team::Blue ? Team::Blue : Team::Red,
                           victim.position, current_tick_};
        ser_event(ev, protocol::MessageType::FlagDropped, msg);
        outbound_.push(ev);
        victim.carrying_flag = false;
    }
}

void Sim::combat() {
    for (int id = 0; id < config::kMaxPlayers; ++id) {
        Player& p = players_[id];
        if (!p.active || !p.alive) continue;
        if (p.fire_cooldown > 0) {
            --p.fire_cooldown;
            continue;
        }
        if ((applied_cmds_[id].buttons & kInputFire) == 0) continue;

        size_t enemy = config::kMaxPlayers;
        const bool hit = cast_ray(p.position, applied_cmds_[id].aim_angle,
                                  p.team, &enemy);

        // SHOT_FIRED is cosmetic and loss-tolerant -> UDP (README §5.4).
        {
            OutboundEvent ev;
            ev.type = OutboundEventType::UdpEvent;
            protocol::MsgShotFired msg;
            msg.shooter_id = static_cast<uint8_t>(id);
            msg.origin = p.position;
            msg.aim_angle = applied_cmds_[id].aim_angle;
            msg.hit_point = last_ray_hit_;
            ser_event(ev, protocol::MessageType::ShotFired, msg);
            outbound_.push(ev);
        }

        p.fire_cooldown = config::kFireCooldownTicks;

        if (!hit) continue;
        Player& v = players_[enemy];
        v.health = v.health > config::kDamagePerShot
                       ? v.health - config::kDamagePerShot
                       : 0;
        if (v.health == 0) {
            handle_death(v);
            OutboundEvent ev;
            ev.type = OutboundEventType::TcpBroadcast;
            protocol::MsgPlayerKilled msg{static_cast<uint8_t>(enemy),
                                static_cast<uint8_t>(id), current_tick_};
            ser_event(ev, protocol::MessageType::PlayerKilled, msg);
            outbound_.push(ev);
        }
    }
}

// ---------------------------------------------------------------------------
// flags phase (README §7.4)
// ---------------------------------------------------------------------------

namespace {

bool aabb_contains(const Vec2Fixed& box_top_left, const Vec2Fixed& point) {
    const int32_t box = config::kPlayerSizePx * config::kFixedScale;
    return point.x >= box_top_left.x && point.x < box_top_left.x + box &&
           point.y >= box_top_left.y && point.y < box_top_left.y + box;
}

} // namespace

void Sim::flags_phase() {
    // Carried flags follow their carrier; dropped flags may auto-return.
    for (int fi = 0; fi < 2; ++fi) {
        FlagInfo& flag = flags_[fi];
        if (flag.state == FlagState::Carried) {
            Player* carrier = find_player(flag.carrier_id);
            if (carrier != nullptr) {
                flag.position = carrier->position;
            }
        } else if (flag.state == FlagState::Dropped) {
            if (current_tick_ - drop_tick_[fi] >=
                static_cast<uint32_t>(config::kFlagAutoReturnTicks)) {
                flag.state = FlagState::AtBase;
                flag.position = base_of(fi == 0 ? Team::Red : Team::Blue);
                flag.carrier_id = 0xFF;
                OutboundEvent ev;
                ev.type = OutboundEventType::TcpBroadcast;
                ser_event(ev, protocol::MessageType::FlagReturned,
                          protocol::MsgFlagReturned{fi == 0 ? Team::Red : Team::Blue,
                                          0xFF, current_tick_});
                outbound_.push(ev);
            }
        }
    }

    for (int id = 0; id < config::kMaxPlayers; ++id) {
        Player& p = players_[id];
        if (!p.active || !p.alive) continue;
        const int own_fi = flag_index(p.team);
        const int enemy_fi = 1 - own_fi;

        // Own dropped flag touched -> instant return to base.
        FlagInfo& own = flags_[own_fi];
        if (own.state == FlagState::Dropped &&
            aabb_contains(p.position, own.position)) {
            own.state = FlagState::AtBase;
            own.position = base_of(p.team);
            own.carrier_id = 0xFF;
            OutboundEvent ev;
            ev.type = OutboundEventType::TcpBroadcast;
            ser_event(ev, protocol::MessageType::FlagReturned,
                      protocol::MsgFlagReturned{p.team, static_cast<uint8_t>(id),
                                      current_tick_});
            outbound_.push(ev);
        }

        // Enemy flag touch -> pickup (at base or dropped, not carried).
        FlagInfo& enemy = flags_[enemy_fi];
        if (!p.carrying_flag && enemy.state != FlagState::Carried &&
            aabb_contains(p.position, enemy.position)) {
            enemy.state = FlagState::Carried;
            enemy.carrier_id = static_cast<uint8_t>(id);
            p.carrying_flag = true;
            OutboundEvent ev;
            ev.type = OutboundEventType::TcpBroadcast;
            ser_event(ev, protocol::MessageType::FlagPickedUp,
                      protocol::MsgFlagPickedUp{p.team == Team::Red ? Team::Blue
                                                          : Team::Red,
                                      static_cast<uint8_t>(id),
                                      current_tick_});
            outbound_.push(ev);
        }

        // Capture: carry the enemy flag home — standing on YOUR OWN base
        // with your own flag at base scores (README §7.4 — the single if
        // that makes defense matter).
        const Team enemy_team = p.team == Team::Red ? Team::Blue : Team::Red;
        if (p.carrying_flag && own.state == FlagState::AtBase &&
            aabb_contains(p.position, base_of(p.team))) {
            p.carrying_flag = false;
            enemy.state = FlagState::AtBase;
            enemy.position = base_of(enemy_team);
            enemy.carrier_id = 0xFF;
            score_[own_fi]++;

            OutboundEvent ev;
            ev.type = OutboundEventType::TcpBroadcast;
            ser_event(ev, protocol::MessageType::FlagCaptured,
                      protocol::MsgFlagCaptured{enemy_team,
                                                static_cast<uint8_t>(id),
                                                current_tick_});
            outbound_.push(ev);
        }
    }

    // Respawn timers run inside the flags phase window of the tick order.
    for (int id = 0; id < config::kMaxPlayers; ++id) {
        Player& p = players_[id];
        if (!p.active || p.alive) continue;
        if (--p.respawn_timer <= 0) {
            p.alive = true;
            p.health = config::kMaxHealth;
            const Vec2Fixed* spawns =
                (p.team == Team::Blue) ? kBlueSpawnPoints : kRedSpawnPoints;
            p.position = spawns[p.spawn_index];
            OutboundEvent ev;
            ev.type = OutboundEventType::TcpBroadcast;
            ser_event(ev, protocol::MessageType::PlayerRespawned,
                      protocol::MsgPlayerRespawned{static_cast<uint8_t>(id), p.position,
                                         current_tick_});
            outbound_.push(ev);
        }
    }
}

// ---------------------------------------------------------------------------
// win check + publish
// ---------------------------------------------------------------------------

void Sim::win_check() {
    if (match_over_) return;

    const int limit_ticks =
        config::kMatchTimeLimitSec * config::kTickRateHz;
    if (score_[0] >= config::kScoreToWin || score_[1] >= config::kScoreToWin ||
        current_tick_ >= static_cast<uint32_t>(limit_ticks)) {
        match_over_ = true;

        Team winner = score_[0] >= score_[1] ? Team::Red : Team::Blue;
        OutboundEvent ev;
        ev.type = OutboundEventType::TcpBroadcast;
        ser_event(ev, protocol::MessageType::MatchEnd,
                  protocol::MsgMatchEnd{winner, score_[0], score_[1]});
        outbound_.push(ev);
    }
}

void Sim::publish() {
    WorldSnapshot snap;
    snap.tick = current_tick_;

    int n = 0;
    uint32_t ticks_elapsed = current_tick_;
    for (int id = 0; id < config::kMaxPlayers; ++id) {
        const Player& p = players_[id];
        if (!p.active) continue;
        PlayerState& ps = snap.players[n];
        ps.id = static_cast<uint8_t>(id);
        ps.team = p.team;
        ps.motion.position = p.position;
        ps.aim_angle = p.aim_angle;
        ps.health = static_cast<uint8_t>(p.health);
        ps.alive = p.alive;
        ps.carrying_flag = p.carrying_flag;
        player_acks_[id] = p.last_input_seq;
        ++n;
    }
    snap.player_count = static_cast<uint8_t>(n);

    snap.flag_carrier_red = flags_[0].carrier_id;
    snap.flag_carrier_blue = flags_[1].carrier_id;
    snap.flag_state_red = flags_[0].state;
    snap.flag_state_blue = flags_[1].state;
    snap.score_red = score_[0];
    snap.score_blue = score_[1];

    const int remaining =
        config::kMatchTimeLimitSec -
        static_cast<int>(ticks_elapsed / config::kTickRateHz);
    snap.seconds_remaining =
        static_cast<uint16_t>(remaining > 0 ? remaining : 0);

    snapshot_ = snap;

    // Serialize the body ONCE; broadcast patches last_input_seq (offset 0)
    // per recipient (README §5.5).
    OutboundEvent ev;
    ev.type = OutboundEventType::UdpSnapshot;
    ev.payload.resize(512);
    ByteWriter w(ev.payload.data(), ev.payload.size());
    protocol::encode_world_snapshot(w, snap);
    ev.payload.resize(w.size());
    for (int id = 0; id < config::kMaxPlayers; ++id) {
        ev.acks[id] = player_acks_[id];
    }
    outbound_.push(ev);
}

// ---------------------------------------------------------------------------
// the tick
// ---------------------------------------------------------------------------

void Sim::tick() {
    drain_inbound();
    pop_inputs();
    move_players();
    combat();
    flags_phase();
    win_check();
    publish();
    ++current_tick_;
}

} // namespace ctf

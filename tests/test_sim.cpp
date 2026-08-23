#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "game_config.h"
#include "map.h"
#include "protocol.h"
#include "queues.h"
#include "sim.h"

using namespace ctf;
using namespace ctf::protocol;

namespace {

constexpr int32_t kTileFp = config::kTileSizePx * config::kFixedScale;
constexpr int32_t kSpeed = config::kMoveSpeedFpPerTick;

Vec2Fixed tile(int32_t tx, int32_t ty) {
    return Vec2Fixed{tx * kTileFp, ty * kTileFp};
}

struct Fixture {
    InboundQueue in;
    OutboundQueue out;
    Sim sim{in, out};

    void join(uint8_t id, Team team, uint8_t spawn_index) {
        InboundCommand cmd;
        cmd.type = InboundCommandType::PlayerJoined;
        cmd.player_id = id;
        cmd.team = team;
        cmd.spawn_index = spawn_index;
        in.push(cmd);
    }

    void leave(uint8_t id) {
        InboundCommand cmd;
        cmd.type = InboundCommandType::PlayerLeft;
        cmd.player_id = id;
        in.push(cmd);
    }

    void input(uint8_t id, uint32_t seq, uint8_t buttons, uint16_t aim = 0) {
        InboundCommand cmd;
        cmd.type = InboundCommandType::PlayerInput;
        cmd.player_id = id;
        cmd.seq = seq;
        cmd.input.buttons = buttons;
        cmd.input.aim_angle = aim;
        in.push(cmd);
    }

    std::vector<OutboundEvent> drain_out() {
        std::vector<OutboundEvent> events;
        OutboundEvent ev;
        while (out.pop(ev)) events.push_back(ev);
        return events;
    }

    const PlayerState* snap_player(uint8_t id) {
        for (int i = 0; i < sim.snapshot().player_count; ++i) {
            if (sim.snapshot().players[i].id == id) {
                return &sim.snapshot().players[i];
            }
        }
        return nullptr;
    }

    template <typename Msg, typename DecodeFn>
    bool pop_event(const std::vector<OutboundEvent>& events, MessageType type,
                   DecodeFn decode, Msg& out) {
        for (const auto& ev : events) {
            if (ev.payload.empty()) continue;
            if (static_cast<MessageType>(ev.payload[0]) != type) continue;
            ByteReader r(ev.payload.data() + 1, ev.payload.size() - 1);
            if (decode(r, out)) return true;
        }
        return false;
    }

    const OutboundEvent* last_snapshot(const std::vector<OutboundEvent>& e) {
        const OutboundEvent* found = nullptr;
        for (const auto& ev : e) {
            if (ev.type == OutboundEventType::UdpSnapshot) found = &ev;
        }
        return found;
    }

    // Fire `shots` shots from `shooter` at `victim` along +x (or west with
    // `west`), spacing for cooldown. Assumes straight open line on row y.
    void shoot(uint8_t shooter, uint8_t victim, uint32_t& seq,
               bool west = false) {
        const uint16_t aim = west ? 32768 : 0;
        for (int i = 0; i < 3; ++i) {
            input(shooter, ++seq, kInputFire, aim);
            sim.tick();
            drain_out();
            for (int c = 0; c < config::kFireCooldownTicks; ++c) {
                sim.tick();
                drain_out();
            }
        }
    }
};

void two_player_setup(Fixture& f) {
    f.join(1, Team::Red, 0);   // red base pocket, tile (2,5)
    f.join(2, Team::Blue, 0);  // blue base pocket, tile (37,5)
    f.sim.tick();              // apply joins
    f.drain_out();
}

} // namespace

// ---------------------------------------------------------------------------
// joins / movement / input ring
// ---------------------------------------------------------------------------

TEST_CASE("joins place players at their spawns with full health") {
    Fixture f;
    two_player_setup(f);

    const WorldSnapshot& s = f.sim.snapshot();
    CHECK(s.player_count == 2);
    REQUIRE(f.snap_player(1) != nullptr);
    REQUIRE(f.snap_player(2) != nullptr);

    CHECK(f.snap_player(1)->motion.position.x == tile(2, 5).x);
    CHECK(f.snap_player(2)->motion.position.x == tile(37, 5).x);
    CHECK(f.snap_player(1)->health == config::kMaxHealth);
    CHECK(f.snap_player(1)->alive);

    CHECK(f.sim.flag_state(Team::Red) == FlagState::AtBase);
    CHECK(f.sim.flag_state(Team::Blue) == FlagState::AtBase);
}

TEST_CASE("one input per tick: ack advances exactly one per tick") {
    Fixture f;
    two_player_setup(f);

    f.input(1, 10, kInputRight);
    f.input(1, 11, kInputRight);
    f.input(1, 12, kInputRight);

    auto check_ack = [&](uint32_t expected) {
        auto events = f.drain_out();
        const OutboundEvent* snap_ev = f.last_snapshot(events);
        REQUIRE(snap_ev != nullptr);
        CHECK(snap_ev->acks[1] == expected);
    };

    f.sim.tick();
    check_ack(10);
    f.sim.tick();
    check_ack(11);
    f.sim.tick();
    check_ack(12);

    CHECK(f.snap_player(1)->motion.position.x == tile(2, 5).x + 3 * kSpeed);
}

TEST_CASE("empty ring buffer: zero movement, ack unchanged") {
    Fixture f;
    two_player_setup(f);

    const int32_t x0 = tile(2, 5).x;
    f.sim.tick();
    f.sim.tick();
    f.sim.tick();

    CHECK(f.snap_player(1)->motion.position.x == x0);

    auto events = f.drain_out();
    CHECK(f.last_snapshot(events)->acks[1] == 0);
}

TEST_CASE("full ring buffer drops oldest, still pops one per tick") {
    Fixture f;
    two_player_setup(f);

    for (uint32_t s = 1; s <= 10; ++s) {
        f.input(1, s, kInputDown);
    }
    f.sim.tick();
    {
        auto events = f.drain_out();
        CHECK(f.last_snapshot(events)->acks[1] == 3); // seqs 1-2 dropped
    }

    // Redundant duplicate seqs are ignored.
    f.input(1, 4, kInputDown);
    f.input(1, 4, kInputDown);
    f.sim.tick();
    {
        auto events = f.drain_out();
        CHECK(f.last_snapshot(events)->acks[1] == 4);
    }
}

TEST_CASE("leave removes the player from snapshots") {
    Fixture f;
    two_player_setup(f);
    f.leave(2);
    f.sim.tick();
    CHECK(f.sim.snapshot().player_count == 1);
    CHECK(f.snap_player(2) == nullptr);
}

// ---------------------------------------------------------------------------
// combat
// ---------------------------------------------------------------------------

TEST_CASE("combat: friendly fire does nothing") {
    Fixture f;
    f.join(1, Team::Red, 0);
    f.join(2, Team::Red, 1);
    f.join(3, Team::Blue, 0);
    f.sim.tick();
    f.drain_out();

    // Both reds aligned on open row 14; shooter aims +x through teammate.
    f.sim.debug_place(1, tile(6, 14));
    f.sim.debug_place(2, tile(12, 14));
    f.sim.tick();
    f.drain_out();

    uint32_t seq = 0;
    f.shoot(1, 2, seq);

    CHECK(f.snap_player(2)->health == config::kMaxHealth);
    CHECK(f.snap_player(2)->alive);
}

TEST_CASE("combat: wall occlusion blocks damage, tracer stops at wall") {
    Fixture f;
    two_player_setup(f);

    // Pillar cols 8-11 rows 11-13 between them on row 12.
    f.sim.debug_place(1, tile(6, 12));
    f.sim.debug_place(2, tile(13, 12));
    f.sim.tick();
    f.drain_out();

    uint32_t seq = 0;
    f.shoot(1, 2, seq);

    CHECK(f.snap_player(2)->health == config::kMaxHealth);
    CHECK(f.snap_player(2)->alive);

    // Re-fire once to catch the tracer event for inspection.
    f.input(1, ++seq, kInputFire, 0);
    f.sim.tick();
    auto events = f.drain_out();
    MsgShotFired shot;
    CHECK(f.pop_event(events, MessageType::ShotFired, decode_shot_fired,
                      shot));
    CHECK(shot.shooter_id == 1);
    CHECK(shot.hit_point.x <= tile(8, 0).x); // stopped at pillar west face
}

TEST_CASE("combat: three hits kill, respawn after 90 ticks at own base") {
    Fixture f;
    two_player_setup(f);

    // Open row 14, straight LOS.
    f.sim.debug_place(1, tile(4, 14));
    f.sim.debug_place(2, tile(20, 14));
    f.sim.tick();
    f.drain_out();

    std::vector<OutboundEvent> all_events;
    uint32_t seq = 0;
    for (int shot_i = 0; shot_i < 3; ++shot_i) {
        f.input(1, ++seq, kInputFire, 0);
        f.sim.tick();
        auto evs = f.drain_out();
        all_events.insert(all_events.end(), evs.begin(), evs.end());
        if (shot_i == 0) {
            CHECK(f.snap_player(2)->health == config::kMaxHealth - 34);
        }
        if (shot_i == 1) {
            CHECK(f.snap_player(2)->health == config::kMaxHealth - 68);
        }
        for (int c = 0; c < config::kFireCooldownTicks; ++c) {
            f.sim.tick();
            auto more = f.drain_out();
            all_events.insert(all_events.end(), more.begin(), more.end());
        }
    }
    CHECK_FALSE(f.snap_player(2)->alive);

    const auto& events = all_events;
    MsgPlayerKilled kill;
    CHECK(f.pop_event(events, MessageType::PlayerKilled, decode_player_killed,
                      kill));
    CHECK(kill.victim_id == 2);
    CHECK(kill.killer_id == 1);

    // Dead: inputs ignored, position frozen.
    const int32_t dead_x = f.snap_player(2)->motion.position.x;
    for (uint32_t i = 0; i < 30; ++i) {
        f.input(2, 500 + i, kInputRight);
        f.sim.tick();
        f.drain_out();
    }
    CHECK(f.snap_player(2)->motion.position.x == dead_x);

    // Respawn accounting: timer set to 90 on the death tick and
    // decremented by that same tick's flags phase; 10 cooldown + 30 frozen
    // ticks have run -> 48 more land one tick short of the 89 needed.
    for (int i = 0; i < 48; ++i) {
        f.sim.tick();
        f.drain_out();
    }
    CHECK_FALSE(f.snap_player(2)->alive); // one tick before expiry
    f.sim.tick();
    {
        auto evs = f.drain_out();
        all_events.insert(all_events.end(), evs.begin(), evs.end());
    }
    CHECK(f.snap_player(2)->alive);
    CHECK(f.snap_player(2)->health == config::kMaxHealth);
    CHECK(f.snap_player(2)->motion.position.x > tile(35, 0).x);

    const auto& respawn_events = all_events;
    MsgPlayerRespawned res;
    CHECK(f.pop_event(respawn_events, MessageType::PlayerRespawned,
                      decode_player_respawned, res));
    CHECK(res.player_id == 2);
}

// ---------------------------------------------------------------------------
// flags
// ---------------------------------------------------------------------------

TEST_CASE("flag: enemy pickup on touch; owner standing on own flag is safe")
{
    Fixture f;
    two_player_setup(f);

    std::vector<OutboundEvent> all;
    // Red onto the blue flag spot; blue parked just south.
    f.sim.debug_place(1, tile(37, 12));
    f.sim.debug_place(2, tile(37, 14));
    f.sim.tick();
    {
        auto evs = f.drain_out();
        all.insert(all.end(), evs.begin(), evs.end());
    }

    // Blue steps onto his own flag: no self-pickup.
    f.sim.debug_place(2, tile(37, 12));
    f.sim.tick();
    {
        auto evs = f.drain_out();
        all.insert(all.end(), evs.begin(), evs.end());
    }

    CHECK(f.sim.flag_state(Team::Blue) == FlagState::Carried);
    CHECK(f.sim.flag_carrier(Team::Blue) == 1);
    CHECK(f.sim.flag_state(Team::Red) == FlagState::AtBase);

    const auto& events = all;
    MsgFlagPickedUp pick;
    CHECK(f.pop_event(events, MessageType::FlagPickedUp,
                      decode_flag_picked_up, pick));
    CHECK(pick.flag_team == Team::Blue);
    CHECK(pick.player_id == 1);
}

TEST_CASE("flag: capture gated on own flag being at base") {
    Fixture f;
    two_player_setup(f);

    // Blue steals the red flag off its base (pickup only -- capture
    // happens at the THIEF'S own base, so nothing scores here).
    f.sim.debug_place(2, tile(2, 12));
    f.sim.tick();
    f.drain_out();
    REQUIRE(f.sim.flag_carrier(Team::Red) == 2);

    // Red takes the blue flag.
    f.sim.debug_place(1, tile(37, 12));
    f.sim.tick();
    f.drain_out();
    REQUIRE(f.sim.flag_carrier(Team::Blue) == 1);

    // Both flags out. Red stands on HIS OWN base holding the blue flag,
    // but his own flag is away -> no capture, no score.
    f.sim.debug_place(1, tile(2, 12));
    f.sim.tick();
    f.sim.tick();
    f.drain_out();

    CHECK(f.sim.snapshot().score_red == 0);
    CHECK(f.sim.flag_state(Team::Blue) == FlagState::Carried);
    CHECK(f.sim.flag_state(Team::Red) == FlagState::Carried);

    // Kill the blue thief: red flag drops at the death spot...
    f.join(3, Team::Red, 2);
    f.sim.tick();
    f.drain_out();
    f.sim.debug_place(2, tile(20, 14));
    f.sim.debug_place(3, tile(16, 14));

    uint32_t seq = 100;
    f.shoot(3, 2, seq);
    REQUIRE_FALSE(f.snap_player(2)->alive);
    CHECK(f.sim.flag_state(Team::Red) == FlagState::Dropped);

    // ...and auto-returns home after 450 untouched ticks.
    std::vector<OutboundEvent> all;
    for (int i = 0; i < config::kFlagAutoReturnTicks + 5; ++i) {
        if (i == config::kRespawnDelayTicks + 1) {
            f.sim.debug_place(2, tile(37, 22)); // respawned blue parked away
        }
        f.sim.tick();
        auto evs = f.drain_out();
        all.insert(all.end(), evs.begin(), evs.end());
    }
    CHECK(f.sim.flag_state(Team::Red) == FlagState::AtBase);

    // Red is STILL standing on his own base still carrying the blue flag:
    // with his own flag back home the gate opens -> capture fires without
    // any further placement.
    CHECK(f.sim.snapshot().score_red == 1);
    CHECK(f.sim.flag_state(Team::Blue) == FlagState::AtBase); // reset

    MsgFlagCaptured cap;
    CHECK(f.pop_event(all, MessageType::FlagCaptured, decode_flag_captured,
                      cap));
    CHECK(cap.flag_team == Team::Blue);
    CHECK(cap.player_id == 1);
}

TEST_CASE("flag: carrier death drops it at the death spot") {
    Fixture f;
    two_player_setup(f);

    f.join(3, Team::Blue, 1);
    f.sim.tick();
    f.drain_out();

    // Red takes the blue flag from its base...
    f.sim.debug_place(1, tile(37, 12));
    f.sim.tick();
    f.drain_out();
    REQUIRE(f.sim.flag_carrier(Team::Blue) == 1);

    // ...then dies mid-map to a westward shot from blue #3.
    const Vec2Fixed death_spot = tile(20, 22);
    f.sim.debug_place(1, death_spot);
    f.sim.debug_place(3, tile(26, 22));
    f.sim.debug_place(2, tile(37, 22));
    f.sim.tick();
    f.drain_out();

    uint32_t seq = 300;
    f.shoot(3, 1, seq, /*west=*/true);

    REQUIRE_FALSE(f.snap_player(1)->alive);
    CHECK(f.sim.flag_state(Team::Blue) == FlagState::Dropped);
    CHECK(f.sim.flag_carrier(Team::Blue) == 0xFF);
}

TEST_CASE("flag: own dropped flag returns instantly on owner touch") {
    Fixture f;
    two_player_setup(f);

    // Blue steals the red flag...
    f.sim.debug_place(2, tile(2, 12));
    f.sim.tick();
    f.drain_out();
    REQUIRE(f.sim.flag_carrier(Team::Red) == 2);

    // ...and dies carrying it mid-map to red shooter #3.
    f.join(3, Team::Red, 2);
    f.sim.tick();
    f.drain_out();
    f.sim.debug_place(2, tile(20, 14));
    f.sim.debug_place(3, tile(16, 14));

    uint32_t seq = 400;
    f.shoot(3, 2, seq);
    REQUIRE(f.sim.flag_state(Team::Red) == FlagState::Dropped);
    CHECK(f.sim.flag_carrier(Team::Red) == 0xFF);

    // Park the respawned blue far away so only the owner touches it.
    for (int i = 0; i < config::kRespawnDelayTicks + 2; ++i) {
        f.sim.tick();
        f.drain_out();
        if (i == config::kRespawnDelayTicks) {
            f.sim.debug_place(2, tile(37, 22));
        }
    }

    // Owner (red #1) steps onto his own team's dropped flag: instant
    // return to base -- no 450-tick wait.
    f.sim.debug_place(1, tile(20, 14));
    f.sim.tick();
    auto events = f.drain_out();

    CHECK(f.sim.flag_state(Team::Red) == FlagState::AtBase);

    MsgFlagReturned ret;
    CHECK(f.pop_event(events, MessageType::FlagReturned,
                      decode_flag_returned, ret));
    CHECK(ret.flag_team == Team::Red);
    CHECK(ret.player_id == 1);
}

TEST_CASE("flag: auto-returns to base after 450 ticks untouched") {
    Fixture f;
    two_player_setup(f);

    // Blue steals the red flag and dies carrying it mid-map.
    f.sim.debug_place(2, tile(2, 12));
    f.sim.tick();
    f.drain_out();
    REQUIRE(f.sim.flag_carrier(Team::Red) == 2);

    f.join(3, Team::Red, 2);
    f.sim.tick();
    f.drain_out();
    f.sim.debug_place(2, tile(20, 14));
    f.sim.debug_place(3, tile(16, 14));

    uint32_t seq = 500;
    f.shoot(3, 2, seq);
    REQUIRE(f.sim.flag_state(Team::Red) == FlagState::Dropped);

    // Nobody touches it. Setup consumed ~35 ticks, so 400 loop ticks stay
    // safely under the 450-since-drop threshold.
    std::vector<OutboundEvent> all;
    for (int i = 0; i < 400; ++i) {
        if (i == config::kRespawnDelayTicks) {
            f.sim.debug_place(2, tile(37, 22)); // keep respawned blue away
        }
        f.sim.tick();
        auto evs = f.drain_out();
        all.insert(all.end(), evs.begin(), evs.end());
    }
    CHECK(f.sim.flag_state(Team::Red) == FlagState::Dropped);

    // Run past the 450-tick mark: auto-return fires on its own.
    std::vector<OutboundEvent> tail;
    for (int i = 0; i < 60; ++i) {
        f.sim.tick();
        auto evs = f.drain_out();
        tail.insert(tail.end(), evs.begin(), evs.end());
    }
    CHECK(f.sim.flag_state(Team::Red) == FlagState::AtBase);
    all.insert(all.end(), tail.begin(), tail.end());

    MsgFlagReturned ret;
    CHECK(f.pop_event(all, MessageType::FlagReturned, decode_flag_returned,
                      ret));
    CHECK(ret.flag_team == Team::Red);
}

// ---------------------------------------------------------------------------
// win condition + freeze
// ---------------------------------------------------------------------------

TEST_CASE("three captures fire MATCH_END and freeze gameplay") {
    Fixture f;
    two_player_setup(f);

    // Each capture: grab the blue flag off its base, carry it home onto
    // the red base (own flag never moves, gate always open).
    std::vector<OutboundEvent> all;
    auto pump = [&] {
        auto evs = f.drain_out();
        all.insert(all.end(), evs.begin(), evs.end());
    };
    for (int round = 0; round < 3; ++round) {
        REQUIRE_FALSE(f.sim.match_over());
        f.sim.debug_place(1, tile(37, 12)); // steal
        f.sim.tick();
        pump();
        f.sim.debug_place(1, tile(2, 12)); // home: capture
        f.sim.tick();
        pump();
        CHECK(f.sim.snapshot().score_red == round + 1);
        f.sim.debug_place(1, tile(30, 22)); // step off the scoring spot
        f.sim.tick();
        pump();
    }

    CHECK(f.sim.match_over());

    MsgMatchEnd end{};
    bool saw_end = false;
    const auto& events = all;
    for (const auto& ev : events) {
        if (!ev.payload.empty() &&
            static_cast<MessageType>(ev.payload[0]) ==
                protocol::MessageType::MatchEnd) {
            ByteReader r(ev.payload.data() + 1, ev.payload.size() - 1);
            CHECK(decode_match_end(r, end));
            saw_end = true;
        }
    }
    REQUIRE(saw_end);
    CHECK(end.winning_team == Team::Red);
    CHECK(end.score_red == 3);

    // Frozen: further inputs change nothing.
    const int32_t frozen_x = f.snap_player(1)->motion.position.x;
    for (uint32_t i = 0; i < 10; ++i) {
        f.input(1, 700 + i, kInputRight);
        f.sim.tick();
        f.drain_out();
    }
    CHECK(f.snap_player(1)->motion.position.x == frozen_x);
}

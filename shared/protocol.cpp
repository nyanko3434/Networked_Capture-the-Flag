#include "protocol.h"

// README §5.2-§5.5: every message gets an explicit encode/decode pair built
// on ByteWriter/ByteReader -- never memcpy a struct onto the wire. Every
// decode path checks reader.ok() after reading and returns false (dropped by
// the caller) rather than trusting a malformed packet; the UDP socket
// accepts datagrams from anyone on the LAN (README §5.2), so this must hold
// for every message, not just the ones that look network-facing.
//
// TCP frame construction/reassembly ([u16 len][u8 type][payload]) is NOT in
// this file -- shared/net_util.h's recv_framed (implementation_guide.md
// §1.7) owns that, operating on the accumulation buffer directly. This file
// only encodes/decodes the payload bytes that go inside a frame (or inside a
// UDP datagram, after the 8-byte UdpHeader). Flagging this split explicitly
// since implementation_guide.md §1.6's checklist describes the reassembly
// loop as if it were part of this file -- it isn't, in this codebase's
// current header layout, and moving it here would fight net_util.h's
// existing signature rather than match it.

namespace ctf::protocol {

namespace {

// Wire position conversion (README §5.1): internal is int32 in 1/256 px,
// wire is int16 in 1/16 px. Both directions shift by the same fixed amount
// so the conversion is exact and lossless within wire range (+/-2048 px).
int16_t to_wire_pos(int32_t internal) {
    return static_cast<int16_t>(internal >> (config::kFixedShift - config::kWireFixedShift));
}

int32_t from_wire_pos(int16_t wire) {
    // Left-shifting a negative signed value is undefined behavior (caught by
    // UBSan) -- multiply instead, which is equivalent for this magnitude
    // (kWireFixedScale == 1 << (kFixedShift - kWireFixedShift)) and
    // well-defined for negative operands.
    return static_cast<int32_t>(wire) * (config::kFixedScale / config::kWireFixedScale);
}

void write_vec2(ByteWriter& w, const Vec2Fixed& v) {
    w.i16(to_wire_pos(v.x));
    w.i16(to_wire_pos(v.y));
}

Vec2Fixed read_vec2(ByteReader& r) {
    Vec2Fixed v;
    v.x = from_wire_pos(r.i16());
    v.y = from_wire_pos(r.i16());
    return v;
}

// Per-player WORLD_SNAPSHOT flags bitfield (README §5.5's "flags (alive |
// carrying | team | firing)" -- bit layout is this file's own decision since
// neither README nor the guide pins one down).
constexpr uint8_t kFlagBitAlive = 1u << 0;
constexpr uint8_t kFlagBitCarrying = 1u << 1;
constexpr uint8_t kFlagBitTeamBlue = 1u << 2;
constexpr uint8_t kFlagBitFiring = 1u << 3;

} // namespace

void encode_udp_header(ByteWriter& w, const UdpHeader& hdr) {
    w.u16(hdr.magic);
    w.u8(hdr.version);
    w.u8(hdr.type);
    w.u32(hdr.tick);
}

bool decode_udp_header(ByteReader& r, UdpHeader& out) {
    out.magic = r.u16();
    out.version = r.u8();
    out.type = r.u8();
    out.tick = r.u32();
    if (!r.ok()) {
        return false;
    }
    // Reject anything else immediately (README §5.3) -- wrong magic or
    // mismatched version (a stale binary) is dropped, not trusted.
    if (out.magic != kMagic || out.version != kProtocolVersion) {
        return false;
    }
    return true;
}

void encode_join_lobby(ByteWriter& w, const MsgJoinLobby& msg) {
    for (char c : msg.name) {
        w.u8(static_cast<uint8_t>(c));
    }
}

bool decode_join_lobby(ByteReader& r, MsgJoinLobby& out) {
    for (char& c : out.name) {
        c = static_cast<char>(r.u8());
    }
    return r.ok();
}

void encode_join_accept(ByteWriter& w, const MsgJoinAccept& msg) {
    w.u8(msg.player_id);
    w.u32(msg.session_token);
    w.u16(msg.udp_port);
}

bool decode_join_accept(ByteReader& r, MsgJoinAccept& out) {
    out.player_id = r.u8();
    out.session_token = r.u32();
    out.udp_port = r.u16();
    return r.ok();
}

void encode_join_reject(ByteWriter& w, const MsgJoinReject& msg) {
    w.u8(static_cast<uint8_t>(msg.reason));
}

bool decode_join_reject(ByteReader& r, MsgJoinReject& out) {
    uint8_t reason = r.u8();
    if (!r.ok()) {
        return false;
    }
    if (reason > static_cast<uint8_t>(JoinRejectReason::BadVersion)) {
        return false;
    }
    out.reason = static_cast<JoinRejectReason>(reason);
    return true;
}

void encode_lobby_state(ByteWriter& w, const MsgLobbyState& msg) {
    w.u8(msg.player_count);
    for (uint8_t id : msg.ids) {
        w.u8(id);
    }
    for (const auto& name : msg.names) {
        for (char c : name) {
            w.u8(static_cast<uint8_t>(c));
        }
    }
    w.u8(msg.host_id);
}

bool decode_lobby_state(ByteReader& r, MsgLobbyState& out) {
    out.player_count = r.u8();
    for (uint8_t& id : out.ids) {
        id = r.u8();
    }
    for (auto& name : out.names) {
        for (char& c : name) {
            c = static_cast<char>(r.u8());
        }
    }
    out.host_id = r.u8();
    if (!r.ok()) {
        return false;
    }
    if (out.player_count > config::kMaxPlayers) {
        return false;
    }
    return true;
}

void encode_start_request(ByteWriter&, const MsgStartRequest&) {}

bool decode_start_request(ByteReader&, MsgStartRequest&) { return true; }

void encode_game_start(ByteWriter& w, const MsgGameStart& msg) {
    w.u8(msg.player_count);
    for (uint8_t id : msg.ids) {
        w.u8(id);
    }
    for (Team t : msg.teams) {
        w.u8(static_cast<uint8_t>(t));
    }
    for (const Vec2Fixed& pos : msg.spawn_points) {
        write_vec2(w, pos);
    }
    w.u32(msg.start_tick);
}

bool decode_game_start(ByteReader& r, MsgGameStart& out) {
    out.player_count = r.u8();
    for (uint8_t& id : out.ids) {
        id = r.u8();
    }
    for (Team& t : out.teams) {
        uint8_t v = r.u8();
        t = (v <= static_cast<uint8_t>(Team::Blue)) ? static_cast<Team>(v) : Team::Red;
    }
    for (Vec2Fixed& pos : out.spawn_points) {
        pos = read_vec2(r);
    }
    out.start_tick = r.u32();
    if (!r.ok()) {
        return false;
    }
    if (out.player_count > config::kMaxPlayers) {
        return false;
    }
    return true;
}

void encode_udp_hello(ByteWriter& w, const MsgUdpHello& msg) {
    w.u8(msg.player_id);
    w.u32(msg.session_token);
}

bool decode_udp_hello(ByteReader& r, MsgUdpHello& out) {
    out.player_id = r.u8();
    out.session_token = r.u32();
    return r.ok();
}

void encode_player_input(ByteWriter& w, const MsgPlayerInput& msg) {
    w.u8(msg.player_id);
    w.u32(msg.session_token);
    w.u32(msg.base_seq);
    w.u8(msg.count);
    // Always all kInputRedundancy slots on the wire (README §5.4 -- "3x
    // input" is a fixed part of the payload; `count` tells the reader how
    // many of them are meaningful vs. leftover zeroed entries).
    for (const InputCmd& cmd : msg.inputs) {
        w.u8(cmd.buttons);
        w.u16(cmd.aim_angle);
    }
}

bool decode_player_input(ByteReader& r, MsgPlayerInput& out) {
    out.player_id = r.u8();
    out.session_token = r.u32();
    out.base_seq = r.u32();
    out.count = r.u8();
    for (InputCmd& cmd : out.inputs) {
        cmd.buttons = r.u8();
        cmd.aim_angle = r.u16();
    }
    if (!r.ok()) {
        return false;
    }
    if (out.count > config::kInputRedundancy) {
        return false;
    }
    return true;
}

void encode_world_snapshot(ByteWriter& w, const WorldSnapshot& msg) {
    // Body only -- `tick` is carried by the UDP header (README §5.3), not
    // duplicated into the body. `last_input_seq` is written here as whatever
    // value msg carries; the per-recipient patch (README §5.5) overwrites
    // these first 4 bytes of the encoded buffer afterward, at a fixed offset
    // of 0 since it's the first field written below.
    w.u32(msg.last_input_seq);
    w.u8(msg.player_count);
    w.u8(msg.flag_carrier_red);
    w.u8(msg.flag_carrier_blue);
    w.u8(static_cast<uint8_t>(msg.flag_state_red));
    w.u8(static_cast<uint8_t>(msg.flag_state_blue));
    w.u8(msg.score_red);
    w.u8(msg.score_blue);
    w.u16(msg.seconds_remaining);

    // Variable-length: only `player_count` records, not always kMaxPlayers
    // (README §5.5 -- "Ten players ~ 128 bytes", implying the packet scales
    // with the roster, not a fixed 10 regardless of how many are connected).
    for (uint8_t i = 0; i < msg.player_count; ++i) {
        const PlayerState& p = msg.players[i];
        w.u8(p.id);
        write_vec2(w, p.motion.position);
        w.u16(p.aim_angle);
        w.u8(p.health);
        uint8_t flags = 0;
        if (p.alive) flags |= kFlagBitAlive;
        if (p.carrying_flag) flags |= kFlagBitCarrying;
        if (p.team == Team::Blue) flags |= kFlagBitTeamBlue;
        if (p.firing) flags |= kFlagBitFiring;
        w.u8(flags);
    }
}

bool decode_world_snapshot(ByteReader& r, WorldSnapshot& out) {
    // `out.tick` is NOT set here -- the caller (net_client.cpp) fills it in
    // from the already-decoded UdpHeader.tick, since the body doesn't carry
    // its own copy (see encode_world_snapshot above).
    out.last_input_seq = r.u32();
    out.player_count = r.u8();
    out.flag_carrier_red = r.u8();
    out.flag_carrier_blue = r.u8();
    uint8_t flag_state_red_raw = r.u8();
    uint8_t flag_state_blue_raw = r.u8();
    out.score_red = r.u8();
    out.score_blue = r.u8();
    out.seconds_remaining = r.u16();

    if (!r.ok()) {
        return false;
    }
    if (out.player_count > config::kMaxPlayers) {
        return false;
    }
    if (flag_state_red_raw > static_cast<uint8_t>(FlagState::Dropped) ||
        flag_state_blue_raw > static_cast<uint8_t>(FlagState::Dropped)) {
        return false;
    }
    out.flag_state_red = static_cast<FlagState>(flag_state_red_raw);
    out.flag_state_blue = static_cast<FlagState>(flag_state_blue_raw);

    for (uint8_t i = 0; i < out.player_count; ++i) {
        PlayerState& p = out.players[i];
        p.id = r.u8();
        p.motion.position = read_vec2(r);
        p.aim_angle = r.u16();
        p.health = r.u8();
        uint8_t flags = r.u8();
        p.alive = (flags & kFlagBitAlive) != 0;
        p.carrying_flag = (flags & kFlagBitCarrying) != 0;
        p.team = (flags & kFlagBitTeamBlue) != 0 ? Team::Blue : Team::Red;
        p.firing = (flags & kFlagBitFiring) != 0;
    }
    return r.ok();
}

void encode_shot_fired(ByteWriter& w, const MsgShotFired& msg) {
    w.u8(msg.shooter_id);
    write_vec2(w, msg.origin);
    w.u16(msg.aim_angle);
    write_vec2(w, msg.hit_point);
}

bool decode_shot_fired(ByteReader& r, MsgShotFired& out) {
    out.shooter_id = r.u8();
    out.origin = read_vec2(r);
    out.aim_angle = r.u16();
    out.hit_point = read_vec2(r);
    return r.ok();
}

void encode_player_killed(ByteWriter& w, const MsgPlayerKilled& msg) {
    w.u8(msg.victim_id);
    w.u8(msg.killer_id);
    w.u32(msg.tick);
}

bool decode_player_killed(ByteReader& r, MsgPlayerKilled& out) {
    out.victim_id = r.u8();
    out.killer_id = r.u8();
    out.tick = r.u32();
    return r.ok();
}

void encode_player_respawned(ByteWriter& w, const MsgPlayerRespawned& msg) {
    w.u8(msg.player_id);
    write_vec2(w, msg.position);
    w.u32(msg.tick);
}

bool decode_player_respawned(ByteReader& r, MsgPlayerRespawned& out) {
    out.player_id = r.u8();
    out.position = read_vec2(r);
    out.tick = r.u32();
    return r.ok();
}

namespace {
bool decode_team(uint8_t raw, Team& out) {
    if (raw > static_cast<uint8_t>(Team::Blue)) {
        return false;
    }
    out = static_cast<Team>(raw);
    return true;
}
} // namespace

void encode_flag_picked_up(ByteWriter& w, const MsgFlagPickedUp& msg) {
    w.u8(static_cast<uint8_t>(msg.flag_team));
    w.u8(msg.player_id);
    w.u32(msg.tick);
}

bool decode_flag_picked_up(ByteReader& r, MsgFlagPickedUp& out) {
    uint8_t team_raw = r.u8();
    out.player_id = r.u8();
    out.tick = r.u32();
    if (!r.ok()) {
        return false;
    }
    return decode_team(team_raw, out.flag_team);
}

void encode_flag_dropped(ByteWriter& w, const MsgFlagDropped& msg) {
    w.u8(static_cast<uint8_t>(msg.flag_team));
    write_vec2(w, msg.position);
    w.u32(msg.tick);
}

bool decode_flag_dropped(ByteReader& r, MsgFlagDropped& out) {
    uint8_t team_raw = r.u8();
    out.position = read_vec2(r);
    out.tick = r.u32();
    if (!r.ok()) {
        return false;
    }
    return decode_team(team_raw, out.flag_team);
}

void encode_flag_returned(ByteWriter& w, const MsgFlagReturned& msg) {
    w.u8(static_cast<uint8_t>(msg.flag_team));
    w.u8(msg.player_id);
    w.u32(msg.tick);
}

bool decode_flag_returned(ByteReader& r, MsgFlagReturned& out) {
    uint8_t team_raw = r.u8();
    out.player_id = r.u8();
    out.tick = r.u32();
    if (!r.ok()) {
        return false;
    }
    return decode_team(team_raw, out.flag_team);
}

void encode_flag_captured(ByteWriter& w, const MsgFlagCaptured& msg) {
    w.u8(static_cast<uint8_t>(msg.flag_team));
    w.u8(msg.player_id);
    w.u32(msg.tick);
}

bool decode_flag_captured(ByteReader& r, MsgFlagCaptured& out) {
    uint8_t team_raw = r.u8();
    out.player_id = r.u8();
    out.tick = r.u32();
    if (!r.ok()) {
        return false;
    }
    return decode_team(team_raw, out.flag_team);
}

void encode_match_end(ByteWriter& w, const MsgMatchEnd& msg) {
    w.u8(static_cast<uint8_t>(msg.winning_team));
    w.u8(msg.score_red);
    w.u8(msg.score_blue);
}

bool decode_match_end(ByteReader& r, MsgMatchEnd& out) {
    uint8_t team_raw = r.u8();
    out.score_red = r.u8();
    out.score_blue = r.u8();
    if (!r.ok()) {
        return false;
    }
    return decode_team(team_raw, out.winning_team);
}

void encode_heartbeat(ByteWriter&, const MsgHeartbeat&) {}

bool decode_heartbeat(ByteReader&, MsgHeartbeat&) { return true; }

} // namespace ctf::protocol

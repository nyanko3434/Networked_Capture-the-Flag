#include "protocol.h"

// Explicit encode/decode pairs for every message in README §5.4, built on
// ByteWriter/ByteReader — never memcpy a struct onto the wire (README §5.2).
//
// Every decode path validates reader.ok() and structural sanity (counts,
// magic, version) and returns false on any problem so callers drop the
// packet. A malformed datagram must never crash or hang the server.

namespace ctf::protocol {

namespace {

// Internal positions are int32 in 1/256 px; wire is i16 in 1/16 px
// (README §5.1). Shift, never float.
void write_vec2(ByteWriter& w, const Vec2Fixed& v) {
    w.i16(static_cast<int16_t>(v.x >> config::kWireFixedShift));
    w.i16(static_cast<int16_t>(v.y >> config::kWireFixedShift));
}

Vec2Fixed read_vec2(ByteReader& r) {
    const int16_t wx = r.i16();
    const int16_t wy = r.i16();
    Vec2Fixed v;
    v.x = static_cast<int32_t>(wx) << config::kWireFixedShift;
    v.y = static_cast<int32_t>(wy) << config::kWireFixedShift;
    return v;
}

constexpr size_t kNameBytes = 16;

void write_name(ByteWriter& w, const char* name) {
    for (size_t i = 0; i < kNameBytes; ++i) {
        w.u8(static_cast<uint8_t>(name[i]));
    }
}

void read_name(ByteReader& r, char* name) {
    for (size_t i = 0; i < kNameBytes; ++i) {
        name[i] = static_cast<char>(r.u8());
    }
}

// Player flags byte (README §5.5): alive | carrying | team | firing.
constexpr uint8_t kFlagAlive = 1u << 0;
constexpr uint8_t kFlagCarrying = 1u << 1;
constexpr uint8_t kFlagTeamBlue = 1u << 2;
constexpr uint8_t kFlagFiring = 1u << 3;

uint8_t pack_player_flags(const PlayerState& p) {
    uint8_t f = 0;
    if (p.alive) f |= kFlagAlive;
    if (p.carrying_flag) f |= kFlagCarrying;
    if (p.team == Team::Blue) f |= kFlagTeamBlue;
    if (p.firing) f |= kFlagFiring;
    return f;
}

void unpack_player_flags(uint8_t f, PlayerState& p) {
    p.alive = (f & kFlagAlive) != 0;
    p.carrying_flag = (f & kFlagCarrying) != 0;
    p.team = (f & kFlagTeamBlue) ? Team::Blue : Team::Red;
    p.firing = (f & kFlagFiring) != 0;
}

// Team/FlagState/JoinRejectReason are wire bytes cast to a fixed-underlying-
// type enum: an out-of-range byte doesn't crash, it just produces a Team
// that's neither Red nor Blue and silently corrupts state wherever the
// caller does `team == Team::Blue`. Validate against the real enumerators
// here so a malformed packet is dropped instead, matching this file's own
// "never trust a malformed packet" contract.
bool decode_team(uint8_t raw, Team& out) {
    if (raw > static_cast<uint8_t>(Team::Blue)) {
        return false;
    }
    out = static_cast<Team>(raw);
    return true;
}

bool decode_flag_state(uint8_t raw, FlagState& out) {
    if (raw > static_cast<uint8_t>(FlagState::Dropped)) {
        return false;
    }
    out = static_cast<FlagState>(raw);
    return true;
}

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
    if (out.magic != kMagic || out.version != kProtocolVersion) {
        return false;
    }
    return true;
}

void encode_join_lobby(ByteWriter& w, const MsgJoinLobby& msg) {
    write_name(w, msg.name);
}

bool decode_join_lobby(ByteReader& r, MsgJoinLobby& out) {
    read_name(r, out.name);
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
    const uint8_t raw = r.u8();
    if (!r.ok()) {
        return false;
    }
    if (raw > static_cast<uint8_t>(JoinRejectReason::BadVersion)) {
        return false;
    }
    out.reason = static_cast<JoinRejectReason>(raw);
    return true;
}

void encode_lobby_state(ByteWriter& w, const MsgLobbyState& msg) {
    w.u8(msg.player_count);
    for (int i = 0; i < msg.player_count && i < config::kMaxPlayers; ++i) {
        w.u8(msg.ids[i]);
        write_name(w, msg.names[i]);
    }
    w.u8(msg.host_id);
}

bool decode_lobby_state(ByteReader& r, MsgLobbyState& out) {
    out.player_count = r.u8();
    if (out.player_count > config::kMaxPlayers) {
        return false;
    }
    for (int i = 0; i < out.player_count; ++i) {
        out.ids[i] = r.u8();
        read_name(r, out.names[i]);
    }
    out.host_id = r.u8();
    return r.ok();
}

void encode_start_request(ByteWriter&, const MsgStartRequest&) {}

bool decode_start_request(ByteReader& r, MsgStartRequest&) { return r.ok(); }

void encode_game_start(ByteWriter& w, const MsgGameStart& msg) {
    w.u8(msg.player_count);
    w.u32(msg.start_tick);
    for (int i = 0; i < msg.player_count && i < config::kMaxPlayers; ++i) {
        w.u8(msg.ids[i]);
        w.u8(static_cast<uint8_t>(msg.teams[i]));
        write_vec2(w, msg.spawn_points[i]);
    }
}

bool decode_game_start(ByteReader& r, MsgGameStart& out) {
    out.player_count = r.u8();
    out.start_tick = r.u32();
    if (!r.ok() || out.player_count > config::kMaxPlayers) {
        return false;
    }
    for (int i = 0; i < out.player_count; ++i) {
        out.ids[i] = r.u8();
        const uint8_t team_raw = r.u8();
        if (!decode_team(team_raw, out.teams[i])) {
            return false;
        }
        out.spawn_points[i] = read_vec2(r);
    }
    return r.ok();
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
    const int n = msg.count < config::kInputRedundancy ? msg.count
                                                       : config::kInputRedundancy;
    for (int i = 0; i < n; ++i) {
        w.u8(msg.inputs[i].buttons);
        w.u16(msg.inputs[i].aim_angle);
    }
}

bool decode_player_input(ByteReader& r, MsgPlayerInput& out) {
    out.player_id = r.u8();
    out.session_token = r.u32();
    out.base_seq = r.u32();
    out.count = r.u8();
    if (out.count > config::kInputRedundancy) {
        return false;
    }
    for (int i = 0; i < out.count; ++i) {
        out.inputs[i].buttons = r.u8();
        out.inputs[i].aim_angle = r.u16();
    }
    return r.ok();
}

void encode_world_snapshot(ByteWriter& w, const WorldSnapshot& snap) {
    // Payload starts at last_input_seq (README §5.5); tick travels in the
    // UDP header, but it is repeated here so TCP-side consumers and tests
    // stay self-describing.
    w.u32(snap.last_input_seq);
    w.u32(snap.tick);
    w.u8(snap.player_count);
    w.u8(snap.flag_carrier_red);
    w.u8(snap.flag_carrier_blue);
    w.u8(static_cast<uint8_t>(snap.flag_state_red));
    w.u8(static_cast<uint8_t>(snap.flag_state_blue));
    w.u8(snap.score_red);
    w.u8(snap.score_blue);
    w.u16(snap.seconds_remaining);

    const int n =
        snap.player_count < config::kMaxPlayers ? snap.player_count : config::kMaxPlayers;
    for (int i = 0; i < n; ++i) {
        const PlayerState& p = snap.players[i];
        w.u8(p.id);
        write_vec2(w, p.motion.position);
        w.u16(p.aim_angle);
        w.u8(p.health);
        w.u8(pack_player_flags(p));
    }
}

bool decode_world_snapshot(ByteReader& r, WorldSnapshot& out) {
    out.last_input_seq = r.u32();
    out.tick = r.u32();
    out.player_count = r.u8();
    out.flag_carrier_red = r.u8();
    out.flag_carrier_blue = r.u8();
    const uint8_t flag_state_red_raw = r.u8();
    const uint8_t flag_state_blue_raw = r.u8();
    out.score_red = r.u8();
    out.score_blue = r.u8();
    out.seconds_remaining = r.u16();
    if (!r.ok() || out.player_count > config::kMaxPlayers) {
        return false;
    }
    if (!decode_flag_state(flag_state_red_raw, out.flag_state_red) ||
        !decode_flag_state(flag_state_blue_raw, out.flag_state_blue)) {
        return false;
    }

    for (int i = 0; i < out.player_count; ++i) {
        PlayerState& p = out.players[i];
        p.id = r.u8();
        p.motion.position = read_vec2(r);
        p.motion.velocity = Vec2Fixed{};
        p.aim_angle = r.u16();
        p.health = r.u8();
        unpack_player_flags(r.u8(), p);
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

void encode_flag_picked_up(ByteWriter& w, const MsgFlagPickedUp& msg) {
    w.u8(static_cast<uint8_t>(msg.flag_team));
    w.u8(msg.player_id);
    w.u32(msg.tick);
}

bool decode_flag_picked_up(ByteReader& r, MsgFlagPickedUp& out) {
    const uint8_t team_raw = r.u8();
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
    const uint8_t team_raw = r.u8();
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
    const uint8_t team_raw = r.u8();
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
    const uint8_t team_raw = r.u8();
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
    const uint8_t team_raw = r.u8();
    out.score_red = r.u8();
    out.score_blue = r.u8();
    if (!r.ok()) {
        return false;
    }
    return decode_team(team_raw, out.winning_team);
}

void encode_heartbeat(ByteWriter&, const MsgHeartbeat&) {}

bool decode_heartbeat(ByteReader& r, MsgHeartbeat&) { return r.ok(); }

} // namespace ctf::protocol

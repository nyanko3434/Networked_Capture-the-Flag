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

// ---------------------------------------------------------------------------
// DELTA_SNAPSHOT (type 19)
//
// Wire layout (after the 8-byte UDP header):
//   u32 last_input_seq          <- offset 0, patched per recipient
//   u8  baseline_ticks_ago      <- baseline tick = header tick - this
//   u8  header_mask             <- which snapshot-header fields changed
//   [changed header fields in bit order]
//   u16 player_change_mask      <- bit i = player slot i changed
//   per set bit: u8 id, u8 field_mask, [only the set fields]
//
// All comparisons happen at wire position granularity (i16, 1/16 px) so an
// encoder never emits a "change" the decoder cannot represent.
// ---------------------------------------------------------------------------

namespace {

constexpr uint8_t kDeltaHeaderScoreRed = 1u << 0;
constexpr uint8_t kDeltaHeaderScoreBlue = 1u << 1;
constexpr uint8_t kDeltaHeaderSecondsRemaining = 1u << 2;
constexpr uint8_t kDeltaHeaderFlagRed = 1u << 3;
constexpr uint8_t kDeltaHeaderFlagBlue = 1u << 4;
constexpr uint8_t kDeltaHeaderPlayerCount = 1u << 5;
constexpr uint8_t kDeltaHeaderKnownMask = 0x3F;

constexpr uint8_t kDeltaFieldPos = 1u << 0;
constexpr uint8_t kDeltaFieldAim = 1u << 1;
constexpr uint8_t kDeltaFieldHealth = 1u << 2;
constexpr uint8_t kDeltaFieldFlagsByte = 1u << 3;
constexpr uint8_t kDeltaFieldKnownMask = 0x0F;

bool wire_pos_eq(const Vec2Fixed& a, const Vec2Fixed& b) {
    return (a.x >> config::kWireFixedShift) ==
               (b.x >> config::kWireFixedShift) &&
           (a.y >> config::kWireFixedShift) ==
               (b.y >> config::kWireFixedShift);
}

bool player_differs(const PlayerState& a, const PlayerState& b) {
    return a.id != b.id || !wire_pos_eq(a.motion.position, b.motion.position) ||
           a.aim_angle != b.aim_angle || a.health != b.health ||
           pack_player_flags(a) != pack_player_flags(b);
}

} // namespace

void encode_delta_snapshot(ByteWriter& w, const WorldSnapshot& current,
                           const WorldSnapshot& baseline) {
    // Offset-0 ack — patched per recipient before sendto, like a full
    // snapshot body.
    w.u32(current.last_input_seq);

    const uint32_t ago =
        current.tick >= baseline.tick ? current.tick - baseline.tick : 0;
    w.u8(static_cast<uint8_t>(ago > 255 ? 255 : ago));

    const int n_cur = current.player_count < config::kMaxPlayers
                          ? current.player_count
                          : config::kMaxPlayers;
    const int n_base = baseline.player_count < config::kMaxPlayers
                           ? baseline.player_count
                           : config::kMaxPlayers;
    const bool roster_changed = current.player_count != baseline.player_count;

    uint8_t header_mask = 0;
    if (current.score_red != baseline.score_red)
        header_mask |= kDeltaHeaderScoreRed;
    if (current.score_blue != baseline.score_blue)
        header_mask |= kDeltaHeaderScoreBlue;
    if (current.seconds_remaining != baseline.seconds_remaining)
        header_mask |= kDeltaHeaderSecondsRemaining;
    if (current.flag_state_red != baseline.flag_state_red ||
        current.flag_carrier_red != baseline.flag_carrier_red)
        header_mask |= kDeltaHeaderFlagRed;
    if (current.flag_state_blue != baseline.flag_state_blue ||
        current.flag_carrier_blue != baseline.flag_carrier_blue)
        header_mask |= kDeltaHeaderFlagBlue;
    if (roster_changed) header_mask |= kDeltaHeaderPlayerCount;
    w.u8(header_mask);

    if (header_mask & kDeltaHeaderScoreRed) w.u8(current.score_red);
    if (header_mask & kDeltaHeaderScoreBlue) w.u8(current.score_blue);
    if (header_mask & kDeltaHeaderSecondsRemaining)
        w.u16(current.seconds_remaining);
    if (header_mask & kDeltaHeaderFlagRed) {
        w.u8(static_cast<uint8_t>(current.flag_state_red));
        w.u8(current.flag_carrier_red);
    }
    if (header_mask & kDeltaHeaderFlagBlue) {
        w.u8(static_cast<uint8_t>(current.flag_state_blue));
        w.u8(current.flag_carrier_blue);
    }
    if (header_mask & kDeltaHeaderPlayerCount) w.u8(current.player_count);

    // A roster change invalidates every cached record: resend all slots in
    // range fully.
    const int n_cmp = roster_changed ? (n_cur > n_base ? n_cur : n_base) : n_cur;
    uint16_t pchange = 0;
    for (int i = 0; i < n_cmp; ++i) {
        if (roster_changed || player_differs(current.players[i],
                                             baseline.players[i])) {
            pchange |= static_cast<uint16_t>(1u << i);
        }
    }
    w.u16(pchange);

    for (int i = 0; i < n_cmp; ++i) {
        if (!(pchange & (1u << i))) continue;
        const PlayerState& p = current.players[i];
        w.u8(p.id);
        uint8_t fm = 0;
        const bool full = roster_changed;
        if (full || !wire_pos_eq(p.motion.position,
                                 baseline.players[i].motion.position))
            fm |= kDeltaFieldPos;
        if (full || p.aim_angle != baseline.players[i].aim_angle)
            fm |= kDeltaFieldAim;
        if (full || p.health != baseline.players[i].health)
            fm |= kDeltaFieldHealth;
        if (full || pack_player_flags(p) !=
                        pack_player_flags(baseline.players[i]))
            fm |= kDeltaFieldFlagsByte;
        w.u8(fm);
        if (fm & kDeltaFieldPos) write_vec2(w, p.motion.position);
        if (fm & kDeltaFieldAim) w.u16(p.aim_angle);
        if (fm & kDeltaFieldHealth) w.u8(p.health);
        if (fm & kDeltaFieldFlagsByte) w.u8(pack_player_flags(p));
    }
}

bool decode_delta_snapshot(ByteReader& r, uint32_t current_tick,
                           WorldSnapshot& cache) {
    // Apply onto a copy and commit only on full success, so a rejected
    // delta leaves the client's cache usable for the next keyframe.
    WorldSnapshot out = cache;

    out.last_input_seq = r.u32();
    const uint8_t ticks_ago = r.u8();
    const uint8_t header_mask = r.u8();
    if (!r.ok()) return false;
    if ((header_mask & ~kDeltaHeaderKnownMask) != 0) return false;

    // Loss detection: the baseline this delta was computed against must be
    // exactly what the cache holds. Anything else means a datagram went
    // missing upstream — drop until the next keyframe.
    if (ticks_ago > current_tick) return false;
    if (cache.tick != current_tick - ticks_ago) return false;

    if (header_mask & kDeltaHeaderScoreRed) out.score_red = r.u8();
    if (header_mask & kDeltaHeaderScoreBlue) out.score_blue = r.u8();
    if (header_mask & kDeltaHeaderSecondsRemaining)
        out.seconds_remaining = r.u16();
    if (header_mask & kDeltaHeaderFlagRed) {
        const uint8_t state_raw = r.u8();
        out.flag_carrier_red = r.u8();
        if (!r.ok() || !decode_flag_state(state_raw, out.flag_state_red))
            return false;
    }
    if (header_mask & kDeltaHeaderFlagBlue) {
        const uint8_t state_raw = r.u8();
        out.flag_carrier_blue = r.u8();
        if (!r.ok() || !decode_flag_state(state_raw, out.flag_state_blue))
            return false;
    }
    if (header_mask & kDeltaHeaderPlayerCount) {
        out.player_count = r.u8();
        if (!r.ok() || out.player_count > config::kMaxPlayers) return false;
    }

    const uint16_t pchange = r.u16();
    if (!r.ok()) return false;
    // Slots are bounded by kMaxPlayers (10); bits 10..15 are protocol
    // violations.
    if (pchange >= (1u << config::kMaxPlayers)) return false;

    for (int i = 0; i < config::kMaxPlayers; ++i) {
        if (!(pchange & (1u << i))) continue;
        PlayerState& p = out.players[i];
        p.id = r.u8();
        const uint8_t fm = r.u8();
        if (!r.ok()) return false;
        if ((fm & ~kDeltaFieldKnownMask) != 0) return false;
        if (fm & kDeltaFieldPos) p.motion.position = read_vec2(r);
        if (fm & kDeltaFieldAim) p.aim_angle = r.u16();
        if (fm & kDeltaFieldHealth) p.health = r.u8();
        if (fm & kDeltaFieldFlagsByte) unpack_player_flags(r.u8(), p);
        if (!r.ok()) return false;
    }

    out.tick = current_tick;
    cache = out;
    return true;
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

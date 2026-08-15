#pragma once

// Message enum, wire header, per-message payload structs, and encode/decode
// signatures for every message in README §5.4. Bodies are unimplemented —
// filled in when shared/ is written together (README §11 week 1).
//
// Never memcpy a struct onto the wire (README §5.2): every message below
// gets an explicit encode/decode pair built on ByteWriter/ByteReader.

#include <cstdint>

#include "bytebuffer.h"
#include "game_config.h"
#include "game_types.h"

namespace ctf::protocol {

// UDP header (README §5.3): 8 bytes, present on every UDP datagram.
constexpr uint16_t kMagic = 0x4346;
constexpr uint8_t kProtocolVersion = 1;

struct UdpHeader {
    uint16_t magic = kMagic;
    uint8_t version = kProtocolVersion;
    uint8_t type = 0;
    uint32_t tick = 0;
};

void encode_udp_header(ByteWriter& w, const UdpHeader& hdr);
bool decode_udp_header(ByteReader& r, UdpHeader& out);

// Every message in README §5.4, transport and direction noted alongside.
enum class MessageType : uint8_t {
    JoinLobby = 1,       // TCP  C->S
    JoinAccept = 2,       // TCP  S->C
    JoinReject = 3,       // TCP  S->C
    LobbyState = 4,       // TCP  S->all
    StartRequest = 5,     // TCP  C->S
    GameStart = 6,        // TCP  S->all
    UdpHello = 7,         // UDP  C->S
    PlayerInput = 8,       // UDP  C->S
    WorldSnapshot = 9,     // UDP  S->all
    ShotFired = 10,        // UDP  S->all
    PlayerKilled = 11,      // TCP  S->all
    PlayerRespawned = 12,   // TCP  S->all
    FlagPickedUp = 13,      // TCP  S->all
    FlagDropped = 14,       // TCP  S->all
    FlagReturned = 15,      // TCP  S->all
    FlagCaptured = 16,      // TCP  S->all
    MatchEnd = 17,          // TCP  S->all
    Heartbeat = 18,         // TCP  C->S
};

// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------

struct MsgJoinLobby {
    char name[16] = {};
};

struct MsgJoinAccept {
    uint8_t player_id = 0;
    uint32_t session_token = 0;
    uint16_t udp_port = 0;
};

enum class JoinRejectReason : uint8_t {
    Full = 0,
    InProgress = 1,
    BadVersion = 2,
};

struct MsgJoinReject {
    JoinRejectReason reason = JoinRejectReason::Full;
};

struct MsgLobbyState {
    uint8_t player_count = 0;
    uint8_t ids[config::kMaxPlayers] = {};
    char names[config::kMaxPlayers][16] = {};
    uint8_t host_id = 0;
};

struct MsgStartRequest {};

struct MsgGameStart {
    uint8_t player_count = 0;
    uint8_t ids[config::kMaxPlayers] = {};
    Team teams[config::kMaxPlayers] = {};
    Vec2Fixed spawn_points[config::kMaxPlayers] = {};
    uint32_t start_tick = 0;
};

struct MsgUdpHello {
    uint8_t player_id = 0;
    uint32_t session_token = 0;
};

// PLAYER_INPUT: player_id, token, base_seq, count, 3x input
// (README §5.4, §5.4 input redundancy note).
struct MsgPlayerInput {
    uint8_t player_id = 0;
    uint32_t session_token = 0;
    uint32_t base_seq = 0;
    uint8_t count = 0;
    InputCmd inputs[config::kInputRedundancy] = {};
};

// WORLD_SNAPSHOT reuses ctf::WorldSnapshot (game_types.h) directly.

struct MsgShotFired {
    uint8_t shooter_id = 0;
    Vec2Fixed origin;
    uint16_t aim_angle = 0;
    Vec2Fixed hit_point;
};

struct MsgPlayerKilled {
    uint8_t victim_id = 0;
    uint8_t killer_id = 0;
    uint32_t tick = 0;
};

struct MsgPlayerRespawned {
    uint8_t player_id = 0;
    Vec2Fixed position;
    uint32_t tick = 0;
};

struct MsgFlagPickedUp {
    Team flag_team = Team::Red;
    uint8_t player_id = 0;
    uint32_t tick = 0;
};

struct MsgFlagDropped {
    Team flag_team = Team::Red;
    Vec2Fixed position;
    uint32_t tick = 0;
};

struct MsgFlagReturned {
    Team flag_team = Team::Red;
    uint8_t player_id = 0;
    uint32_t tick = 0;
};

struct MsgFlagCaptured {
    Team flag_team = Team::Red;
    uint8_t player_id = 0;
    uint32_t tick = 0;
};

struct MsgMatchEnd {
    Team winning_team = Team::Red;
    uint8_t score_red = 0;
    uint8_t score_blue = 0;
};

struct MsgHeartbeat {};

// ---------------------------------------------------------------------------
// encode/decode signatures — one pair per message.
// ---------------------------------------------------------------------------

void encode_join_lobby(ByteWriter& w, const MsgJoinLobby& msg);
bool decode_join_lobby(ByteReader& r, MsgJoinLobby& out);

void encode_join_accept(ByteWriter& w, const MsgJoinAccept& msg);
bool decode_join_accept(ByteReader& r, MsgJoinAccept& out);

void encode_join_reject(ByteWriter& w, const MsgJoinReject& msg);
bool decode_join_reject(ByteReader& r, MsgJoinReject& out);

void encode_lobby_state(ByteWriter& w, const MsgLobbyState& msg);
bool decode_lobby_state(ByteReader& r, MsgLobbyState& out);

void encode_start_request(ByteWriter& w, const MsgStartRequest& msg);
bool decode_start_request(ByteReader& r, MsgStartRequest& out);

void encode_game_start(ByteWriter& w, const MsgGameStart& msg);
bool decode_game_start(ByteReader& r, MsgGameStart& out);

void encode_udp_hello(ByteWriter& w, const MsgUdpHello& msg);
bool decode_udp_hello(ByteReader& r, MsgUdpHello& out);

void encode_player_input(ByteWriter& w, const MsgPlayerInput& msg);
bool decode_player_input(ByteReader& r, MsgPlayerInput& out);

// `snapshot.last_input_seq` is patched per-recipient after this call
// (README §5.5); the body is otherwise serialized once per tick.
void encode_world_snapshot(ByteWriter& w, const WorldSnapshot& msg);
bool decode_world_snapshot(ByteReader& r, WorldSnapshot& out);

void encode_shot_fired(ByteWriter& w, const MsgShotFired& msg);
bool decode_shot_fired(ByteReader& r, MsgShotFired& out);

void encode_player_killed(ByteWriter& w, const MsgPlayerKilled& msg);
bool decode_player_killed(ByteReader& r, MsgPlayerKilled& out);

void encode_player_respawned(ByteWriter& w, const MsgPlayerRespawned& msg);
bool decode_player_respawned(ByteReader& r, MsgPlayerRespawned& out);

void encode_flag_picked_up(ByteWriter& w, const MsgFlagPickedUp& msg);
bool decode_flag_picked_up(ByteReader& r, MsgFlagPickedUp& out);

void encode_flag_dropped(ByteWriter& w, const MsgFlagDropped& msg);
bool decode_flag_dropped(ByteReader& r, MsgFlagDropped& out);

void encode_flag_returned(ByteWriter& w, const MsgFlagReturned& msg);
bool decode_flag_returned(ByteReader& r, MsgFlagReturned& out);

void encode_flag_captured(ByteWriter& w, const MsgFlagCaptured& msg);
bool decode_flag_captured(ByteReader& r, MsgFlagCaptured& out);

void encode_match_end(ByteWriter& w, const MsgMatchEnd& msg);
bool decode_match_end(ByteReader& r, MsgMatchEnd& out);

void encode_heartbeat(ByteWriter& w, const MsgHeartbeat& msg);
bool decode_heartbeat(ByteReader& r, MsgHeartbeat& out);

} // namespace ctf::protocol

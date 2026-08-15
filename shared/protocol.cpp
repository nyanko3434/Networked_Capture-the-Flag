#include "protocol.h"

// Scaffolding only — real encode/decode bodies are written together in the
// shared/ pairing session (README §11 week 1). Decoders return false
// (as if underflowed) so a malformed or not-yet-implemented packet is always
// safely dropped rather than trusted.

namespace ctf::protocol {

void encode_udp_header(ByteWriter&, const UdpHeader&) {}
bool decode_udp_header(ByteReader&, UdpHeader&) { return false; }

void encode_join_lobby(ByteWriter&, const MsgJoinLobby&) {}
bool decode_join_lobby(ByteReader&, MsgJoinLobby&) { return false; }

void encode_join_accept(ByteWriter&, const MsgJoinAccept&) {}
bool decode_join_accept(ByteReader&, MsgJoinAccept&) { return false; }

void encode_join_reject(ByteWriter&, const MsgJoinReject&) {}
bool decode_join_reject(ByteReader&, MsgJoinReject&) { return false; }

void encode_lobby_state(ByteWriter&, const MsgLobbyState&) {}
bool decode_lobby_state(ByteReader&, MsgLobbyState&) { return false; }

void encode_start_request(ByteWriter&, const MsgStartRequest&) {}
bool decode_start_request(ByteReader&, MsgStartRequest&) { return false; }

void encode_game_start(ByteWriter&, const MsgGameStart&) {}
bool decode_game_start(ByteReader&, MsgGameStart&) { return false; }

void encode_udp_hello(ByteWriter&, const MsgUdpHello&) {}
bool decode_udp_hello(ByteReader&, MsgUdpHello&) { return false; }

void encode_player_input(ByteWriter&, const MsgPlayerInput&) {}
bool decode_player_input(ByteReader&, MsgPlayerInput&) { return false; }

void encode_world_snapshot(ByteWriter&, const WorldSnapshot&) {}
bool decode_world_snapshot(ByteReader&, WorldSnapshot&) { return false; }

void encode_shot_fired(ByteWriter&, const MsgShotFired&) {}
bool decode_shot_fired(ByteReader&, MsgShotFired&) { return false; }

void encode_player_killed(ByteWriter&, const MsgPlayerKilled&) {}
bool decode_player_killed(ByteReader&, MsgPlayerKilled&) { return false; }

void encode_player_respawned(ByteWriter&, const MsgPlayerRespawned&) {}
bool decode_player_respawned(ByteReader&, MsgPlayerRespawned&) { return false; }

void encode_flag_picked_up(ByteWriter&, const MsgFlagPickedUp&) {}
bool decode_flag_picked_up(ByteReader&, MsgFlagPickedUp&) { return false; }

void encode_flag_dropped(ByteWriter&, const MsgFlagDropped&) {}
bool decode_flag_dropped(ByteReader&, MsgFlagDropped&) { return false; }

void encode_flag_returned(ByteWriter&, const MsgFlagReturned&) {}
bool decode_flag_returned(ByteReader&, MsgFlagReturned&) { return false; }

void encode_flag_captured(ByteWriter&, const MsgFlagCaptured&) {}
bool decode_flag_captured(ByteReader&, MsgFlagCaptured&) { return false; }

void encode_match_end(ByteWriter&, const MsgMatchEnd&) {}
bool decode_match_end(ByteReader&, MsgMatchEnd&) { return false; }

void encode_heartbeat(ByteWriter&, const MsgHeartbeat&) {}
bool decode_heartbeat(ByteReader&, MsgHeartbeat&) { return false; }

} // namespace ctf::protocol

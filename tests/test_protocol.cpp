// shared/protocol tests (implementation_guide.md §1.6 acceptance criteria):
// round-trip for every message type, malformed-packet fuzzing on the UDP
// header path, and the snapshot per-recipient patch test.
//
// NOT here: the TCP frame reassembly test and TCP overflow test named in
// §1.6. Those exercise shared/net_util.h's recv_framed, which is where TCP
// framing/reassembly actually lives in this codebase (see the note at the
// top of protocol.cpp) -- they're written in test_net_util.cpp instead, once
// §1.7 implements recv_framed for real.

#include "arpa/inet.h"
#include "protocol.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ctf;
using namespace ctf::protocol;

namespace {

void test_round_trip_udp_header() {
    uint8_t buf[64];
    UdpHeader hdr;
    hdr.type = static_cast<uint8_t>(MessageType::PlayerInput);
    hdr.tick = 424242;

    ByteWriter w(buf, sizeof(buf));
    encode_udp_header(w, hdr);
    assert(w.ok());

    ByteReader r(buf, w.size());
    UdpHeader out;
    assert(decode_udp_header(r, out));
    assert(out.magic == kMagic);
    assert(out.version == kProtocolVersion);
    assert(out.type == hdr.type);
    assert(out.tick == hdr.tick);
    printf("test_round_trip_udp_header: OK\n");
}

void test_round_trip_join_lobby() {
    uint8_t buf[64];
    MsgJoinLobby msg;
    std::snprintf(msg.name, sizeof(msg.name), "bibidh");

    ByteWriter w(buf, sizeof(buf));
    encode_join_lobby(w, msg);
    assert(w.ok());

    ByteReader r(buf, w.size());
    MsgJoinLobby out;
    assert(decode_join_lobby(r, out));
    assert(std::memcmp(out.name, msg.name, sizeof(msg.name)) == 0);
    printf("test_round_trip_join_lobby: OK\n");
}

void test_round_trip_join_accept() {
    uint8_t buf[64];
    MsgJoinAccept msg{7, 0xCAFEBABE, 55555};

    ByteWriter w(buf, sizeof(buf));
    encode_join_accept(w, msg);
    ByteReader r(buf, w.size());
    MsgJoinAccept out;
    assert(decode_join_accept(r, out));
    assert(out.player_id == msg.player_id);
    assert(out.session_token == msg.session_token);
    assert(out.udp_port == msg.udp_port);
    printf("test_round_trip_join_accept: OK\n");
}

void test_round_trip_join_reject() {
    uint8_t buf[64];
    MsgJoinReject msg{JoinRejectReason::InProgress};

    ByteWriter w(buf, sizeof(buf));
    encode_join_reject(w, msg);
    ByteReader r(buf, w.size());
    MsgJoinReject out;
    assert(decode_join_reject(r, out));
    assert(out.reason == msg.reason);
    printf("test_round_trip_join_reject: OK\n");
}

void test_round_trip_lobby_state() {
    uint8_t buf[512];
    MsgLobbyState msg;
    msg.player_count = 3;
    msg.ids[0] = 1; msg.ids[1] = 2; msg.ids[2] = 3;
    std::snprintf(msg.names[0], 16, "alice");
    std::snprintf(msg.names[1], 16, "bob");
    std::snprintf(msg.names[2], 16, "carol");
    msg.host_id = 1;

    ByteWriter w(buf, sizeof(buf));
    encode_lobby_state(w, msg);
    ByteReader r(buf, w.size());
    MsgLobbyState out;
    assert(decode_lobby_state(r, out));
    assert(out.player_count == 3);
    assert(out.ids[0] == 1 && out.ids[1] == 2 && out.ids[2] == 3);
    assert(std::strcmp(out.names[0], "alice") == 0);
    assert(std::strcmp(out.names[1], "bob") == 0);
    assert(std::strcmp(out.names[2], "carol") == 0);
    assert(out.host_id == 1);
    printf("test_round_trip_lobby_state: OK\n");
}

void test_round_trip_start_request() {
    uint8_t buf[8];
    MsgStartRequest msg;
    ByteWriter w(buf, sizeof(buf));
    encode_start_request(w, msg);
    ByteReader r(buf, w.size());
    MsgStartRequest out;
    assert(decode_start_request(r, out));
    printf("test_round_trip_start_request: OK\n");
}

void test_round_trip_game_start() {
    uint8_t buf[512];
    MsgGameStart msg;
    msg.player_count = 2;
    msg.ids[0] = 1; msg.ids[1] = 2;
    msg.teams[0] = Team::Red; msg.teams[1] = Team::Blue;
    msg.spawn_points[0] = Vec2Fixed{100 * 256, 200 * 256};
    msg.spawn_points[1] = Vec2Fixed{-50 * 256, 300 * 256};
    msg.start_tick = 0;

    ByteWriter w(buf, sizeof(buf));
    encode_game_start(w, msg);
    ByteReader r(buf, w.size());
    MsgGameStart out;
    assert(decode_game_start(r, out));
    assert(out.player_count == 2);
    assert(out.teams[0] == Team::Red && out.teams[1] == Team::Blue);
    // Wire position is 1/16 px (int16); round-trip is exact only to that
    // resolution (16 internal units per wire unit), not bit-for-bit with the
    // original 1/256 px internal value -- verify within one wire unit.
    assert(std::abs(out.spawn_points[0].x - msg.spawn_points[0].x) < 16);
    assert(std::abs(out.spawn_points[0].y - msg.spawn_points[0].y) < 16);
    assert(std::abs(out.spawn_points[1].x - msg.spawn_points[1].x) < 16);
    printf("test_round_trip_game_start: OK\n");
}

void test_round_trip_udp_hello() {
    uint8_t buf[64];
    MsgUdpHello msg{4, 0x11223344};
    ByteWriter w(buf, sizeof(buf));
    encode_udp_hello(w, msg);
    ByteReader r(buf, w.size());
    MsgUdpHello out;
    assert(decode_udp_hello(r, out));
    assert(out.player_id == 4);
    assert(out.session_token == 0x11223344u);
    printf("test_round_trip_udp_hello: OK\n");
}

void test_round_trip_player_input() {
    uint8_t buf[64];
    MsgPlayerInput msg;
    msg.player_id = 2;
    msg.session_token = 0xABCD1234;
    msg.base_seq = 999;
    msg.count = 3;
    msg.inputs[0] = InputCmd{kInputUp | kInputRight, 100};
    msg.inputs[1] = InputCmd{kInputUp, 150};
    msg.inputs[2] = InputCmd{0, 200};

    ByteWriter w(buf, sizeof(buf));
    encode_player_input(w, msg);
    ByteReader r(buf, w.size());
    MsgPlayerInput out;
    assert(decode_player_input(r, out));
    assert(out.player_id == 2);
    assert(out.session_token == 0xABCD1234u);
    assert(out.base_seq == 999u);
    assert(out.count == 3);
    for (int i = 0; i < 3; ++i) {
        assert(out.inputs[i].buttons == msg.inputs[i].buttons);
        assert(out.inputs[i].aim_angle == msg.inputs[i].aim_angle);
    }
    printf("test_round_trip_player_input: OK\n");
}

void test_round_trip_world_snapshot() {
    uint8_t buf[512];
    WorldSnapshot msg;
    msg.last_input_seq = 42;
    msg.player_count = 2;
    msg.flag_carrier_red = 0xFF;
    msg.flag_carrier_blue = 3;
    msg.flag_state_red = FlagState::AtBase;
    msg.flag_state_blue = FlagState::Carried;
    msg.score_red = 1;
    msg.score_blue = 2;
    msg.seconds_remaining = 300;

    msg.players[0].id = 1;
    msg.players[0].team = Team::Red;
    msg.players[0].motion.position = Vec2Fixed{10 * 256, 20 * 256};
    msg.players[0].aim_angle = 1234;
    msg.players[0].health = 100;
    msg.players[0].alive = true;
    msg.players[0].carrying_flag = false;
    msg.players[0].firing = true;

    msg.players[1].id = 3;
    msg.players[1].team = Team::Blue;
    msg.players[1].motion.position = Vec2Fixed{500 * 256, 700 * 256};
    msg.players[1].aim_angle = 60000;
    msg.players[1].health = 66;
    msg.players[1].alive = false;
    msg.players[1].carrying_flag = true;
    msg.players[1].firing = false;

    ByteWriter w(buf, sizeof(buf));
    encode_world_snapshot(w, msg);
    assert(w.ok());

    ByteReader r(buf, w.size());
    WorldSnapshot out;
    assert(decode_world_snapshot(r, out));
    assert(out.last_input_seq == 42u);
    assert(out.player_count == 2);
    assert(out.flag_carrier_red == 0xFF);
    assert(out.flag_carrier_blue == 3);
    assert(out.flag_state_red == FlagState::AtBase);
    assert(out.flag_state_blue == FlagState::Carried);
    assert(out.score_red == 1 && out.score_blue == 2);
    assert(out.seconds_remaining == 300);

    assert(out.players[0].id == 1);
    assert(out.players[0].team == Team::Red);
    assert(std::abs(out.players[0].motion.position.x - msg.players[0].motion.position.x) < 16);
    assert(std::abs(out.players[0].motion.position.y - msg.players[0].motion.position.y) < 16);
    assert(out.players[0].aim_angle == 1234);
    assert(out.players[0].health == 100);
    assert(out.players[0].alive == true);
    assert(out.players[0].carrying_flag == false);
    assert(out.players[0].firing == true);

    assert(out.players[1].id == 3);
    assert(out.players[1].team == Team::Blue);
    assert(out.players[1].aim_angle == 60000);
    assert(out.players[1].health == 66);
    assert(out.players[1].alive == false);
    assert(out.players[1].carrying_flag == true);
    assert(out.players[1].firing == false);

    printf("test_round_trip_world_snapshot: OK\n");
}

void test_round_trip_shot_fired() {
    uint8_t buf[64];
    MsgShotFired msg;
    msg.shooter_id = 5;
    msg.origin = Vec2Fixed{1 * 256, 2 * 256};
    msg.aim_angle = 999;
    msg.hit_point = Vec2Fixed{50 * 256, 60 * 256};

    ByteWriter w(buf, sizeof(buf));
    encode_shot_fired(w, msg);
    ByteReader r(buf, w.size());
    MsgShotFired out;
    assert(decode_shot_fired(r, out));
    assert(out.shooter_id == 5);
    assert(out.aim_angle == 999);
    assert(std::abs(out.origin.x - msg.origin.x) < 16);
    assert(std::abs(out.hit_point.y - msg.hit_point.y) < 16);
    printf("test_round_trip_shot_fired: OK\n");
}

void test_round_trip_player_killed() {
    uint8_t buf[64];
    MsgPlayerKilled msg{2, 5, 1000};
    ByteWriter w(buf, sizeof(buf));
    encode_player_killed(w, msg);
    ByteReader r(buf, w.size());
    MsgPlayerKilled out;
    assert(decode_player_killed(r, out));
    assert(out.victim_id == 2 && out.killer_id == 5 && out.tick == 1000u);
    printf("test_round_trip_player_killed: OK\n");
}

void test_round_trip_player_respawned() {
    uint8_t buf[64];
    MsgPlayerRespawned msg;
    msg.player_id = 2;
    msg.position = Vec2Fixed{64 * 256, 128 * 256};
    msg.tick = 2000;
    ByteWriter w(buf, sizeof(buf));
    encode_player_respawned(w, msg);
    ByteReader r(buf, w.size());
    MsgPlayerRespawned out;
    assert(decode_player_respawned(r, out));
    assert(out.player_id == 2 && out.tick == 2000u);
    assert(std::abs(out.position.x - msg.position.x) < 16);
    printf("test_round_trip_player_respawned: OK\n");
}

void test_round_trip_flag_messages() {
    uint8_t buf[64];
    {
        MsgFlagPickedUp msg{Team::Blue, 4, 10};
        ByteWriter w(buf, sizeof(buf));
        encode_flag_picked_up(w, msg);
        ByteReader r(buf, w.size());
        MsgFlagPickedUp out;
        assert(decode_flag_picked_up(r, out));
        assert(out.flag_team == Team::Blue && out.player_id == 4 && out.tick == 10u);
    }
    {
        MsgFlagDropped msg{Team::Red, Vec2Fixed{10 * 256, 20 * 256}, 11};
        ByteWriter w(buf, sizeof(buf));
        encode_flag_dropped(w, msg);
        ByteReader r(buf, w.size());
        MsgFlagDropped out;
        assert(decode_flag_dropped(r, out));
        assert(out.flag_team == Team::Red && out.tick == 11u);
    }
    {
        MsgFlagReturned msg{Team::Blue, 6, 12};
        ByteWriter w(buf, sizeof(buf));
        encode_flag_returned(w, msg);
        ByteReader r(buf, w.size());
        MsgFlagReturned out;
        assert(decode_flag_returned(r, out));
        assert(out.flag_team == Team::Blue && out.player_id == 6 && out.tick == 12u);
    }
    {
        MsgFlagCaptured msg{Team::Red, 7, 13};
        ByteWriter w(buf, sizeof(buf));
        encode_flag_captured(w, msg);
        ByteReader r(buf, w.size());
        MsgFlagCaptured out;
        assert(decode_flag_captured(r, out));
        assert(out.flag_team == Team::Red && out.player_id == 7 && out.tick == 13u);
    }
    printf("test_round_trip_flag_messages: OK\n");
}

void test_round_trip_match_end() {
    uint8_t buf[64];
    MsgMatchEnd msg{Team::Blue, 1, 3};
    ByteWriter w(buf, sizeof(buf));
    encode_match_end(w, msg);
    ByteReader r(buf, w.size());
    MsgMatchEnd out;
    assert(decode_match_end(r, out));
    assert(out.winning_team == Team::Blue && out.score_red == 1 && out.score_blue == 3);
    printf("test_round_trip_match_end: OK\n");
}

void test_round_trip_heartbeat() {
    uint8_t buf[8];
    MsgHeartbeat msg;
    ByteWriter w(buf, sizeof(buf));
    encode_heartbeat(w, msg);
    ByteReader r(buf, w.size());
    MsgHeartbeat out;
    assert(decode_heartbeat(r, out));
    printf("test_round_trip_heartbeat: OK\n");
}

void test_malformed_udp_fuzzing() {
    // 1-byte datagram: far too short even for the 8-byte header.
    {
        uint8_t tiny[1] = {0x42};
        ByteReader r(tiny, sizeof(tiny));
        UdpHeader out;
        assert(!decode_udp_header(r, out));
    }
    // Wrong magic, otherwise well-formed header.
    {
        uint8_t buf[8];
        ByteWriter w(buf, sizeof(buf));
        w.u16(0xDEAD); // wrong magic
        w.u8(kProtocolVersion);
        w.u8(static_cast<uint8_t>(MessageType::Heartbeat));
        w.u32(1);
        ByteReader r(buf, w.size());
        UdpHeader out;
        assert(!decode_udp_header(r, out));
    }
    // Wrong version, otherwise well-formed header.
    {
        uint8_t buf[8];
        ByteWriter w(buf, sizeof(buf));
        w.u16(kMagic);
        w.u8(kProtocolVersion + 1); // wrong version
        w.u8(static_cast<uint8_t>(MessageType::Heartbeat));
        w.u32(1);
        ByteReader r(buf, w.size());
        UdpHeader out;
        assert(!decode_udp_header(r, out));
    }
    // Nothing dispatched, nothing crashed, nothing hung in any of the above.
    printf("test_malformed_udp_fuzzing: OK\n");
}

void test_snapshot_patch() {
    // Serialize a WORLD_SNAPSHOT body once, then patch the first 4 bytes
    // (last_input_seq, README §5.5) for two different recipients -- only
    // that field should differ between the two decoded results.
    uint8_t body[256];
    WorldSnapshot msg;
    msg.last_input_seq = 0; // placeholder, overwritten by the patch below
    msg.player_count = 1;
    msg.players[0].id = 9;
    msg.players[0].motion.position = Vec2Fixed{100 * 256, 100 * 256};
    msg.players[0].health = 50;

    ByteWriter w(body, sizeof(body));
    encode_world_snapshot(w, msg);
    size_t body_size = w.size();

    uint8_t body_for_a[256];
    uint8_t body_for_b[256];
    std::memcpy(body_for_a, body, body_size);
    std::memcpy(body_for_b, body, body_size);

    uint32_t seq_a = 111;
    uint32_t seq_b = 222;
    uint32_t net_a = htonl(seq_a);
    uint32_t net_b = htonl(seq_b);
    std::memcpy(body_for_a, &net_a, 4); // patch offset 0, 4 bytes
    std::memcpy(body_for_b, &net_b, 4);

    // Only bytes [0,4) differ between the two patched buffers.
    assert(std::memcmp(body_for_a + 4, body_for_b + 4, body_size - 4) == 0);
    assert(std::memcmp(body_for_a, body_for_b, 4) != 0);

    ByteReader ra(body_for_a, body_size);
    WorldSnapshot out_a;
    assert(decode_world_snapshot(ra, out_a));
    assert(out_a.last_input_seq == seq_a);

    ByteReader rb(body_for_b, body_size);
    WorldSnapshot out_b;
    assert(decode_world_snapshot(rb, out_b));
    assert(out_b.last_input_seq == seq_b);

    // Everything else decoded identically.
    assert(out_a.player_count == out_b.player_count);
    assert(out_a.players[0].id == out_b.players[0].id);
    assert(out_a.players[0].health == out_b.players[0].health);

    printf("test_snapshot_patch: OK\n");
}

} // namespace

int main() {
    test_round_trip_udp_header();
    test_round_trip_join_lobby();
    test_round_trip_join_accept();
    test_round_trip_join_reject();
    test_round_trip_lobby_state();
    test_round_trip_start_request();
    test_round_trip_game_start();
    test_round_trip_udp_hello();
    test_round_trip_player_input();
    test_round_trip_world_snapshot();
    test_round_trip_shot_fired();
    test_round_trip_player_killed();
    test_round_trip_player_respawned();
    test_round_trip_flag_messages();
    test_round_trip_match_end();
    test_round_trip_heartbeat();
    test_malformed_udp_fuzzing();
    test_snapshot_patch();
    printf("All protocol tests passed.\n");
    return 0;
}

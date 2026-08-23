#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "game_config.h"
#include "game_types.h"
#include "protocol.h"

using namespace ctf;
using namespace ctf::protocol;

namespace {

// Builds a representative snapshot with distinct field values.
WorldSnapshot sample_snapshot() {
    WorldSnapshot s;
    s.tick = 123456;
    s.last_input_seq = 777;
    s.player_count = 2;
    s.flag_carrier_red = 0xFF;
    s.flag_carrier_blue = 3;
    s.flag_state_red = FlagState::Dropped;
    s.flag_state_blue = FlagState::Carried;
    s.score_red = 1;
    s.score_blue = 2;
    s.seconds_remaining = 599;

    PlayerState& a = s.players[0];
    a.id = 1;
    a.team = Team::Red;
    a.motion.position = Vec2Fixed{123456, 234567}; // inside the map
    a.aim_angle = 65000;
    a.health = 66;
    a.alive = true;
    a.carrying_flag = false;

    PlayerState& b = s.players[1];
    b.id = 2;
    b.team = Team::Blue;
    b.motion.position = Vec2Fixed{100000, 99999};
    b.aim_angle = 500;
    b.health = 34;
    b.alive = false;
    b.carrying_flag = true;
    return s;
}

template <typename BuildMsg, typename EncodeFn, typename DecodeFn,
          typename CompareFn>
void round_trip(size_t max_bytes, BuildMsg build, EncodeFn encode,
                DecodeFn decode, CompareFn compare) {
    auto msg = build();
    std::vector<uint8_t> buf(max_bytes);
    ByteWriter w(buf.data(), buf.size());
    encode(w, msg);
    REQUIRE(w.ok());

    ByteReader r(buf.data(), w.size());
    decltype(msg) out;
    CHECK(decode(r, out));
    compare(msg, out);
}

} // namespace

TEST_CASE("UDP header encode/decode round-trip") {
    uint8_t buf[config::kUdpHeaderBytes];
    UdpHeader hdr;
    hdr.type = static_cast<uint8_t>(MessageType::PlayerInput);
    hdr.tick = 424242;

    ByteWriter w(buf, sizeof(buf));
    encode_udp_header(w, hdr);
    CHECK(w.ok());
    CHECK(w.size() == config::kUdpHeaderBytes);

    // Magic 0x4346 big-endian -> {0x43, 0x46}.
    CHECK(buf[0] == 0x43);
    CHECK(buf[1] == 0x46);

    ByteReader r(buf, sizeof(buf));
    UdpHeader out;
    CHECK(decode_udp_header(r, out));
    CHECK(out.magic == kMagic);
    CHECK(out.version == kProtocolVersion);
    CHECK(out.type == hdr.type);
    CHECK(out.tick == hdr.tick);
}

TEST_CASE("WORLD_SNAPSHOT round-trip preserves every field") {
    std::vector<uint8_t> buf(512);
    const WorldSnapshot snap = sample_snapshot();

    ByteWriter w(buf.data(), buf.size());
    encode_world_snapshot(w, snap);
    REQUIRE(w.ok());

    ByteReader r(buf.data(), w.size());
    WorldSnapshot out;
    REQUIRE(decode_world_snapshot(r, out));

    CHECK(out.tick == snap.tick);
    CHECK(out.last_input_seq == snap.last_input_seq);
    CHECK(out.player_count == snap.player_count);
    CHECK(out.flag_carrier_red == snap.flag_carrier_red);
    CHECK(out.flag_carrier_blue == snap.flag_carrier_blue);
    CHECK(out.flag_state_red == snap.flag_state_red);
    CHECK(out.flag_state_blue == snap.flag_state_blue);
    CHECK(out.score_red == snap.score_red);
    CHECK(out.score_blue == snap.score_blue);
    CHECK(out.seconds_remaining == snap.seconds_remaining);

    for (int i = 0; i < snap.player_count; ++i) {
        CHECK(out.players[i].id == snap.players[i].id);
        CHECK(out.players[i].team == snap.players[i].team);
        // Wire positions are 1/16 px vs internal 1/256 px: round-trip must
        // preserve the value down to wire granularity.
        const int32_t exp_ax =
            (snap.players[i].motion.position.x >> config::kWireFixedShift)
            << config::kWireFixedShift;
        const int32_t exp_ay =
            (snap.players[i].motion.position.y >> config::kWireFixedShift)
            << config::kWireFixedShift;
        CHECK(out.players[i].motion.position.x == exp_ax);
        CHECK(out.players[i].motion.position.y == exp_ay);
        CHECK(out.players[i].aim_angle == snap.players[i].aim_angle);
        CHECK(out.players[i].health == snap.players[i].health);
        CHECK(out.players[i].alive == snap.players[i].alive);
        CHECK(out.players[i].carrying_flag == snap.players[i].carrying_flag);
    }
}

TEST_CASE("snapshot last_input_seq is patchable per recipient") {
    std::vector<uint8_t> base(512);
    ByteWriter w(base.data(), base.size());
    encode_world_snapshot(w, sample_snapshot());
    REQUIRE(w.ok());
    const size_t body_len = w.size();

    // Two recipients' copies differ only in the leading 4-byte field.
    std::vector<uint8_t> for_a(base.begin(), base.begin() + body_len);
    std::vector<uint8_t> for_b(base.begin(), base.begin() + body_len);
    const uint32_t seq_a = 111;
    const uint32_t seq_b = 222222;

    auto patch = [&](std::vector<uint8_t>& buf, uint32_t seq) {
        ByteWriter pw(buf.data(), buf.size());
        pw.u32(seq); // last_input_seq sits at payload offset 0 (README §5.5)
    };
    patch(for_a, seq_a);
    patch(for_b, seq_b);

    REQUIRE(for_a.size() == for_b.size());
    size_t diff_bytes = 0;
    for (size_t i = 0; i < for_a.size(); ++i) {
        if (for_a[i] != for_b[i]) ++diff_bytes;
    }
    CHECK(diff_bytes <= 4);

    WorldSnapshot out_a;
    ByteReader ra(for_a.data(), for_a.size());
    REQUIRE(decode_world_snapshot(ra, out_a));
    WorldSnapshot out_b;
    ByteReader rb(for_b.data(), for_b.size());
    REQUIRE(decode_world_snapshot(rb, out_b));

    CHECK(out_a.last_input_seq == seq_a);
    CHECK(out_b.last_input_seq == seq_b);
    // Everything else identical between the two decodes.
    CHECK(std::memcmp(&out_a.players, &out_b.players,
                      sizeof(out_a.players)) == 0);
    CHECK(out_a.tick == out_b.tick);
    CHECK(out_a.score_red == out_b.score_red);
}

TEST_CASE("PLAYER_INPUT round-trip with redundancy entries") {
    MsgPlayerInput in;
    in.player_id = 7;
    in.session_token = 0xDEADBEEF;
    in.base_seq = 900;
    in.count = 3;
    in.inputs[0] = InputCmd{1, 100};
    in.inputs[1] = InputCmd{15, 32768};
    in.inputs[2] = InputCmd{31, 65535};

    uint8_t buf[64];
    ByteWriter w(buf, sizeof(buf));
    encode_player_input(w, in);
    REQUIRE(w.ok());

    ByteReader r(buf, sizeof(buf));
    MsgPlayerInput out;
    CHECK(decode_player_input(r, out));
    CHECK(out.player_id == 7);
    CHECK(out.session_token == 0xDEADBEEF);
    CHECK(out.base_seq == 900);
    CHECK(out.count == 3);
    for (int i = 0; i < config::kInputRedundancy; ++i) {
        CHECK(out.inputs[i].buttons == in.inputs[i].buttons);
        CHECK(out.inputs[i].aim_angle == in.inputs[i].aim_angle);
    }
}

TEST_CASE("TCP lobby messages round-trip") {
    SUBCASE("JOIN_LOBBY keeps the 16-byte fixed name") {
        MsgJoinLobby in;
        std::strcpy(in.name, "bibidh");
        round_trip(
            64, [&] { return in; },
            [](ByteWriter& w, const MsgJoinLobby& m) { encode_join_lobby(w, m); },
            [](ByteReader& r, MsgJoinLobby& m) { return decode_join_lobby(r, m); },
            [](const MsgJoinLobby& a, const MsgJoinLobby& b) {
                CHECK(std::memcmp(a.name, b.name, sizeof(a.name)) == 0);
            });
    }

    SUBCASE("JOIN_ACCEPT carries id, token, udp port") {
        MsgJoinAccept in;
        in.player_id = 5;
        in.session_token = 0xCAFEBABE;
        in.udp_port = 54321;
        round_trip(
            64, [&] { return in; },
            [](ByteWriter& w, const MsgJoinAccept& m) { encode_join_accept(w, m); },
            [](ByteReader& r, MsgJoinAccept& m) { return decode_join_accept(r, m); },
            [](const MsgJoinAccept& a, const MsgJoinAccept& b) {
                CHECK(a.player_id == b.player_id);
                CHECK(a.session_token == b.session_token);
                CHECK(a.udp_port == b.udp_port);
            });
    }

    SUBCASE("JOIN_REJECT carries reason") {
        MsgJoinReject in{JoinRejectReason::Full};
        round_trip(
            16, [&] { return in; },
            [](ByteWriter& w, const MsgJoinReject& m) { encode_join_reject(w, m); },
            [](ByteReader& r, MsgJoinReject& m) { return decode_join_reject(r, m); },
            [](const MsgJoinReject& a, const MsgJoinReject& b) {
                CHECK(a.reason == b.reason);
            });
    }

    SUBCASE("LOBBY_STATE roster round-trip") {
        MsgLobbyState in;
        in.player_count = 3;
        in.ids[0] = 1; in.ids[1] = 2; in.ids[2] = 3;
        std::strcpy(in.names[0], "bibidh");
        std::strcpy(in.names[1], "nayan");
        std::strcpy(in.names[2], "bot3");
        in.host_id = 1;
        round_trip(
            512, [&] { return in; },
            [](ByteWriter& w, const MsgLobbyState& m) { encode_lobby_state(w, m); },
            [](ByteReader& r, MsgLobbyState& m) { return decode_lobby_state(r, m); },
            [](const MsgLobbyState& a, const MsgLobbyState& b) {
                CHECK(a.player_count == b.player_count);
                CHECK(a.host_id == b.host_id);
                for (int i = 0; i < a.player_count; ++i) {
                    CHECK(a.ids[i] == b.ids[i]);
                    CHECK(std::strcmp(a.names[i], b.names[i]) == 0);
                }
            });
    }

    SUBCASE("GAME_START assignments round-trip") {
        MsgGameStart in;
        in.player_count = 2;
        in.ids[0] = 1; in.ids[1] = 2;
        in.teams[0] = Team::Red; in.teams[1] = Team::Blue;
        in.spawn_points[0] = Vec2Fixed{8192, 98304};  // red base pocket
        in.spawn_points[1] = Vec2Fixed{314572, 12345}; // blue side, in range
        in.start_tick = 42;
        const int32_t exp_s0x = (in.spawn_points[0].x >> config::kWireFixedShift)
                                << config::kWireFixedShift;
        const int32_t exp_s0y = (in.spawn_points[0].y >> config::kWireFixedShift)
                                << config::kWireFixedShift;
        const int32_t exp_s1x = (in.spawn_points[1].x >> config::kWireFixedShift)
                                << config::kWireFixedShift;
        const int32_t exp_s1y = (in.spawn_points[1].y >> config::kWireFixedShift)
                                << config::kWireFixedShift;
        round_trip(
            1024, [&] { return in; },
            [](ByteWriter& w, const MsgGameStart& m) { encode_game_start(w, m); },
            [](ByteReader& r, MsgGameStart& m) { return decode_game_start(r, m); },
            [&](const MsgGameStart& a, const MsgGameStart& b) {
                CHECK(a.player_count == b.player_count);
                CHECK(a.start_tick == b.start_tick);
                for (int i = 0; i < a.player_count; ++i) {
                    CHECK(a.ids[i] == b.ids[i]);
                    CHECK(a.teams[i] == b.teams[i]);
                    CHECK(b.spawn_points[0].x == exp_s0x);
                    CHECK(b.spawn_points[0].y == exp_s0y);
                    CHECK(b.spawn_points[1].x == exp_s1x);
                    CHECK(b.spawn_points[1].y == exp_s1y);
                }
            });
    }
}

TEST_CASE("combat and flag events round-trip") {
    SUBCASE("SHOT_FIRED") {
        MsgShotFired in;
        in.shooter_id = 4;
        in.origin = Vec2Fixed{1000, 2000};
        in.aim_angle = 40000;
        in.hit_point = Vec2Fixed{300000, 400000};
        round_trip(
            64, [&] { return in; },
            [](ByteWriter& w, const MsgShotFired& m) { encode_shot_fired(w, m); },
            [](ByteReader& r, MsgShotFired& m) { return decode_shot_fired(r, m); },
            [](const MsgShotFired& a, const MsgShotFired& b) {
                CHECK(a.shooter_id == b.shooter_id);
                CHECK(a.aim_angle == b.aim_angle);
                CHECK(b.origin.x ==
                      (a.origin.x >> config::kWireFixedShift)
                          << config::kWireFixedShift);
                CHECK(b.hit_point.x ==
                      (a.hit_point.x >> config::kWireFixedShift)
                          << config::kWireFixedShift);
            });
    }

    SUBCASE("PLAYER_KILLED / PLAYER_RESPAWNED") {
        MsgPlayerKilled kill{9, 2, 555};
        round_trip(
            32, [&] { return kill; },
            [](ByteWriter& w, const MsgPlayerKilled& m) { encode_player_killed(w, m); },
            [](ByteReader& r, MsgPlayerKilled& m) { return decode_player_killed(r, m); },
            [](const MsgPlayerKilled& a, const MsgPlayerKilled& b) {
                CHECK(a.victim_id == b.victim_id);
                CHECK(a.killer_id == b.killer_id);
                CHECK(a.tick == b.tick);
            });

        MsgPlayerRespawned res{9, Vec2Fixed{8192, 16384}, 645};
        round_trip(
            32, [&] { return res; },
            [](ByteWriter& w, const MsgPlayerRespawned& m) {
                encode_player_respawned(w, m);
            },
            [](ByteReader& r, MsgPlayerRespawned& m) {
                return decode_player_respawned(r, m);
            },
            [](const MsgPlayerRespawned& a, const MsgPlayerRespawned& b) {
                CHECK(a.player_id == b.player_id);
                CHECK(a.tick == b.tick);
                CHECK(b.position.x == 8192);
                CHECK(b.position.y == 16384);
            });
    }

    SUBCASE("flag lifecycle events") {
        MsgFlagPickedUp pick{Team::Blue, 6, 700};
        round_trip(
            32, [&] { return pick; },
            [](ByteWriter& w, const MsgFlagPickedUp& m) {
                encode_flag_picked_up(w, m);
            },
            [](ByteReader& r, MsgFlagPickedUp& m) {
                return decode_flag_picked_up(r, m);
            },
            [](const MsgFlagPickedUp& a, const MsgFlagPickedUp& b) {
                CHECK(a.flag_team == b.flag_team);
                CHECK(a.player_id == b.player_id);
                CHECK(a.tick == b.tick);
            });

        MsgFlagDropped drop{Team::Red, Vec2Fixed{123456, 654321}, 701};
        const int32_t exp_dx =
            (drop.position.x >> config::kWireFixedShift) << config::kWireFixedShift;
        round_trip(
            32, [&] { return drop; },
            [](ByteWriter& w, const MsgFlagDropped& m) { encode_flag_dropped(w, m); },
            [](ByteReader& r, MsgFlagDropped& m) { return decode_flag_dropped(r, m); },
            [&](const MsgFlagDropped& a, const MsgFlagDropped& b) {
                CHECK(a.flag_team == b.flag_team);
                CHECK(a.tick == b.tick);
                CHECK(b.position.x == exp_dx);
            });

        MsgFlagReturned ret{Team::Red, 8, 702};
        round_trip(
            32, [&] { return ret; },
            [](ByteWriter& w, const MsgFlagReturned& m) {
                encode_flag_returned(w, m);
            },
            [](ByteReader& r, MsgFlagReturned& m) {
                return decode_flag_returned(r, m);
            },
            [](const MsgFlagReturned& a, const MsgFlagReturned& b) {
                CHECK(a.flag_team == b.flag_team);
                CHECK(a.player_id == b.player_id);
                CHECK(a.tick == b.tick);
            });

        MsgFlagCaptured cap{Team::Blue, 6, 800};
        round_trip(
            32, [&] { return cap; },
            [](ByteWriter& w, const MsgFlagCaptured& m) {
                encode_flag_captured(w, m);
            },
            [](ByteReader& r, MsgFlagCaptured& m) {
                return decode_flag_captured(r, m);
            },
            [](const MsgFlagCaptured& a, const MsgFlagCaptured& b) {
                CHECK(a.flag_team == b.flag_team);
                CHECK(a.player_id == b.player_id);
                CHECK(a.tick == b.tick);
            });
    }
}

TEST_CASE("control-plane messages round-trip") {
    SUBCASE("UDP_HELLO") {
        MsgUdpHello in{6, 0xFEEDFACE};
        round_trip(
            32, [&] { return in; },
            [](ByteWriter& w, const MsgUdpHello& m) { encode_udp_hello(w, m); },
            [](ByteReader& r, MsgUdpHello& m) { return decode_udp_hello(r, m); },
            [](const MsgUdpHello& a, const MsgUdpHello& b) {
                CHECK(a.player_id == b.player_id);
                CHECK(a.session_token == b.session_token);
            });
    }

    SUBCASE("START_REQUEST and HEARTBEAT have empty payloads") {
        uint8_t buf[8];
        ByteWriter w(buf, sizeof(buf));
        encode_start_request(w, MsgStartRequest{});
        CHECK(w.size() == 0);

        ByteWriter w2(buf, sizeof(buf));
        encode_heartbeat(w2, MsgHeartbeat{});
        CHECK(w2.size() == 0);

        ByteReader r(buf, 0);
        MsgStartRequest sr;
        CHECK(decode_start_request(r, sr));
    }

    SUBCASE("MATCH_END") {
        MsgMatchEnd in{Team::Red, 3, 1};
        round_trip(
            16, [&] { return in; },
            [](ByteWriter& w, const MsgMatchEnd& m) { encode_match_end(w, m); },
            [](ByteReader& r, MsgMatchEnd& m) { return decode_match_end(r, m); },
            [](const MsgMatchEnd& a, const MsgMatchEnd& b) {
                CHECK(a.winning_team == b.winning_team);
                CHECK(a.score_red == b.score_red);
                CHECK(a.score_blue == b.score_blue);
            });
    }
}

TEST_CASE("malformed packets never crash and are always rejected") {
    uint8_t scratch[512];

    // 1-byte datagram through the UDP header path.
    {
        scratch[0] = 0x42;
        ByteReader r(scratch, 1);
        UdpHeader hdr;
        CHECK_FALSE(decode_udp_header(r, hdr));
    }

    // Wrong magic rejected before touching the payload.
    {
        ByteWriter w(scratch, sizeof(scratch));
        UdpHeader hdr;
        hdr.magic = 0xFFFF;
        encode_udp_header(w, hdr);
        ByteReader r(scratch, w.size());
        UdpHeader out;
        CHECK_FALSE(decode_udp_header(r, out));
    }

    // Bad version rejected (stale binaries).
    {
        ByteWriter w(scratch, sizeof(scratch));
        UdpHeader hdr;
        hdr.version = kProtocolVersion + 1;
        encode_udp_header(w, hdr);
        ByteReader r(scratch, w.size());
        UdpHeader out;
        CHECK_FALSE(decode_udp_header(r, out));
    }

    // Truncated PLAYER_INPUT: cut the buffer at every length, all reject.
    {
        MsgPlayerInput in;
        in.player_id = 1;
        in.session_token = 99;
        in.base_seq = 5;
        in.count = 3;
        ByteWriter w(scratch, sizeof(scratch));
        encode_player_input(w, in);
        const size_t full = w.size();
        CHECK(full > 4);
        for (size_t cut = 0; cut < full; ++cut) {
            ByteReader r(scratch, cut);
            MsgPlayerInput out;
            bool decoded = decode_player_input(r, out);
            if (decoded) {
                // Only acceptable at exactly-full length.
                CHECK(cut == full);
            } else {
                CHECK_FALSE(r.ok()); // flagged underflow, no crash
            }
        }
    }

    // Truncated WORLD_SNAPSHOT rejects cleanly.
    {
        ByteWriter w(scratch, sizeof(scratch));
        encode_world_snapshot(w, sample_snapshot());
        const size_t full = w.size();
        for (size_t cut = 0; cut < full; cut += 7) {
            ByteReader r(scratch, cut);
            WorldSnapshot out;
            if (decode_world_snapshot(r, out)) {
                CHECK(cut >= full);
            }
        }
    }
}

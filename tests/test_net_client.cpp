#include <doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "bytebuffer.h"
#include "net_client.h"
#include "net_server.h"
#include "net_util.h"
#include "poller.h"
#include "protocol.h"
#include "queues.h"
#include "sim.h"

using namespace ctf;
using namespace ctf::protocol;

namespace {

// ---------------------------------------------------------------------------
// Harness A: a real NetServer + Sim wired exactly like server/main.cpp,
// bound to port 0 (kernel-assigned, TCP and UDP independently) so tests
// exercise the genuine end-to-end pipeline instead of a mock.
// ---------------------------------------------------------------------------
struct TestServer {
    InboundQueue inbound;
    OutboundQueue outbound;
    NetServer net;
    Sim sim;
    std::thread net_thread;
    std::thread sim_thread;

    TestServer()
        : net(/*port=*/0, inbound, outbound, make_poll_poller(),
             /*poll_timeout_ms=*/20, /*udp_silence_ms=*/3000),
          sim(inbound, outbound) {
        REQUIRE(net.start());
        sim.set_wake_fd(net.wake_fd());
        sim.set_tick_rate(30);
        sim.set_snapshot_rate(30);
        net_thread = std::thread([this] { net.run(); });
        sim_thread = std::thread([this] { sim.run(); });
    }

    ~TestServer() {
        net.stop();
        sim.stop();
        net_thread.join();
        sim_thread.join();
    }

    uint16_t tcp_port() const { return net.local_port(); }
};

bool poll_until(const std::function<bool()>& pred, int timeout_ms,
                int step_ms = 10) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

// ---------------------------------------------------------------------------
// Harness B: a hand-rolled fake peer that speaks just enough of the TCP
// handshake + UDP datagram protocol to isolate NetClient's wire format and
// timing behavior from the real server, without reimplementing it.
// ---------------------------------------------------------------------------
struct FakePeer {
    int tcp_listen_fd = -1;
    int tcp_conn_fd = -1;
    int udp_fd = -1;
    uint16_t tcp_port = 0;
    uint16_t udp_port = 0;
    std::vector<uint8_t> tcp_accum;

    FakePeer() {
        tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(tcp_listen_fd >= 0);
        int one = 1;
        setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(bind(tcp_listen_fd, reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)) == 0);
        REQUIRE(listen(tcp_listen_fd, 1) == 0);
        socklen_t len = sizeof(addr);
        getsockname(tcp_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        tcp_port = ntohs(addr.sin_port);

        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE(udp_fd >= 0);
        sockaddr_in uaddr{};
        uaddr.sin_family = AF_INET;
        uaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uaddr.sin_port = 0;
        REQUIRE(bind(udp_fd, reinterpret_cast<sockaddr*>(&uaddr),
                     sizeof(uaddr)) == 0);
        len = sizeof(uaddr);
        getsockname(udp_fd, reinterpret_cast<sockaddr*>(&uaddr), &len);
        udp_port = ntohs(uaddr.sin_port);
    }

    ~FakePeer() {
        if (tcp_conn_fd >= 0) close(tcp_conn_fd);
        if (tcp_listen_fd >= 0) close(tcp_listen_fd);
        if (udp_fd >= 0) close(udp_fd);
    }

    // Blocks until a client connects; returns false on timeout.
    bool accept_client(int timeout_ms) {
        pollfd pfd{tcp_listen_fd, POLLIN, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) return false;
        tcp_conn_fd = accept(tcp_listen_fd, nullptr, nullptr);
        return tcp_conn_fd >= 0;
    }

    // Reads exactly one TCP frame, blocking up to timeout_ms.
    bool recv_tcp_frame(int timeout_ms, uint8_t& type, std::vector<uint8_t>& payload) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        for (;;) {
            uint8_t out_type = 0;
            const uint8_t* out_payload = nullptr;
            uint16_t out_len = 0;
            const size_t consumed = recv_framed(tcp_accum.data(), tcp_accum.size(),
                                                out_type, out_payload, out_len);
            if (consumed > 0 && consumed != SIZE_MAX) {
                type = out_type;
                payload.assign(out_payload, out_payload + out_len);
                tcp_accum.erase(tcp_accum.begin(),
                                tcp_accum.begin() + static_cast<long>(consumed));
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            const int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    .count());
            pollfd pfd{tcp_conn_fd, POLLIN, 0};
            if (::poll(&pfd, 1, remaining_ms) <= 0) continue;
            uint8_t buf[4096];
            ssize_t n = recv(tcp_conn_fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            tcp_accum.insert(tcp_accum.end(), buf, buf + n);
        }
    }

    void send_tcp_frame(MessageType type, const uint8_t* payload, size_t len) {
        std::vector<uint8_t> frame(len + 3);
        frame[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
        frame[1] = static_cast<uint8_t>(len & 0xFF);
        frame[2] = static_cast<uint8_t>(type);
        if (len > 0) std::memcpy(frame.data() + 3, payload, len);
        REQUIRE(send(tcp_conn_fd, frame.data(), frame.size(), 0) ==
                static_cast<ssize_t>(frame.size()));
    }

    // Convenience: JOIN_ACCEPT reply with fixed test values.
    void reply_join_accept(uint8_t player_id, uint32_t token) {
        MsgJoinAccept accept{player_id, token, udp_port};
        uint8_t payload[7];
        ByteWriter w(payload, sizeof(payload));
        encode_join_accept(w, accept);
        send_tcp_frame(MessageType::JoinAccept, payload, w.size());
    }

    void reply_join_reject(JoinRejectReason reason) {
        MsgJoinReject rej{reason};
        uint8_t payload[1];
        ByteWriter w(payload, sizeof(payload));
        encode_join_reject(w, rej);
        send_tcp_frame(MessageType::JoinReject, payload, w.size());
    }

    // Receives one UDP datagram (any type), blocking up to timeout_ms.
    bool recv_udp(int timeout_ms, std::vector<uint8_t>& out, sockaddr_in& from) {
        pollfd pfd{udp_fd, POLLIN, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) return false;
        uint8_t buf[2048];
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) return false;
        out.assign(buf, buf + n);
        return true;
    }

    void send_udp(const uint8_t* data, size_t len, const sockaddr_in& to) {
        sendto(udp_fd, data, len, 0, reinterpret_cast<const sockaddr*>(&to),
              sizeof(to));
    }

    // Builds a minimal-but-valid WORLD_SNAPSHOT datagram (0 players).
    std::vector<uint8_t> build_snapshot_datagram(uint32_t tick, uint32_t ack) {
        WorldSnapshot snap;
        snap.tick = tick;
        snap.last_input_seq = ack;
        snap.player_count = 0;
        std::vector<uint8_t> body(256);
        ByteWriter bw(body.data(), body.size());
        encode_world_snapshot(bw, snap);
        body.resize(bw.size());

        std::vector<uint8_t> dgram(config::kUdpHeaderBytes + body.size());
        ByteWriter hw(dgram.data(), dgram.size());
        encode_udp_header(hw, UdpHeader{kMagic, kProtocolVersion,
                                        static_cast<uint8_t>(
                                            MessageType::WorldSnapshot),
                                        tick});
        std::memcpy(dgram.data() + config::kUdpHeaderBytes, body.data(), body.size());
        return dgram;
    }
};

} // namespace

// ===========================================================================
// A) Real end-to-end tests against TestServer (NetServer + Sim, README §3.2)
// ===========================================================================

TEST_CASE("net_client: full handshake reaches GAME_START against a real server") {
    TestServer server;
    NetClient host;
    NetClient guest;

    REQUIRE(host.connect_and_join("127.0.0.1", server.tcp_port(), "host", 2000));
    CHECK(host.state() == NetClientState::InLobby);
    REQUIRE(guest.connect_and_join("127.0.0.1", server.tcp_port(), "guest", 2000));
    CHECK(guest.state() == NetClientState::InLobby);

    // JOIN_ACCEPT's udp_port must be read from the payload, not assumed
    // equal to the TCP port - assert it matches the real server's ground
    // truth (bound independently via port=0).
    CHECK(host.server_udp_port() == server.net.udp_port());
    CHECK(guest.server_udp_port() == server.net.udp_port());

    // Drive both clients' poll() loops (sends UDP_HELLO, drains LOBBY_STATE)
    // until each has seen itself as a roster member and we know who's host.
    REQUIRE(poll_until(
        [&] {
            host.poll();
            guest.poll();
            return host.lobby_state().player_count == 2 &&
                  guest.lobby_state().player_count == 2;
        },
        2000));

    NetClient& actual_host = host.is_host() ? host : guest;
    NetClient& actual_guest = host.is_host() ? guest : host;
    CHECK(actual_host.is_host());
    CHECK_FALSE(actual_guest.is_host());

    actual_host.send_start_request();

    REQUIRE(poll_until(
        [&] {
            host.poll();
            guest.poll();
            return host.game_started() && guest.game_started();
        },
        2000));

    CHECK(host.game_start().player_count == 2);
    CHECK(guest.game_start().player_count == 2);
}

TEST_CASE("net_client: first WORLD_SNAPSHOT arrives against a real server "
         "without waiting for GAME_START") {
    TestServer server;
    NetClient client;
    REQUIRE(client.connect_and_join("127.0.0.1", server.tcp_port(), "solo", 2000));

    REQUIRE(poll_until(
        [&] {
            client.poll();
            return client.first_snapshot_received();
        },
        2000));

    auto snaps = client.take_snapshots();
    REQUIRE_FALSE(snaps.empty());
}

TEST_CASE("net_client: mid-match join is rejected with reason InProgress") {
    TestServer server;
    NetClient host;
    NetClient guest;
    REQUIRE(host.connect_and_join("127.0.0.1", server.tcp_port(), "host", 2000));
    REQUIRE(guest.connect_and_join("127.0.0.1", server.tcp_port(), "guest", 2000));
    REQUIRE(poll_until(
        [&] {
            host.poll();
            guest.poll();
            return host.lobby_state().player_count == 2;
        },
        2000));
    NetClient& actual_host = host.is_host() ? host : guest;
    actual_host.send_start_request();
    REQUIRE(poll_until([&] {
        host.poll();
        guest.poll();
        return host.game_started() && guest.game_started();
    }, 2000));

    NetClient latecomer;
    const bool joined =
        latecomer.connect_and_join("127.0.0.1", server.tcp_port(), "late", 2000);
    CHECK_FALSE(joined);
    CHECK(latecomer.state() == NetClientState::Rejected);
    CHECK(latecomer.join_reject_reason() == JoinRejectReason::InProgress);
}

// ===========================================================================
// B) Isolated wire-format / timing tests against FakePeer
// ===========================================================================

TEST_CASE("net_client: JOIN_LOBBY wire bytes match the 16-byte fixed name "
         "layout, and JOIN_ACCEPT fields are read back exactly") {
    FakePeer peer;
    NetClient client;

    std::thread server_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        CHECK(type == static_cast<uint8_t>(MessageType::JoinLobby));
        REQUIRE(payload.size() == 16);
        CHECK(std::memcmp(payload.data(), "smoke\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
        peer.reply_join_accept(/*player_id=*/3, /*token=*/0xCAFEBABEu);
    });

    REQUIRE(client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000));
    server_thread.join();

    CHECK(client.player_id() == 3);
    CHECK(client.session_token() == 0xCAFEBABEu);
    CHECK(client.server_udp_port() == peer.udp_port);
    CHECK(client.state() == NetClientState::InLobby);
}

TEST_CASE("net_client: JOIN_REJECT is surfaced with the correct reason and "
         "connect_and_join returns false") {
    FakePeer peer;
    NetClient client;

    std::thread server_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        peer.reply_join_reject(JoinRejectReason::Full);
    });

    const bool ok =
        client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000);
    server_thread.join();

    CHECK_FALSE(ok);
    CHECK(client.state() == NetClientState::Rejected);
    CHECK(client.join_reject_reason() == JoinRejectReason::Full);
}

TEST_CASE("net_client: handshake times out cleanly if the server never "
         "replies") {
    FakePeer peer;
    NetClient client;
    std::thread server_thread([&] { peer.accept_client(500); /* never replies */ });

    const bool ok =
        client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 300);
    server_thread.join();

    CHECK_FALSE(ok);
    CHECK_FALSE(client.connected()); // disconnect() closes the socket
}

TEST_CASE("net_client: PLAYER_INPUT wire bytes carry base_seq and exactly "
         "the requested redundant input count") {
    FakePeer peer;
    NetClient client;
    std::thread accept_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        peer.reply_join_accept(5, 0x11223344u);
    });
    REQUIRE(client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000));
    accept_thread.join();

    InputCmd inputs[3] = {{/*buttons=*/0x01, /*aim=*/1000},
                          {/*buttons=*/0x08, /*aim=*/2000},
                          {/*buttons=*/0x18, /*aim=*/3000}};
    client.send_input(/*base_seq=*/42, inputs, 3);

    std::vector<uint8_t> dgram;
    sockaddr_in from{};
    REQUIRE(peer.recv_udp(2000, dgram, from));

    ByteReader r(dgram.data(), dgram.size());
    UdpHeader hdr;
    REQUIRE(decode_udp_header(r, hdr));
    CHECK(hdr.type == static_cast<uint8_t>(MessageType::PlayerInput));

    MsgPlayerInput decoded;
    REQUIRE(decode_player_input(r, decoded));
    CHECK(r.ok());
    CHECK(decoded.player_id == 5);
    CHECK(decoded.session_token == 0x11223344u);
    CHECK(decoded.base_seq == 42);
    CHECK(decoded.count == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(decoded.inputs[i].buttons == inputs[i].buttons);
        CHECK(decoded.inputs[i].aim_angle == inputs[i].aim_angle);
    }
}

TEST_CASE("net_client: UDP_HELLO is resent roughly every "
         "config::kUdpHelloResendMs until a snapshot arrives, and a client "
         "whose first hello is effectively lost still gets its first "
         "snapshot within about 1s") {
    FakePeer peer;
    NetClient client;
    std::thread accept_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        peer.reply_join_accept(9, 0xAAAAAAAAu);
    });
    REQUIRE(client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000));
    accept_thread.join();

    const auto t0 = std::chrono::steady_clock::now();

    // Single-threaded interleaved pump: repeatedly call client.poll() (which
    // is what actually sends/resends UDP_HELLO) and, in the same iteration,
    // check the fake server's socket with a short timeout. This measures
    // real wall-clock spacing between hellos instead of batching client.poll()
    // calls before ever reading the socket (which would let both hellos sit
    // in the kernel receive buffer and be read back-to-back).
    auto wait_for_next_hello = [&](int timeout_ms, std::vector<uint8_t>& out,
                                   sockaddr_in& out_from) -> bool {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            client.poll();
            if (peer.recv_udp(5, out, out_from)) return true;
        }
        return false;
    };

    // Simulate the first two UDP_HELLOs being lost: read and discard them
    // (as if they vanished on the wire) rather than acting on them.
    std::vector<uint8_t> dgram;
    sockaddr_in from{};
    for (int i = 0; i < 2; ++i) {
        REQUIRE(wait_for_next_hello(600, dgram, from));
        CHECK(dgram.size() >= config::kUdpHeaderBytes);
        CHECK(dgram[3] == static_cast<uint8_t>(MessageType::UdpHello));
    }
    // Third hello: this one "lands" - reply with a snapshot.
    REQUIRE(wait_for_next_hello(600, dgram, from));
    const auto t_third_hello = std::chrono::steady_clock::now();
    auto snap_dgram = peer.build_snapshot_datagram(/*tick=*/1, /*ack=*/0);
    peer.send_udp(snap_dgram.data(), snap_dgram.size(), from);

    REQUIRE(poll_until([&] {
        client.poll();
        return client.first_snapshot_received();
    }, 1000, 5));
    const auto t_snapshot = std::chrono::steady_clock::now();

    const auto ms = [](auto d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };
    // Two resends at ~200ms cadence before the third hello lands.
    CHECK(ms(t_third_hello - t0) >= 2 * config::kUdpHelloResendMs - 50);
    // Total time to first snapshot stays within about 1s, matching the
    // acceptance criterion's "resend intervals ... within ~1s" language.
    CHECK(ms(t_snapshot - t0) < 1000);

    auto snaps = client.take_snapshots();
    REQUIRE(snaps.size() == 1);
    CHECK(snaps[0].tick == 1);
}

TEST_CASE("net_client: HEARTBEAT is sent over TCP roughly every "
         "config::kHeartbeatIntervalSec") {
    FakePeer peer;
    NetClient client;
    std::thread accept_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        peer.reply_join_accept(1, 0x1u);
    });
    REQUIRE(client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000));
    accept_thread.join();

    int heartbeats = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll();
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        if (peer.recv_tcp_frame(20, type, payload)) {
            if (type == static_cast<uint8_t>(MessageType::Heartbeat)) ++heartbeats;
        }
    }
    // ~1.5s at a 1s interval: expect at least one, not a flood.
    CHECK(heartbeats >= 1);
    CHECK(heartbeats <= 3);
}

TEST_CASE("net_client: LOBBY_STATE and GAME_START land in both the typed "
         "accessors and the drained event queue") {
    FakePeer peer;
    NetClient client;
    std::thread accept_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        peer.reply_join_accept(2, 0x2u);

        MsgLobbyState lobby;
        lobby.player_count = 1;
        lobby.ids[0] = 2;
        std::snprintf(lobby.names[0], sizeof(lobby.names[0]), "smoke");
        lobby.host_id = 2;
        uint8_t lp[64];
        ByteWriter lw(lp, sizeof(lp));
        encode_lobby_state(lw, lobby);
        peer.send_tcp_frame(MessageType::LobbyState, lp, lw.size());

        MsgGameStart gs;
        gs.player_count = 1;
        gs.ids[0] = 2;
        gs.teams[0] = Team::Red;
        gs.spawn_points[0] = Vec2Fixed{100, 200};
        gs.start_tick = 0;
        uint8_t gp[64];
        ByteWriter gw(gp, sizeof(gp));
        encode_game_start(gw, gs);
        peer.send_tcp_frame(MessageType::GameStart, gp, gw.size());
    });
    REQUIRE(client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000));
    accept_thread.join();

    REQUIRE(poll_until([&] {
        client.poll();
        return client.game_started();
    }, 2000));

    CHECK(client.lobby_state().player_count == 1);
    CHECK(client.lobby_state().host_id == 2);
    CHECK(client.is_host());
    CHECK(client.game_start().player_count == 1);
    CHECK(client.game_start().teams[0] == Team::Red);

    auto events = client.take_events();
    bool saw_lobby = false, saw_start = false;
    for (auto& ev : events) {
        if (std::holds_alternative<MsgLobbyState>(ev)) saw_lobby = true;
        if (std::holds_alternative<MsgGameStart>(ev)) saw_start = true;
    }
    CHECK(saw_lobby);
    CHECK(saw_start);
}

TEST_CASE("net_client: a malformed UDP datagram (bad magic) is dropped "
         "without disrupting state") {
    FakePeer peer;
    NetClient client;
    std::thread accept_thread([&] {
        REQUIRE(peer.accept_client(2000));
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        REQUIRE(peer.recv_tcp_frame(2000, type, payload));
        peer.reply_join_accept(4, 0x4u);
    });
    REQUIRE(client.connect_and_join("127.0.0.1", peer.tcp_port, "smoke", 2000));
    accept_thread.join();

    std::vector<uint8_t> dgram;
    sockaddr_in from{};
    // client.poll() is what actually sends the first UDP_HELLO; pump it
    // until the fake server observes one.
    REQUIRE(poll_until(
        [&] {
            client.poll();
            return peer.recv_udp(5, dgram, from);
        },
        1000, 0));

    // Send a garbage 1-byte datagram, then a valid one.
    const uint8_t garbage[1] = {0xFF};
    peer.send_udp(garbage, sizeof(garbage), from);
    auto snap_dgram = peer.build_snapshot_datagram(7, 0);
    peer.send_udp(snap_dgram.data(), snap_dgram.size(), from);

    REQUIRE(poll_until([&] {
        client.poll();
        return client.first_snapshot_received();
    }, 1000, 5));
    auto snaps = client.take_snapshots();
    REQUIRE(snaps.size() == 1);
    CHECK(snaps[0].tick == 7);
}

#include <doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "net_server.h"
#include "poller.h"
#include "protocol.h"

using namespace ctf;
using namespace ctf::protocol;

namespace {

constexpr int kPollTimeoutMs = 50;   // short so cycles are observable
constexpr int kSilenceMs = 250;      // injected UDP-silence threshold

// Framed TCP client speaking the protocol over loopback.
struct TestClient {
    int fd = -1;
    std::vector<uint8_t> accum; // persists across wait_for_frame calls

    void connect_to(uint16_t port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr)) == 0);
    }

    template <typename Msg, typename Encode>
    bool send_frame(MessageType type, const Msg& msg, Encode encode) {
        std::vector<uint8_t> payload(160);
        ByteWriter w(payload.data(), payload.size());
        encode(w, msg);
        if (!w.ok()) return false;
        payload.resize(w.size());

        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
        frame.push_back(static_cast<uint8_t>(type));
        frame.insert(frame.end(), payload.begin(), payload.end());
        ssize_t n = send(fd, frame.data(), frame.size(), 0);
        return n == static_cast<ssize_t>(frame.size());
    }

    // Reads whatever is available; returns bytes received (0 on EOF).
    size_t recv_some(std::vector<uint8_t>& buf) {
        uint8_t tmp[4096];
        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n <= 0) {
            buf.clear();
            return 0;
        }
        buf.assign(tmp, tmp + n);
        return buf.size();
    }
};

// Extracts complete frames from an accumulation buffer.
struct FrameView {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

std::vector<FrameView> parse_frames(const std::vector<uint8_t>& data) {
    std::vector<FrameView> out;
    size_t pos = 0;
    while (data.size() - pos >= 3) {
        const uint16_t len =
            static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        if (data.size() - pos < 3u + len) break;
        FrameView fv;
        fv.type = data[pos + 2];
        fv.payload.assign(data.begin() + pos + 3,
                          data.begin() + pos + 3 + len);
        out.push_back(std::move(fv));
        pos += 3 + len;
    }
    return out;
}

const FrameView* find_frame(const std::vector<FrameView>& frames,
                            MessageType type) {
    for (const auto& f : frames) {
        if (f.type == static_cast<uint8_t>(type)) return &f;
    }
    return nullptr;
}

bool wait_for_frame(TestClient& c, MessageType type, FrameView& out,
                    int timeout_ms) {
    std::vector<uint8_t>& accum = c.accum;
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        std::vector<uint8_t> chunk;
        c.recv_some(chunk);
        accum.insert(accum.end(), chunk.begin(), chunk.end());
        auto frames = parse_frames(accum);
        if (const FrameView* f = find_frame(frames, type)) {
            out = *f;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// A running server on a private loopback port.
struct ServerHarness {
    InboundQueue inbound;
    OutboundQueue outbound;
    std::unique_ptr<NetServer> server;
    std::thread thread;

    explicit ServerHarness(int silence_ms = kSilenceMs) {
        server = std::make_unique<NetServer>(
            0, inbound, outbound, make_poll_poller(), kPollTimeoutMs,
            silence_ms);
        REQUIRE(server->start());
        thread = std::thread([this] { server->run(); });
    }

    ~ServerHarness() {
        server->stop();
        thread.join();
    }

    // Drains everything currently queued for the sim.
    std::vector<InboundCommand> drain_inbound() {
        std::vector<InboundCommand> cmds;
        InboundCommand cmd;
        while (inbound.pop(cmd)) cmds.push_back(cmd);
        return cmds;
    }

    void wake() {
        const uint64_t one = 1;
        ssize_t n = write(server->wake_fd(), &one, sizeof(one));
        (void)n;
    }
};

} // namespace

TEST_CASE("handshake: JOIN_LOBBY gets JOIN_ACCEPT and a roster entry") {
    ServerHarness h;
    TestClient c;
    c.connect_to(h.server->local_port());

    MsgJoinLobby join{};
    std::strcpy(join.name, "tester");
    CHECK(c.send_frame(MessageType::JoinLobby, join, encode_join_lobby));

    FrameView accept;
    CHECK(wait_for_frame(c, MessageType::JoinAccept, accept, 1000));
    REQUIRE(accept.payload.size() >= 7);
    ByteReader r(accept.payload.data(), accept.payload.size());
    MsgJoinAccept acc;
    REQUIRE(decode_join_accept(r, acc));
    CHECK(acc.session_token != 0);

    // Roster updated; LOBBY_STATE broadcast follows.
    FrameView lobby_state;
    CHECK(wait_for_frame(c, MessageType::LobbyState, lobby_state, 1000));
    CHECK(h.server->registry().entries().size() == 1);

    // No sim commands until a match starts.
    CHECK(h.drain_inbound().empty());
}

TEST_CASE("start request: host-only, then GAME_START and sim joins") {
    ServerHarness h;
    TestClient host;
    host.connect_to(h.server->local_port());

    MsgJoinLobby join{};
    std::strcpy(join.name, "host");
    REQUIRE(host.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(host, MessageType::JoinAccept, accept, 1000));

    // A second client so begin_match has two players.
    TestClient guest;
    guest.connect_to(h.server->local_port());
    MsgJoinLobby gjoin{};
    std::strcpy(gjoin.name, "guest");
    REQUIRE(guest.send_frame(MessageType::JoinLobby, gjoin,
                             encode_join_lobby));
    FrameView gaccept;
    REQUIRE(wait_for_frame(guest, MessageType::JoinAccept, gaccept, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 3));

    SUBCASE("non-host start request is ignored") {
        MsgStartRequest req{};
        CHECK(guest.send_frame(MessageType::StartRequest, req,
                               encode_start_request));
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollTimeoutMs * 4));
        FrameView start;
        CHECK_FALSE(wait_for_frame(guest, MessageType::GameStart, start, 300));
        CHECK(h.drain_inbound().empty());
    }

    SUBCASE("host start request begins the match") {
        MsgStartRequest req{};
        CHECK(host.send_frame(MessageType::StartRequest, req,
                              encode_start_request));

        FrameView start;
        CHECK(wait_for_frame(host, MessageType::GameStart, start, 1000));
        REQUIRE(start.payload.size() >= 2);
        ByteReader r(start.payload.data(), start.payload.size());
        MsgGameStart gs;
        REQUIRE(decode_game_start(r, gs));
        CHECK(gs.player_count == 2);

        // Sim receives one PlayerJoined per participant with teams set.
        auto cmds = h.drain_inbound();
        REQUIRE(cmds.size() == 2);
        int red = 0, blue = 0;
        for (const auto& cmd : cmds) {
            CHECK(cmd.type == InboundCommandType::PlayerJoined);
            (cmd.team == Team::Red ? red : blue)++;
        }
        CHECK(red + blue == 2);
        CHECK(red <= 1);  // balanced within one
        CHECK(blue <= 1); // balanced within one
    }
}

TEST_CASE("udp: hello registers address; inputs flow; garbage is dropped") {
    // Long silence window: this test exercises input routing, not
    // disconnect detection, and its sleeps must not trip the detector.
    ServerHarness h(60000);
    TestClient c;
    c.connect_to(h.server->local_port());

    // Join + start so the player exists in the sim.
    MsgJoinLobby join{};
    std::strcpy(join.name, "p1");
    REQUIRE(c.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(c, MessageType::JoinAccept, accept, 1000));
    ByteReader ar(accept.payload.data(), accept.payload.size());
    MsgJoinAccept acc;
    REQUIRE(decode_join_accept(ar, acc));
    const uint8_t my_id = acc.player_id;
    const uint32_t token = acc.session_token;

    // A second participant so the match can actually start.
    TestClient c2;
    c2.connect_to(h.server->local_port());
    MsgJoinLobby join2{};
    std::strcpy(join2.name, "p2");
    REQUIRE(c2.send_frame(MessageType::JoinLobby, join2,
                          encode_join_lobby));
    FrameView accept2;
    REQUIRE(wait_for_frame(c2, MessageType::JoinAccept, accept2, 1000));

    MsgStartRequest req{};
    REQUIRE(c.send_frame(MessageType::StartRequest, req,
                         encode_start_request));
    FrameView start;
    REQUIRE(wait_for_frame(c, MessageType::GameStart, start, 1000));
    h.drain_inbound();

    // A bound UDP socket acting as this client's source.
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(udp_fd >= 0);
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    src.sin_port = 0;
    REQUIRE(bind(udp_fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) == 0);
    socklen_t slen = sizeof(src);
    REQUIRE(getsockname(udp_fd, reinterpret_cast<sockaddr*>(&src), &slen) ==
            0);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(h.server->udp_port());
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    auto dgram_with_header = [&](uint16_t magic, uint8_t version,
                                 uint8_t type, uint32_t tick,
                                 const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> d(config::kUdpHeaderBytes);
        d[0] = magic >> 8; d[1] = magic & 0xFF;
        d[2] = version;
        d[3] = type;
        for (int i = 0; i < 4; ++i) {
            d[4 + i] = static_cast<uint8_t>(tick >> (24 - 8 * i));
        }
        d.insert(d.end(), payload.begin(), payload.end());
        return d;
    };

    // 1. Garbage datagrams are dropped silently.
    std::vector<uint8_t> bad_magic =
        dgram_with_header(0x1234, kProtocolVersion, 7, 0, {1, 2, 3});
    std::vector<uint8_t> bad_version =
        dgram_with_header(kMagic, kProtocolVersion + 5, 7, 0, {1, 2, 3});
    std::vector<uint8_t> one_byte{0x43};
    for (const auto* d : {&bad_magic, &bad_version, &one_byte}) {
        REQUIRE(sendto(udp_fd, d->data(), d->size(), 0,
                       reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) > 0);
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPollTimeoutMs * 3));
    CHECK(h.drain_inbound().empty());

    // 2. UDP_HELLO registers our address.
    MsgUdpHello hello{my_id, token};
    std::vector<uint8_t> hello_payload(16);
    {
        ByteWriter w(hello_payload.data(), hello_payload.size());
        encode_udp_hello(w, hello);
        hello_payload.resize(w.size());
    }
    auto hello_dgram = dgram_with_header(kMagic, kProtocolVersion, 7, 0,
                                         hello_payload);
    REQUIRE(sendto(udp_fd, hello_dgram.data(), hello_dgram.size(), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) > 0);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPollTimeoutMs * 3));

    // 3. Valid PLAYER_INPUT produces sim commands with correct seqs.
    MsgPlayerInput in;
    in.player_id = my_id;
    in.session_token = token;
    in.base_seq = 42;
    in.count = 3;
    in.inputs[0] = InputCmd{1, 10};
    in.inputs[1] = InputCmd{2, 20};
    in.inputs[2] = InputCmd{4, 30};
    std::vector<uint8_t> input_payload(48);
    {
        ByteWriter w(input_payload.data(), input_payload.size());
        encode_player_input(w, in);
        input_payload.resize(w.size());
    }
    auto input_dgram = dgram_with_header(kMagic, kProtocolVersion, 8, 9,
                                         input_payload);
    REQUIRE(sendto(udp_fd, input_dgram.data(), input_dgram.size(), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) > 0);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPollTimeoutMs * 3));

    auto cmds = h.drain_inbound();
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0].seq == 40); // base_seq - count + 1
    CHECK(cmds[1].seq == 41);
    CHECK(cmds[2].seq == 42);
    CHECK(cmds[0].input.buttons == 1);

    // 4. Tampered token is rejected even with valid header.
    MsgPlayerInput forged;
    forged.player_id = my_id;
    forged.session_token = token ^ 0xDEAD;
    forged.base_seq = 100;
    forged.count = 1;
    std::vector<uint8_t> forged_payload(48);
    {
        ByteWriter w(forged_payload.data(), forged_payload.size());
        encode_player_input(w, forged);
        forged_payload.resize(w.size());
    }
    auto forged_dgram = dgram_with_header(kMagic, kProtocolVersion, 8, 10,
                                          forged_payload);
    REQUIRE(sendto(udp_fd, forged_dgram.data(), forged_dgram.size(), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) > 0);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPollTimeoutMs * 3));
    CHECK(h.drain_inbound().empty());

    close(udp_fd);
}

TEST_CASE("mid-match JOIN_LOBBY is rejected with reason InProgress") {
    ServerHarness h(60000); // no silence interference
    TestClient host;
    host.connect_to(h.server->local_port());
    MsgJoinLobby j1{};
    std::strcpy(j1.name, "host");
    REQUIRE(host.send_frame(MessageType::JoinLobby, j1, encode_join_lobby));
    FrameView a1;
    REQUIRE(wait_for_frame(host, MessageType::JoinAccept, a1, 1000));

    TestClient guest;
    guest.connect_to(h.server->local_port());
    MsgJoinLobby j2{};
    std::strcpy(j2.name, "guest");
    REQUIRE(guest.send_frame(MessageType::JoinLobby, j2, encode_join_lobby));
    FrameView a2;
    REQUIRE(wait_for_frame(guest, MessageType::JoinAccept, a2, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 3));

    // Start the match.
    MsgStartRequest req{};
    REQUIRE(host.send_frame(MessageType::StartRequest, req,
                            encode_start_request));
    FrameView start;
    REQUIRE(wait_for_frame(host, MessageType::GameStart, start, 1000));

    // Third client tries to join MID-MATCH: rejected with InProgress.
    TestClient late;
    late.connect_to(h.server->local_port());
    MsgJoinLobby j3{};
    std::strcpy(j3.name, "late");
    REQUIRE(late.send_frame(MessageType::JoinLobby, j3, encode_join_lobby));

    FrameView reject;
    REQUIRE(wait_for_frame(late, MessageType::JoinReject, reject, 1000));
    REQUIRE(reject.payload.size() >= 1);
    ByteReader r(reject.payload.data(), reject.payload.size());
    MsgJoinReject rr;
    REQUIRE(decode_join_reject(r, rr));
    CHECK(rr.reason == JoinRejectReason::InProgress);
    CHECK(h.server->registry().entries().size() == 2); // not leaked
}

TEST_CASE("tcp close removes the client within one poll cycle") {
    ServerHarness h;
    TestClient c;
    c.connect_to(h.server->local_port());

    MsgJoinLobby join{};
    std::strcpy(join.name, "bye");
    REQUIRE(c.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(c, MessageType::JoinAccept, accept, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 2));
    REQUIRE(h.server->registry().entries().size() == 1);

    close(c.fd);
    c.fd = -1;

    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 4));
    CHECK(h.server->registry().entries().empty());

    // Lobby-phase departure never reaches the sim (no match started).
    CHECK(h.drain_inbound().empty());
}

TEST_CASE("udp silence disconnects a quiet client") {
    ServerHarness h(kSilenceMs);
    TestClient c;
    c.connect_to(h.server->local_port());

    MsgJoinLobby join{};
    std::strcpy(join.name, "quiet");
    REQUIRE(c.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(c, MessageType::JoinAccept, accept, 1000));

    std::this_thread::sleep_for(
        std::chrono::milliseconds(kSilenceMs + kPollTimeoutMs * 6));

    CHECK(h.server->registry().entries().empty());
}

TEST_CASE("pending output beyond 64KB disconnects a slow client") {
    ServerHarness h;
    TestClient c;
    c.connect_to(h.server->local_port());

    MsgJoinLobby join{};
    std::strcpy(join.name, "slow");
    REQUIRE(c.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(c, MessageType::JoinAccept, accept, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 2));

    // Pump > 64KB of broadcast events without ever reading them client-
    // side faster than the cap check: the cap fires on buffered size
    // regardless of kernel buffers.
    OutboundEvent ev;
    ev.type = OutboundEventType::TcpBroadcast;
    ev.payload.assign(1024, 0xAB);
    for (int i = 0; i < static_cast<int>(
                        config::kTcpPendingBufferCapBytes / 1024) +
                    4;
         ++i) {
        h.outbound.push(ev);
    }
    h.wake();

    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPollTimeoutMs * 6));
    CHECK(h.server->registry().entries().empty());
}

TEST_CASE("eventfd wake beats the poll timeout") {
    ServerHarness h;
    TestClient c;
    c.connect_to(h.server->local_port());

    // Long poll timeout would hide busy-wake bugs; inject a big one via a
    // second server instance.
    InboundQueue in2;
    OutboundQueue out2;
    NetServer slow(0, in2, out2, make_poll_poller(),
                   /*poll_timeout_ms=*/5000, /*udp_silence_ms=*/60000);
    REQUIRE(slow.start());
    std::thread runner([&] { slow.run(); });

    TestClient sc;
    sc.connect_to(slow.local_port());
    MsgJoinLobby join{};
    std::strcpy(join.name, "wake");
    REQUIRE(sc.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(sc, MessageType::JoinAccept, accept, 1000));

    // Publish a snapshot event and poke the eventfd: the client must see
    // the TCP fan-out far sooner than the 5s poll timeout.
    OutboundEvent ev;
    ev.type = OutboundEventType::TcpBroadcast;
    MsgPlayerKilled kill{1, 2, 5};
    ev.payload.push_back(static_cast<uint8_t>(MessageType::PlayerKilled));
    ev.payload.resize(ev.payload.size() + 16);
    {
        ByteWriter w(ev.payload.data() + 1, 16);
        encode_player_killed(w, kill);
        ev.payload.resize(1 + w.size());
    }
    out2.push(ev);
    const uint64_t one = 1;
    REQUIRE(write(slow.wake_fd(), &one, sizeof(one)) == 8);

    FrameView killed;
    CHECK(wait_for_frame(sc, MessageType::PlayerKilled, killed, 1500));

    slow.stop();
    runner.join();
}

TEST_CASE("outbound counters track snapshots and fanned-out events") {
    ServerHarness h;
    TestClient c;
    c.connect_to(h.server->local_port());
    MsgJoinLobby join{};
    std::strcpy(join.name, "counter");
    REQUIRE(c.send_frame(MessageType::JoinLobby, join, encode_join_lobby));
    FrameView accept;
    REQUIRE(wait_for_frame(c, MessageType::JoinAccept, accept, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 2));

    const uint64_t tcp_before = h.server->tcp_events_fanned_out();

    for (int i = 0; i < 5; ++i) {
        OutboundEvent ev;
        ev.type = OutboundEventType::UdpSnapshot;
        ev.payload.assign(64, 0x11);
        h.outbound.push(ev);
    }
    h.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 3));
    CHECK(h.server->udp_snapshots_sent() == 5);

    OutboundEvent ev;
    ev.type = OutboundEventType::TcpBroadcast;
    ev.payload.assign(8, 0x22);
    h.outbound.push(ev);
    h.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollTimeoutMs * 3));
    CHECK(h.server->tcp_events_fanned_out() == tcp_before + 1);
}

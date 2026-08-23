#include <doctest.h>

#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "broadcast.h"
#include "client_registry.h"
#include "protocol.h"

using namespace ctf;
using namespace ctf::protocol;

namespace {

// A bound UDP socket on localhost that acts as a recipient we can recvfrom.
struct UdpSink {
    int fd = -1;
    sockaddr_in addr{};

    UdpSink() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE(fd >= 0);
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
                0);
        socklen_t len = sizeof(addr);
        REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    }
    ~UdpSink() {
        if (fd >= 0) close(fd);
    }
};

ClientEntry* attach_udp_client(ClientRegistry& reg, uint8_t tag,
                               const UdpSink& sink) {
    ClientEntry* e = reg.add(100 + tag, "c" + std::to_string(tag));
    REQUIRE(e != nullptr);
    e->udp_addr = sink.addr;
    e->has_udp_addr = true;
    return e;
}

} // namespace

TEST_CASE("tcp event fan-out reaches every client's pending buffer") {
    ClientRegistry reg;
    ClientEntry* a = reg.add(1, "a");
    ClientEntry* b = reg.add(2, "b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    std::vector<uint8_t> payload{static_cast<uint8_t>(MessageType::PlayerKilled),
                                 1, 2, 0, 0, 0, 42};
    int reached = broadcast_tcp_event(reg, payload);
    CHECK(reached == 2);
    CHECK(a->pending_out == payload);
    CHECK(b->pending_out == payload);

    // Appends accumulate; nothing is sent here (net_server flushes).
    broadcast_tcp_event(reg, payload);
    CHECK(a->pending_out.size() == payload.size() * 2);
}

TEST_CASE("udp snapshots differ only in the patched 4-byte field") {
    UdpSink sink;
    ClientRegistry reg;
    ClientEntry* a = attach_udp_client(reg, 1, sink);
    ClientEntry* b = attach_udp_client(reg, 2, sink);

    // Build a snapshot body once.
    WorldSnapshot snap;
    snap.tick = 7777;
    snap.player_count = 2;
    std::vector<uint8_t> body(512);
    ByteWriter w(body.data(), body.size());
    encode_world_snapshot(w, snap);
    body.resize(w.size());

    uint32_t acks[config::kMaxPlayers] = {};
    acks[a->player_id] = 111;
    acks[b->player_id] = 222222;

    // Server socket to send from.
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(udp_fd >= 0);

    send_udp_snapshots(reg, udp_fd, body, acks, /*tick=*/7777);

    auto recv_one = [&]() -> std::vector<uint8_t> {
        std::vector<uint8_t> buf(2048);
        ssize_t n = recv(sink.fd, buf.data(), buf.size(), 0);
        REQUIRE(n > 0);
        buf.resize(static_cast<size_t>(n));
        return buf;
    };

    std::vector<uint8_t> dgram_a = recv_one();
    std::vector<uint8_t> dgram_b = recv_one();

    const size_t hdr = config::kUdpHeaderBytes;
    REQUIRE(dgram_a.size() == hdr + body.size());
    REQUIRE(dgram_b.size() == hdr + body.size());

    // Transport header: magic, version, type=WorldSnapshot, tick.
    CHECK(dgram_a[0] == 0x43);
    CHECK(dgram_a[1] == 0x46);
    CHECK(dgram_a[3] == static_cast<uint8_t>(MessageType::WorldSnapshot));
    ByteReader hr(dgram_a.data() + 4, 4);
    CHECK(hr.u32() == 7777);

    // Exactly the first 4 payload bytes differ between recipients.
    size_t diff_bytes = 0;
    for (size_t i = 0; i < dgram_a.size(); ++i) {
        if (dgram_a[i] != dgram_b[i]) ++diff_bytes;
    }
    CHECK(diff_bytes <= 4);

    // Patched values are the per-recipient acks.
    auto read_seq = [&](const std::vector<uint8_t>& d) {
        ByteReader r(d.data() + hdr, d.size() - hdr);
        return r.u32();
    };
    CHECK(read_seq(dgram_a) == 111);
    CHECK(read_seq(dgram_b) == 222222);

    // Everything after the patch matches the shared body.
    CHECK(std::memcmp(dgram_a.data() + hdr + 4, body.data() + 4,
                      body.size() - 4) == 0);

    close(udp_fd);
}

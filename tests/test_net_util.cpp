#include <doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "game_config.h"
#include "net_util.h"
#include "protocol.h"

using namespace ctf;
using namespace ctf::protocol;

namespace {

// A call that must not block longer than `ms` — used to prove non-blocking
// behavior (test fails on timeout rather than hanging the run).
template <typename Fn>
bool finishes_within(Fn&& fn, int ms) {
    bool done = false;
    std::thread t([&] {
        fn();
        done = true;
    });
    for (int waited = 0; waited < ms && !done; waited += 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    t.join();
    return done;
}

} // namespace

TEST_CASE("set_nonblocking: recv with no data returns EAGAIN immediately") {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    REQUIRE(set_nonblocking(fds[0]));

    char byte = 0;
    bool got_eagain = false;
    CHECK(finishes_within(
        [&] {
            ssize_t n = recv(fds[0], &byte, 1, 0);
            got_eagain = (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        },
        100));
    CHECK(got_eagain);

    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("send_all loops over partial writes and delivers everything") {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    REQUIRE(set_nonblocking(fds[0]));
    REQUIRE(set_nonblocking(fds[1]));

    uint8_t payload[4096];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7);
    }

    ssize_t sent = send_all(fds[0], payload, sizeof(payload));
    CHECK(sent == static_cast<ssize_t>(sizeof(payload)));

    // Peer sees the full payload.
    uint8_t echo[sizeof(payload)] = {};
    size_t received = 0;
    while (received < sizeof(payload)) {
        ssize_t n = recv(fds[1], echo + received, sizeof(payload) - received, 0);
        REQUIRE(n > 0);
        received += static_cast<size_t>(n);
    }
    CHECK(std::memcmp(payload, echo, sizeof(payload)) == 0);

    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("send_all returns early on EAGAIN without blocking forever") {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    REQUIRE(set_nonblocking(fds[0]));

    // Shrink SO_SNDBUF so the socket fills quickly.
    int small_buf = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small_buf, sizeof(small_buf));

    std::vector<uint8_t> big(256 * 1024, 0xAB);
    ssize_t sent = -2;
    CHECK(finishes_within([&] { sent = send_all(fds[0], big.data(), big.size()); },
                          500));
    // Either fully sent or a partial count — but never blocked indefinitely
    // and never an error other than a partial send.
    CHECK(sent >= 0);

    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("recv_framed extracts one frame; split feeds behave identically") {
    // Build one frame: [u16 len][u8 type][payload].
    uint8_t frame[64];
    ByteWriter w(frame, sizeof(frame));
    w.u16(4);
    w.u8(static_cast<uint8_t>(MessageType::PlayerKilled));
    w.u8(3); // victim
    w.u8(5); // killer
    w.u16(77);
    const size_t frame_len = w.size();

    SUBCASE("whole frame in one buffer") {
        uint8_t type = 0;
        const uint8_t* payload = nullptr;
        uint16_t plen = 0;
        size_t consumed =
            recv_framed(frame, frame_len, type, payload, plen);
        CHECK(consumed == frame_len);
        CHECK(type == static_cast<uint8_t>(MessageType::PlayerKilled));
        CHECK(plen == 4);
        CHECK(payload[0] == 3);
    }

    SUBCASE("one byte at a time reports incomplete until complete") {
        uint8_t type = 0;
        const uint8_t* payload = nullptr;
        uint16_t plen = 0;
        uint8_t accum[64] = {};
        for (size_t have = 0; have + 1 <= frame_len; ++have) {
            std::memcpy(accum + have, frame + have, 1);
            size_t got =
                recv_framed(accum, have + 1, type, payload, plen);
            if (have + 1 < frame_len) {
                CHECK(got == 0); // incomplete yet
            } else {
                CHECK(got == frame_len);
                CHECK(type ==
                      static_cast<uint8_t>(MessageType::PlayerKilled));
                CHECK(plen == 4);
            }
        }
    }

    SUBCASE("split mid-header") {
        uint8_t type = 0;
        const uint8_t* payload = nullptr;
        uint16_t plen = 0;
        uint8_t accum[64] = {};
        // First 2 bytes = only part of the u16 length prefix visible.
        std::memcpy(accum, frame, 2);
        CHECK(recv_framed(accum, 2, type, payload, plen) == 0);
        // Feed the rest.
        std::memcpy(accum + 2, frame + 2, frame_len - 2);
        size_t got = recv_framed(accum, frame_len, type, payload, plen);
        CHECK(got == frame_len);
        CHECK(plen == 4);
    }
}

TEST_CASE("recv_framed dispatches two back-to-back frames in sequence") {
    uint8_t buf[128];
    ByteWriter w(buf, sizeof(buf));
    // Both frames have empty payloads: [u16 len=0][u8 type].
    w.u16(0);
    w.u8(static_cast<uint8_t>(MessageType::Heartbeat));
    w.u16(0);
    w.u8(static_cast<uint8_t>(MessageType::StartRequest));
    const size_t total = w.size();
    REQUIRE(total == 6);

    uint8_t type = 0;
    const uint8_t* payload = nullptr;
    uint16_t plen = 0;

    size_t c1 = recv_framed(buf, total, type, payload, plen);
    CHECK(c1 == 3);
    CHECK(plen == 0);
    CHECK(type == static_cast<uint8_t>(MessageType::Heartbeat));

    size_t c2 = recv_framed(buf + c1, total - c1, type, payload, plen);
    CHECK(c2 == 3);
    CHECK(type == static_cast<uint8_t>(MessageType::StartRequest));
}

TEST_CASE("frame exceeding the 4KB cap is flagged as oversize") {
    // Declare a length beyond kTcpFrameCapBytes: caller must treat this as a
    // disconnect condition. recv_framed returns a sentinel (SIZE_MAX).
    uint8_t hdr[3];
    ByteWriter w(hdr, sizeof(hdr));
    w.u16(static_cast<uint16_t>(config::kTcpFrameCapBytes + 1));
    w.u8(static_cast<uint8_t>(MessageType::Heartbeat));

    uint8_t type = 0;
    const uint8_t* payload = nullptr;
    uint16_t plen = 0;
    CHECK(recv_framed(hdr, sizeof(hdr), type, payload, plen) == SIZE_MAX);
}

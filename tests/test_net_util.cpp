// shared/net_util tests. Covers implementation_guide.md §1.7's own
// acceptance criteria (non-blocking recv/send behavior) AND the two TCP
// tests named in §1.6 that actually belong here since recv_framed (not
// protocol.cpp) is where framing/reassembly lives in this codebase: the
// split-read reassembly test and the 4KB overflow/disconnect test (README
// §5.3, §10).

#include "game_config.h"
#include "net_util.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

using namespace ctf;

namespace {

// --- set_nonblocking --------------------------------------------------

void test_set_nonblocking_recv_returns_immediately() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]));

    struct timeval start, end;
    gettimeofday(&start, nullptr);

    uint8_t buf[16];
    ssize_t n = recv(fds[0], buf, sizeof(buf), 0); // no data available
    int saved_errno = errno;

    gettimeofday(&end, nullptr);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                       (end.tv_usec - start.tv_usec) / 1000;

    assert(n == -1);
    assert(saved_errno == EAGAIN || saved_errno == EWOULDBLOCK);
    assert(elapsed_ms < 100); // must not have blocked

    close(fds[0]);
    close(fds[1]);
    printf("test_set_nonblocking_recv_returns_immediately: OK (%ld ms)\n", elapsed_ms);
}

// --- send_all -----------------------------------------------------------

void test_send_all_partial_on_full_buffer() {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]));
    assert(set_nonblocking(fds[1]));

    // Shrink the send buffer so it's easy to fill without a huge payload.
    int small_buf = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small_buf, sizeof(small_buf));

    // A payload much larger than the (shrunk) socket buffers on both ends,
    // with nobody draining the receive side yet.
    std::vector<uint8_t> payload(256 * 1024, 0x5A);

    ssize_t sent = send_all(fds[0], payload.data(), payload.size());
    assert(sent >= 0);
    assert(static_cast<size_t>(sent) < payload.size()); // partial, not full, not blocked/crashed

    // Drain the peer, then send the remainder -- it should eventually all
    // get through.
    std::vector<uint8_t> received(sent);
    size_t total_received = 0;
    while (total_received < received.size()) {
        ssize_t n = recv(fds[1], received.data() + total_received,
                          received.size() - total_received, 0);
        if (n <= 0) break;
        total_received += static_cast<size_t>(n);
    }
    assert(total_received == static_cast<size_t>(sent));

    // Send the remainder; drain concurrently isn't needed for a unix
    // socketpair of this size once the first chunk was consumed, but loop in
    // case another partial occurs.
    size_t remaining_offset = static_cast<size_t>(sent);
    while (remaining_offset < payload.size()) {
        ssize_t n2 = send_all(fds[0], payload.data() + remaining_offset,
                               payload.size() - remaining_offset);
        assert(n2 >= 0);
        if (n2 == 0) {
            // Drain more from the peer to make room, then retry.
            uint8_t drain_buf[4096];
            ssize_t d = recv(fds[1], drain_buf, sizeof(drain_buf), 0);
            if (d <= 0) break;
            continue;
        }
        remaining_offset += static_cast<size_t>(n2);
    }
    assert(remaining_offset == payload.size());

    close(fds[0]);
    close(fds[1]);
    printf("test_send_all_partial_on_full_buffer: OK (first call sent %zd/%zu bytes)\n",
           sent, payload.size());
}

// --- recv_framed: reassembly across arbitrary chunk splits ---------------

std::vector<uint8_t> build_test_frame(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    uint16_t len = static_cast<uint16_t>(payload.size());
    frame.push_back(static_cast<uint8_t>(len >> 8));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    frame.push_back(type);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Feeds `frame` into the accumulation buffer in the given chunk sizes,
// calling recv_framed after each append, and returns the single dispatched
// message (asserts exactly one dispatch, at the expected point).
void feed_and_dispatch(const std::vector<uint8_t>& frame, const std::vector<size_t>& chunk_sizes,
                        uint8_t& out_type, std::vector<uint8_t>& out_payload) {
    std::vector<uint8_t> accum;
    size_t offset = 0;
    int dispatch_count = 0;

    for (size_t chunk : chunk_sizes) {
        size_t take = std::min(chunk, frame.size() - offset);
        accum.insert(accum.end(), frame.begin() + offset, frame.begin() + offset + take);
        offset += take;

        uint8_t type;
        const uint8_t* payload_ptr;
        uint16_t payload_len;
        size_t frame_size = recv_framed(accum.data(), accum.size(), type, payload_ptr, payload_len);
        if (frame_size > 0) {
            ++dispatch_count;
            out_type = type;
            out_payload.assign(payload_ptr, payload_ptr + payload_len);
            // memmove the remainder out, as a real caller would.
            accum.erase(accum.begin(), accum.begin() + frame_size);
        }
    }
    assert(dispatch_count == 1); // dispatched exactly once, not twice, not dropped
}

void test_reassembly_one_byte_at_a_time() {
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto frame = build_test_frame(static_cast<uint8_t>(protocol::MessageType::PlayerKilled), payload);

    // One shot (whole frame at once) as the reference result.
    uint8_t ref_type;
    const uint8_t* ref_payload_ptr;
    uint16_t ref_payload_len;
    size_t ref_size = recv_framed(frame.data(), frame.size(), ref_type, ref_payload_ptr, ref_payload_len);
    assert(ref_size == frame.size());

    // Split 1 byte at a time.
    std::vector<size_t> one_byte_chunks(frame.size(), 1);
    uint8_t type;
    std::vector<uint8_t> out_payload;
    feed_and_dispatch(frame, one_byte_chunks, type, out_payload);

    assert(type == ref_type);
    assert(out_payload.size() == ref_payload_len);
    assert(std::memcmp(out_payload.data(), ref_payload_ptr, ref_payload_len) == 0);
    printf("test_reassembly_one_byte_at_a_time: OK\n");
}

void test_reassembly_split_mid_header() {
    std::vector<uint8_t> payload = {0xAA, 0xBB, 0xCC};
    auto frame = build_test_frame(static_cast<uint8_t>(protocol::MessageType::Heartbeat), payload);

    // Split so the first chunk lands in the middle of the 3-byte header
    // (after just 1 byte of it), then the rest arrives in one big chunk.
    std::vector<size_t> chunks = {1, frame.size() - 1};
    uint8_t type;
    std::vector<uint8_t> out_payload;
    feed_and_dispatch(frame, chunks, type, out_payload);

    assert(type == static_cast<uint8_t>(protocol::MessageType::Heartbeat));
    assert(out_payload == payload);
    printf("test_reassembly_split_mid_header: OK\n");
}

void test_reassembly_matches_one_shot() {
    // Same message, fed in one shot vs. an arbitrary uneven split -- results
    // must be identical.
    std::vector<uint8_t> payload(50, 0x42);
    auto frame = build_test_frame(static_cast<uint8_t>(protocol::MessageType::ShotFired), payload);

    uint8_t one_shot_type;
    const uint8_t* one_shot_payload_ptr;
    uint16_t one_shot_payload_len;
    size_t one_shot_size =
        recv_framed(frame.data(), frame.size(), one_shot_type, one_shot_payload_ptr, one_shot_payload_len);
    assert(one_shot_size == frame.size());

    std::vector<size_t> uneven_chunks = {2, 5, 1, frame.size()}; // arbitrary, includes an early tiny chunk
    uint8_t split_type;
    std::vector<uint8_t> split_payload;
    feed_and_dispatch(frame, uneven_chunks, split_type, split_payload);

    assert(split_type == one_shot_type);
    assert(split_payload.size() == one_shot_payload_len);
    assert(std::memcmp(split_payload.data(), one_shot_payload_ptr, one_shot_payload_len) == 0);
    printf("test_reassembly_matches_one_shot: OK\n");
}

// --- 4KB cap / disconnect signal ------------------------------------------

void test_overflow_no_complete_frame_signals_disconnect() {
    // A stream that claims a payload_len large enough that a complete frame
    // never fits within the cap, and the accumulation buffer itself grows
    // past config::kTcpFrameCapBytes without recv_framed ever returning
    // non-zero. This is the caller-visible signal net_server.cpp (§2.2) acts
    // on to disconnect the client -- recv_framed itself never crashes,
    // hangs, or reads out of bounds while that happens.
    std::vector<uint8_t> accum;
    uint16_t claimed_len = 60000; // larger than kTcpFrameCapBytes (4096)
    accum.push_back(static_cast<uint8_t>(claimed_len >> 8));
    accum.push_back(static_cast<uint8_t>(claimed_len & 0xFF));
    accum.push_back(static_cast<uint8_t>(protocol::MessageType::PlayerInput));

    // Keep appending payload bytes (never enough to complete the claimed
    // frame) until the accumulation buffer itself exceeds the cap.
    while (accum.size() <= config::kTcpFrameCapBytes) {
        accum.push_back(0);

        uint8_t type;
        const uint8_t* payload_ptr;
        uint16_t payload_len;
        size_t frame_size = recv_framed(accum.data(), accum.size(), type, payload_ptr, payload_len);
        assert(frame_size == 0); // never a complete frame
    }

    // The buffer is now past the cap with no complete frame -- this is
    // exactly the condition a caller (net_server.cpp) checks to disconnect
    // the client, rather than growing the buffer without bound.
    assert(accum.size() > config::kTcpFrameCapBytes);
    printf("test_overflow_no_complete_frame_signals_disconnect: OK (accum=%zu bytes, cap=%zu)\n",
           accum.size(), config::kTcpFrameCapBytes);
}

} // namespace

int main() {
    test_set_nonblocking_recv_returns_immediately();
    test_send_all_partial_on_full_buffer();
    test_reassembly_one_byte_at_a_time();
    test_reassembly_split_mid_header();
    test_reassembly_matches_one_shot();
    test_overflow_no_complete_frame_signals_disconnect();
    printf("All net_util tests passed.\n");
    return 0;
}

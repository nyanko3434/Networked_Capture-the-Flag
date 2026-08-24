#include "net_util.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>

#include "game_config.h"

// README §3.2 (non-blocking sockets, EAGAIN handling) and §5.3 (TCP framing:
// [u16 payload_len][u8 type][payload...], byte-stream reassembly).

namespace ctf {

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return false;
    }
    return true;
}

ssize_t send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t n = send(fd, p + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (n > 0) {
            total_sent += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            // send() returning 0 for a non-empty request isn't a normal TCP
            // outcome; treat it as "nothing more can be sent right now"
            // rather than looping forever.
            break;
        }
        // n < 0
        if (errno == EINTR) {
            continue; // interrupted by a signal -- retry the same call
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Non-blocking fd with no send buffer space left right now.
            // Return early with whatever was actually sent so the caller
            // can buffer the remainder (README §3.2's per-client pending
            // output buffer) rather than blocking here.
            return static_cast<ssize_t>(total_sent);
        }
        // A real error (e.g. ECONNRESET, or EPIPE with MSG_NOSIGNAL
        // suppressing the signal but still returning the error).
        return -1;
    }
    return static_cast<ssize_t>(total_sent);
}

size_t recv_framed(const uint8_t* buf, size_t len, uint8_t& out_type,
                    const uint8_t*& out_payload, uint16_t& out_payload_len) {
    // "While buffered >= 3, peek len" (README §5.3).
    if (len < config::kTcpFrameHeaderBytes) {
        return 0;
    }

    // Header is [u16 payload_len][u8 type], written in network byte order by
    // the same ByteWriter path used for message payloads (README §5.2) --
    // read it the same way here rather than relying on host endianness.
    uint16_t payload_len = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    uint8_t type = buf[2];

    const size_t frame_size = config::kTcpFrameHeaderBytes + payload_len;
    if (len < frame_size) {
        // "If buffered >= len + 3, dispatch one message" -- not yet, more
        // bytes still need to arrive. The caller is responsible for
        // recognizing when the *accumulation buffer itself* has grown past
        // config::kTcpFrameCapBytes without a complete frame ever forming --
        // that's the disconnect trigger (README §5.3), not something this
        // pure parsing function decides.
        return 0;
    }

    out_type = type;
    out_payload = buf + config::kTcpFrameHeaderBytes;
    out_payload_len = payload_len;
    return frame_size;
}

} // namespace ctf

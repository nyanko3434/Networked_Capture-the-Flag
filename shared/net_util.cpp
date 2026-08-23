#include "net_util.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "game_config.h"

// Small POSIX socket helpers (README §3.2 outbound sends). All sockets in
// the server are non-blocking; send_all reports partial progress on EAGAIN
// so the caller can buffer the remainder instead of blocking its thread.

namespace ctf {

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

ssize_t send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full: report progress so far; caller buffers
            // the rest and waits for POLLOUT.
            return static_cast<ssize_t>(sent);
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // Hard error (EPIPE, ECONNRESET, ...).
        return sent > 0 ? static_cast<ssize_t>(sent) : -1;
    }
    return static_cast<ssize_t>(sent);
}

size_t recv_framed(const uint8_t* buf, size_t len, uint8_t& out_type,
                   const uint8_t*& out_payload, uint16_t& out_payload_len) {
    if (len < config::kTcpFrameHeaderBytes) {
        return 0;
    }

    // Peek the length prefix (big-endian per ByteWriter::u16).
    const uint16_t payload_len =
        static_cast<uint16_t>((buf[0] << 8) | buf[1]);

    if (payload_len > config::kTcpFrameCapBytes) {
        // Oversize frame: caller must disconnect this client rather than
        // accumulate an unbounded buffer (README §5.3 4KB cap).
        return SIZE_MAX;
    }

    if (len < config::kTcpFrameHeaderBytes + payload_len) {
        return 0; // complete frame not buffered yet
    }

    out_type = buf[2];
    out_payload = buf + config::kTcpFrameHeaderBytes;
    out_payload_len = payload_len;
    return config::kTcpFrameHeaderBytes + payload_len;
}

} // namespace ctf

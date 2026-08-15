#pragma once

// Small POSIX socket helpers shared by server, client, and bot (README §4).
//
// TCP is a byte stream, not a message stream (README §5.3): recv_framed
// implements the [u16 payload_len][u8 type][payload...] reassembly loop
// against a caller-owned accumulation buffer, capped per
// config::kTcpFrameCapBytes.

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace ctf {

// Sets O_NONBLOCK on fd. Returns true on success.
bool set_nonblocking(int fd);

// Sends the full buffer, looping over partial writes on a blocking fd, or
// returning early with EAGAIN on a non-blocking one. Returns bytes sent, or
// -1 on error.
ssize_t send_all(int fd, const void* buf, size_t len);

// Attempts to extract exactly one TCP frame from `buf` (README §5.3):
// while buffered >= 3, peek len; if buffered >= len + 3, one message is
// available. On success, returns the number of bytes the frame occupies
// (including the 3-byte header) so the caller can memmove the remainder;
// returns 0 if no complete frame is buffered yet.
size_t recv_framed(const uint8_t* buf, size_t len, uint8_t& out_type,
                    const uint8_t*& out_payload, uint16_t& out_payload_len);

} // namespace ctf

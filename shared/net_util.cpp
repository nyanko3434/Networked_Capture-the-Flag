#include "net_util.h"

// Scaffolding only — real socket/framing bodies are written together in the
// shared/ pairing session (README §11 week 1).

namespace ctf {

bool set_nonblocking(int) { return false; }

ssize_t send_all(int, const void*, size_t) { return -1; }

size_t recv_framed(const uint8_t*, size_t, uint8_t&, const uint8_t*&, uint16_t&) {
    return 0;
}

} // namespace ctf

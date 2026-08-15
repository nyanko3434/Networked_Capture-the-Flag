#pragma once

// Client-side networking: TCP handshake (README §5.6) plus the UDP input/
// snapshot stream. Shared verbatim with the bot, minus rendering.

#include <cstdint>
#include <string>

#include "game_types.h"
#include "protocol.h"

namespace ctf {

class NetClient {
public:
    NetClient();
    ~NetClient();

    // Connects TCP, sends JOIN_LOBBY, and waits for JOIN_ACCEPT.
    bool connect_and_join(const std::string& host, uint16_t port, const std::string& name);

    // Sends UDP_HELLO, resent every config::kUdpHelloResendMs until the
    // first snapshot arrives (README §5.6).
    void send_udp_hello();

    // Sends the last config::kInputRedundancy inputs in one PLAYER_INPUT
    // packet (README §5.4).
    void send_input(uint32_t base_seq, const InputCmd* inputs, uint8_t count);

    // Pumps both sockets; dispatches decoded messages via the not-yet-wired
    // callbacks below.
    void poll();

    uint8_t player_id() const { return player_id_; }
    uint32_t session_token() const { return session_token_; }

private:
    int tcp_fd_ = -1;
    int udp_fd_ = -1;
    uint8_t player_id_ = 0;
    uint32_t session_token_ = 0;
};

} // namespace ctf

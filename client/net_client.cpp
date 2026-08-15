#include "net_client.h"

// Scaffolding only — real connection/handshake bodies are written together
// in week 1 (README §11).

namespace ctf {

NetClient::NetClient() = default;
NetClient::~NetClient() = default;

bool NetClient::connect_and_join(const std::string&, uint16_t, const std::string&) {
    return false;
}

void NetClient::send_udp_hello() {}

void NetClient::send_input(uint32_t, const InputCmd*, uint8_t) {}

void NetClient::poll() {}

} // namespace ctf

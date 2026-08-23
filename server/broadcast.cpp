#include "broadcast.h"

#include <sys/socket.h>
#include <unistd.h>

#include "bytebuffer.h"

namespace ctf {

int broadcast_tcp_event(ClientRegistry& registry,
                        const std::vector<uint8_t>& payload) {
    int reached = 0;
    for (auto& entry : registry.entries()) {
        ClientEntry* e =
            registry.find_by_id(entry.player_id);
        if (e == nullptr) continue;
        e->pending_out.insert(e->pending_out.end(), payload.begin(),
                              payload.end());
        ++reached;
    }
    return reached;
}

void send_udp_snapshots(ClientRegistry& registry, int udp_fd,
                        const std::vector<uint8_t>& body,
                        const uint32_t* acks) {
    if (udp_fd < 0 || body.size() < 4) return;

    for (auto& entry : registry.entries()) {
        if (!entry.has_udp_addr) continue;
        ClientEntry* e = registry.find_by_id(entry.player_id);
        if (e == nullptr) continue;

        // Patch the 4-byte last_input_seq field (payload offset 0) for
        // this recipient, then one sendto (README §5.5).
        std::vector<uint8_t> dgram(body);
        dgram[0] = static_cast<uint8_t>(acks[e->player_id] >> 24);
        dgram[1] = static_cast<uint8_t>(acks[e->player_id] >> 16);
        dgram[2] = static_cast<uint8_t>(acks[e->player_id] >> 8);
        dgram[3] = static_cast<uint8_t>(acks[e->player_id]);

        sendto(udp_fd, dgram.data(), dgram.size(), 0,
               reinterpret_cast<const sockaddr*>(&e->udp_addr),
               sizeof(e->udp_addr));
    }
}

} // namespace ctf

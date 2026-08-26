#include "broadcast.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>

#include "bytebuffer.h"
#include "protocol.h"

namespace ctf {

int broadcast_tcp_event(ClientRegistry& registry,
                        const std::vector<uint8_t>& payload) {
    int reached = 0;
    registry.for_each_live([&](const ClientEntry& entry) {
        ClientEntry* e = registry.find_by_id(entry.player_id);
        if (e == nullptr) return;
        e->pending_out.insert(e->pending_out.end(), payload.begin(),
                              payload.end());
        ++reached;
    });
    return reached;
}

void send_udp_snapshots(ClientRegistry& registry, int udp_fd,
                        const uint8_t* body, size_t body_len,
                        const uint32_t* acks, uint32_t tick,
                        uint8_t udp_msg_type) {
    if (udp_fd < 0 || body_len < 4) return;

    // 8-byte UDP header (README §5.3): magic, version, type, tick.
    // Pre-allocate on stack — max snapshot is ~128 bytes + 8 header = ~136.
    std::array<uint8_t, 600> dgram;
    const size_t dgram_size = config::kUdpHeaderBytes + body_len;
    if (dgram_size > dgram.size()) return;

    dgram[0] = protocol::kMagic >> 8;
    dgram[1] = protocol::kMagic & 0xFF;
    dgram[2] = protocol::kProtocolVersion;
    dgram[3] = udp_msg_type;
    dgram[4] = static_cast<uint8_t>(tick >> 24);
    dgram[5] = static_cast<uint8_t>(tick >> 16);
    dgram[6] = static_cast<uint8_t>(tick >> 8);
    dgram[7] = static_cast<uint8_t>(tick);
    std::copy(body, body + body_len,
              dgram.begin() + config::kUdpHeaderBytes);

    registry.for_each_live([&](const ClientEntry& entry) {
        if (!entry.has_udp_addr) return;
        ClientEntry* e = registry.find_by_id(entry.player_id);
        if (e == nullptr) return;

        // Patch the 4-byte last_input_seq field (payload offset 0, i.e.
        // right after the transport header) for this recipient, then one
        // sendto (README §5.5).
        const size_t off = config::kUdpHeaderBytes;
        dgram[off + 0] = static_cast<uint8_t>(acks[e->player_id] >> 24);
        dgram[off + 1] = static_cast<uint8_t>(acks[e->player_id] >> 16);
        dgram[off + 2] = static_cast<uint8_t>(acks[e->player_id] >> 8);
        dgram[off + 3] = static_cast<uint8_t>(acks[e->player_id]);

        sendto(udp_fd, dgram.data(), dgram_size, 0,
               reinterpret_cast<const sockaddr*>(&e->udp_addr),
               sizeof(e->udp_addr));
    });
}

} // namespace ctf

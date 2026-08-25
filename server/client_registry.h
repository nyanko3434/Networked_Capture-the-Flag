#pragma once

// Owned exclusively by the network thread (README §3.2): fd, UDP address,
// session token, name. The simulation thread never touches a socket or this
// registry directly — it only sees CMD_PLAYER_JOINED / CMD_PLAYER_LEFT
// commands via the inbound queue.

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "game_config.h"
namespace ctf {

struct ClientEntry {
    uint8_t player_id = 0;
    int tcp_fd = -1;
    std::string name;

    sockaddr_in udp_addr{};
    bool has_udp_addr = false;

    uint32_t session_token = 0;
    uint32_t last_input_tick = 0; // for the 3s UDP silence timeout (README §5.7)

    // Pending TCP output (network-thread-owned): appended by broadcast,
    // flushed by net_server while POLLOUT is registered. Capped at
    // config::kTcpPendingBufferCapBytes — beyond that the client goes.
    std::vector<uint8_t> pending_out;
};

class ClientRegistry {
public:
    ClientRegistry();

    ClientEntry* add(int tcp_fd, const std::string& name);
    void remove(uint8_t player_id);

    ClientEntry* find_by_id(uint8_t player_id);
    ClientEntry* find_by_fd(int tcp_fd);

    // Live entries by reference — callers iterate without copying.
    // Storage is stable (tombstoned in place), so the reference is safe.
    const std::vector<ClientEntry>& storage() const { return storage_; }

    // Convenience: iterate live entries without copying. Calls fn(entry)
    // for each entry with tcp_fd != -1.
    template <typename Fn>
    void for_each_live(Fn fn) const {
        for (const auto& e : storage_) {
            if (e.tcp_fd >= 0) fn(e);
        }
    }

private:
    // Indexed by player_id; a free slot has tcp_fd == -1.
    std::vector<ClientEntry> storage_;
};

} // namespace ctf

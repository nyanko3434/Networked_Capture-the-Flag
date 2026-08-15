#pragma once

// Lobby roster and host tracking (README §5.6, §7.5). Host is whoever
// joined first; if they leave during the lobby, the lowest remaining player
// id is promoted.

#include <cstdint>
#include <vector>

namespace ctf {

class Lobby {
public:
    void add_player(uint8_t player_id);
    void remove_player(uint8_t player_id);

    uint8_t host_id() const;
    bool can_start() const;

    const std::vector<uint8_t>& player_ids() const { return player_ids_; }

private:
    std::vector<uint8_t> player_ids_;
    uint8_t host_id_ = 0;
};

} // namespace ctf

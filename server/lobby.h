#pragma once

// Lobby roster, host tracking, and match flow (README §5.6 steps 1-2/4-5,
// §7.5). Host is whoever joined first; if they leave during the lobby, the
// lowest remaining player id is promoted. No mid-match joining — MATCH_END
// returns the same roster to the lobby for an immediate rematch.

#include <cstdint>
#include <tuple>
#include <vector>

#include "game_config.h"

namespace ctf {

// Outcome of a join attempt; maps onto JOIN_ACCEPT / JOIN_REJECT reasons.
enum class JoinResult : uint8_t {
    Ok = 0,
    Full = 1,
    InProgress = 2,
};

class Lobby {
public:
    // Attempts to add a player; Ok on success or the rejection reason
    // (Full covers both capacity and duplicate-id joins).
    JoinResult add_player(uint8_t player_id);

    // Removes a player; promotes the lowest remaining id to host if the
    // host left while in the lobby phase.
    void remove_player(uint8_t player_id);

    uint8_t host_id() const { return host_id_; }

    // True only when `player_id` is the host AND no match is running.
    bool can_issue_start(uint8_t player_id) const;

    // Transitions lobby -> in-progress. Fails if already in progress.
    bool begin_match();

    // MATCH_END: back to lobby with roster intact.
    void end_match();

    bool in_progress() const { return in_progress_; }

    // Balanced random team assignment (shuffle, alternate). Sizes differ
    // by at most one; repeated calls vary statistically.
    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> assign_teams() const;

    const std::vector<uint8_t>& player_ids() const { return player_ids_; }

private:
    void promote_host();

    std::vector<uint8_t> player_ids_;
    uint8_t host_id_ = 0;
    bool in_progress_ = false;
};

} // namespace ctf

#include "lobby.h"

#include <algorithm>
#include <random>

namespace ctf {

namespace {
std::mt19937& rng() {
    static std::mt19937 generator{std::random_device{}()};
    return generator;
}
} // namespace

JoinResult Lobby::add_player(uint8_t player_id) {
    if (in_progress_) {
        return JoinResult::InProgress; // no mid-match joins
    }
    if (player_ids_.size() >= static_cast<size_t>(config::kMaxPlayers) ||
        std::find(player_ids_.begin(), player_ids_.end(), player_id) !=
            player_ids_.end()) {
        return JoinResult::Full;
    }

    player_ids_.push_back(player_id);
    if (player_ids_.size() == 1) {
        host_id_ = player_id; // first joiner is host
    }
    return JoinResult::Ok;
}

void Lobby::remove_player(uint8_t player_id) {
    auto it = std::find(player_ids_.begin(), player_ids_.end(), player_id);
    if (it == player_ids_.end()) {
        return;
    }
    const bool was_host = (player_id == host_id_);
    player_ids_.erase(it);

    // Host promotion happens when the lobby-phase host leaves; mid-match
    // departures keep the match's team structure, so promotion is deferred
    // to end_match() by re-deriving the lowest id then.
    if (was_host && !player_ids_.empty() && !in_progress_) {
        promote_host();
    } else if (was_host && in_progress_) {
        // Track that the host slot must be refilled at MATCH_END.
        host_id_ = player_ids_.empty()
                       ? 0
                       : *std::min_element(player_ids_.begin(),
                                           player_ids_.end());
    }
}

void Lobby::promote_host() {
    if (player_ids_.empty()) {
        host_id_ = 0;
        return;
    }
    host_id_ =
        *std::min_element(player_ids_.begin(), player_ids_.end());
}

bool Lobby::can_issue_start(uint8_t player_id) const {
    return !in_progress_ && player_id == host_id_;
}

bool Lobby::begin_match() {
    if (in_progress_ || player_ids_.size() < 2) {
        return false; // need at least one per team
    }
    in_progress_ = true;
    return true;
}

void Lobby::end_match() { in_progress_ = false; }

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> Lobby::assign_teams()
    const {
    std::vector<uint8_t> shuffled = player_ids_;
    std::shuffle(shuffled.begin(), shuffled.end(), rng());

    // Deal alternately: balanced within one for any roster size.
    std::vector<uint8_t> red;
    std::vector<uint8_t> blue;
    for (size_t i = 0; i < shuffled.size(); ++i) {
        (i % 2 == 0 ? red : blue).push_back(shuffled[i]);
    }
    return {red, blue};
}

} // namespace ctf

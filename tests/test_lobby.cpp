#include <doctest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "lobby.h"

using ctf::Lobby;
using ctf::JoinResult;

TEST_CASE("first joiner becomes host") {
    Lobby lobby;
    CHECK(lobby.add_player(0) == JoinResult::Ok);
    CHECK(lobby.host_id() == 0);
}

TEST_CASE("11th join is rejected full and never assigned an id") {
    Lobby lobby;
    for (uint8_t i = 0; i < ctf::config::kMaxPlayers; ++i) {
        CHECK(lobby.add_player(i) == JoinResult::Ok);
    }
    CHECK(lobby.player_ids().size() == ctf::config::kMaxPlayers);

    // The 11th connection: rejected, not added.
    CHECK(lobby.add_player(10) == JoinResult::Full);
    CHECK(lobby.player_ids().size() == ctf::config::kMaxPlayers);
    CHECK(std::find(lobby.player_ids().begin(), lobby.player_ids().end(), 10) ==
          lobby.player_ids().end());
}

TEST_CASE("duplicate join of the same id is rejected") {
    Lobby lobby;
    CHECK(lobby.add_player(3) == JoinResult::Ok);
    CHECK(lobby.add_player(3) == JoinResult::Full); // already present
}

TEST_CASE("host leaving during lobby promotes lowest remaining id") {
    Lobby lobby;
    for (uint8_t i = 0; i < 5; ++i) {
        REQUIRE(lobby.add_player(i) == JoinResult::Ok);
    }
    REQUIRE(lobby.host_id() == 0);

    lobby.remove_player(0);
    CHECK(lobby.host_id() == 1); // lowest remaining

    lobby.remove_player(1);
    CHECK(lobby.host_id() == 2);
}

TEST_CASE("non-host leaving never changes the host") {
    Lobby lobby;
    for (uint8_t i = 0; i < 4; ++i) {
        REQUIRE(lobby.add_player(i) == JoinResult::Ok);
    }
    lobby.remove_player(3);
    lobby.remove_player(2);
    CHECK(lobby.host_id() == 0);
}

TEST_CASE("START_REQUEST accepted only from the host during lobby") {
    Lobby lobby;
    for (uint8_t i = 0; i < 4; ++i) {
        REQUIRE(lobby.add_player(i) == JoinResult::Ok);
    }

    CHECK_FALSE(lobby.can_issue_start(1));
    CHECK_FALSE(lobby.can_issue_start(2));
    CHECK(lobby.can_issue_start(0)); // host
}

TEST_CASE("team assignment is balanced within one player") {
    Lobby lobby;
    for (uint8_t i = 0; i < 7; ++i) {
        REQUIRE(lobby.add_player(i) == JoinResult::Ok);
    }

    auto [red, blue] = lobby.assign_teams();
    REQUIRE(red.size() + blue.size() == 7);
    const size_t diff =
        red.size() > blue.size() ? red.size() - blue.size()
                                 : blue.size() - red.size();
    CHECK(diff <= 1);

    // Every player appears exactly once across both teams.
    std::set<uint8_t> all(red.begin(), red.end());
    all.insert(blue.begin(), blue.end());
    CHECK(all.size() == 7);
}

TEST_CASE("repeated shuffles do not produce identical splits every time") {
    Lobby lobby;
    for (uint8_t i = 0; i < 6; ++i) {
        REQUIRE(lobby.add_player(i) == JoinResult::Ok);
    }

    int distinct = 0;
    std::vector<std::vector<uint8_t>> seen;
    for (int run = 0; run < 20; ++run) {
        auto [red, _] = lobby.assign_teams();
        std::vector<uint8_t> key = red;
        std::sort(key.begin(), key.end());
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            ++distinct;
        }
    }
    // Statistically varied assignment (README §2.4): at least a few distinct
    // splits over 20 runs; deterministic assignment would give exactly 1.
    CHECK(distinct >= 4);
}

TEST_CASE("match lifecycle: lobby -> in-progress -> back to lobby") {
    Lobby lobby;
    for (uint8_t i = 0; i < 4; ++i) {
        REQUIRE(lobby.add_player(i) == JoinResult::Ok);
    }

    REQUIRE(lobby.begin_match());
    CHECK(lobby.in_progress());

    // No new joins mid-match (README §7.5 no-mid-match-joining).
    CHECK(lobby.add_player(9) == JoinResult::InProgress);

    // START_REQUEST meaningless mid-match even from host.
    CHECK_FALSE(lobby.can_issue_start(0));

    // MATCH_END: same roster returns to the lobby.
    lobby.end_match();
    CHECK_FALSE(lobby.in_progress());
    CHECK(lobby.player_ids().size() == 4);
    CHECK(lobby.can_issue_start(0));

    // And a new match can start immediately without reconnecting.
    CHECK(lobby.begin_match());
    CHECK(lobby.in_progress());
}

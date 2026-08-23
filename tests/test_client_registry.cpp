#include <doctest.h>

#include <netinet/in.h>
#include <string>

#include "client_registry.h"

using ctf::ClientEntry;
using ctf::ClientRegistry;

TEST_CASE("add assigns the lowest free player_id and find_by_id returns it") {
    ClientRegistry reg;

    ClientEntry* a = reg.add(100, "bibidh");
    REQUIRE(a != nullptr);
    CHECK(a->tcp_fd == 100);
    CHECK(a->name == "bibidh");

    ClientEntry* found = reg.find_by_id(a->player_id);
    REQUIRE(found != nullptr);
    CHECK(found == a);
}

TEST_CASE("find_by_fd resolves after add") {
    ClientRegistry reg;
    ClientEntry* a = reg.add(42, "nayan");
    REQUIRE(a != nullptr);

    ClientEntry* by_fd = reg.find_by_fd(42);
    REQUIRE(by_fd != nullptr);
    CHECK(by_fd == a);

    // Unknown fd misses cleanly.
    CHECK(reg.find_by_fd(999) == nullptr);
}

TEST_CASE("remove makes lookups fail with no stale pointers") {
    ClientRegistry reg;
    ClientEntry* a = reg.add(10, "one");   // id 0
    ClientEntry* b = reg.add(11, "two");   // id 1
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    reg.remove(a->player_id);
    CHECK(reg.find_by_id(a->player_id) == nullptr);
    CHECK(reg.find_by_id(b->player_id) == b); // survivor untouched

    // Removing again is a clean no-op.
    reg.remove(a->player_id);
    CHECK(reg.find_by_id(a->player_id) == nullptr);
    CHECK(reg.entries().size() == 1);
}

TEST_CASE("ids are reused lowest-first after removal") {
    ClientRegistry reg;
    ClientEntry* a = reg.add(1, "a"); // id 0
    ClientEntry* b = reg.add(2, "b"); // id 1
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    reg.remove(a->player_id);
    ClientEntry* c = reg.add(3, "c");
    REQUIRE(c != nullptr);
    CHECK(c->player_id == a->player_id); // recycled the freed slot
}

TEST_CASE("per-client record carries udp address and session token") {
    ClientRegistry reg;
    ClientEntry* a = reg.add(5, "bot5");
    REQUIRE(a != nullptr);

    a->session_token = 0xFEEDF00D;
    a->has_udp_addr = true;
    a->udp_addr.sin_family = AF_INET;
    a->udp_addr.sin_port = htons(7777);
    a->last_input_tick = 1234;

    ClientEntry* found = reg.find_by_id(a->player_id);
    REQUIRE(found != nullptr);
    CHECK(found->session_token == 0xFEEDF00D);
    CHECK(found->has_udp_addr);
    CHECK(found->udp_addr.sin_port == htons(7777));
    CHECK(found->last_input_tick == 1234);
}

TEST_CASE("registry caps at kMaxPlayers") {
    ClientRegistry reg;
    for (int i = 0; i < ctf::config::kMaxPlayers; ++i) {
        REQUIRE(reg.add(100 + i, "p" + std::to_string(i)) != nullptr);
    }
    CHECK(reg.entries().size() == ctf::config::kMaxPlayers);

    // Full roster: an 11th add must fail (caller maps this to JOIN_REJECT).
    CHECK(reg.add(999, "overflow") == nullptr);

    reg.remove(reg.entries().front().player_id);
    CHECK(reg.entries().size() == ctf::config::kMaxPlayers - 1);
}

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "game_config.h"

// Sanity check that the harness builds, links against shared/, and runs.
TEST_CASE("harness: doctest runs and shared/ is linked") {
    CHECK(ctf::config::kTickRateHz == 30);
    CHECK(ctf::config::kMaxPlayers == 10);
}

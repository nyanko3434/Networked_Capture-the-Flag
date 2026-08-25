#include <doctest.h>

#include "interpolation.h"
#include "render_policy.h"

using namespace ctf;
using namespace ctf::render_policy;

TEST_CASE("render_policy: resolve_local_position honors the F1 toggle") {
    const Vec2Fixed predicted{1000, 2000};
    const Vec2Fixed raw{500, 600};

    CHECK(resolve_local_position(/*prediction_enabled=*/true, predicted, raw).x ==
         predicted.x);
    CHECK(resolve_local_position(true, predicted, raw).y == predicted.y);

    CHECK(resolve_local_position(/*prediction_enabled=*/false, predicted, raw).x ==
         raw.x);
    CHECK(resolve_local_position(false, predicted, raw).y == raw.y);
}

TEST_CASE("render_policy: resolve_remote_state honors the F2 toggle and "
         "falls back to raw when the player isn't in the interpolation "
         "ring yet") {
    WorldSnapshot snap0;
    snap0.tick = 0;
    snap0.player_count = 1;
    snap0.players[0].id = 9;
    snap0.players[0].motion.position = Vec2Fixed{0, 0};

    WorldSnapshot snap1;
    snap1.tick = 1;
    snap1.player_count = 1;
    snap1.players[0].id = 9;
    snap1.players[0].motion.position = Vec2Fixed{1000, 0};

    Interpolation interp;
    interp.push_snapshot(snap0);
    interp.push_snapshot(snap1);
    interp.advance(1.0 / config::kTickRateHz); // move render_tick_ off 0

    PlayerState raw;
    raw.id = 9;
    raw.motion.position = Vec2Fixed{777, 777}; // deliberately different from
                                               // either snapshot, so a test
                                               // failure is obvious either way

    // F2 off: must be exactly `raw`, untouched by interpolation.
    const PlayerState off = resolve_remote_state(false, raw, interp);
    CHECK(off.motion.position.x == 777);
    CHECK(off.motion.position.y == 777);

    // F2 on: must come from Interpolation::sample(), not `raw`.
    const PlayerState on = resolve_remote_state(true, raw, interp);
    CHECK(on.motion.position.x != 777);

    // Fallback: player_id not present in the ring at all.
    PlayerState missing;
    missing.id = 42;
    missing.motion.position = Vec2Fixed{55, 66};
    const PlayerState fallback = resolve_remote_state(true, missing, interp);
    CHECK(fallback.motion.position.x == 55);
    CHECK(fallback.motion.position.y == 66);
}

TEST_CASE("render_policy: resolve_flag_position covers all three flag "
         "states (README §7.4)") {
    const Vec2Fixed base{100, 100};
    const Vec2Fixed dropped_cache{9000, 9000};

    WorldSnapshot latest;
    latest.player_count = 1;
    latest.players[0].id = 3;
    latest.players[0].motion.position = Vec2Fixed{500, 600};

    CHECK(resolve_flag_position(FlagState::AtBase, /*carrier_id=*/0xFF, latest,
                                base, dropped_cache)
             .x == base.x);

    const Vec2Fixed carried =
        resolve_flag_position(FlagState::Carried, /*carrier_id=*/3, latest, base,
                              dropped_cache);
    CHECK(carried.x == 500);
    CHECK(carried.y == 600);

    // Carrier not found in the snapshot (shouldn't happen in practice) -
    // falls back to base rather than garbage.
    const Vec2Fixed carrier_missing =
        resolve_flag_position(FlagState::Carried, /*carrier_id=*/250, latest,
                              base, dropped_cache);
    CHECK(carrier_missing.x == base.x);

    const Vec2Fixed dropped = resolve_flag_position(
        FlagState::Dropped, /*carrier_id=*/0xFF, latest, base, dropped_cache);
    CHECK(dropped.x == dropped_cache.x);
    CHECK(dropped.y == dropped_cache.y);
}

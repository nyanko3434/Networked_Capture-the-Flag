#include <doctest.h>

#include <cmath>

#include "game_config.h"
#include "interpolation.h"

using namespace ctf;

namespace {

WorldSnapshot make_snap(uint32_t tick, uint8_t player_id, int32_t x, int32_t y,
                        uint16_t angle) {
    WorldSnapshot snap;
    snap.tick = tick;
    snap.player_count = 1;
    PlayerState& p = snap.players[0];
    p.id = player_id;
    p.motion.position = Vec2Fixed{x, y};
    p.aim_angle = angle;
    p.health = 100;
    p.alive = true;
    return snap;
}

} // namespace

TEST_CASE("interpolation: angle wraparound takes the short arc, not the "
         "near-full-spin naive path (README §6.4)") {
    Interpolation interp;
    interp.push_snapshot(make_snap(0, 1, 0, 0, 65000));
    interp.push_snapshot(make_snap(1, 1, 0, 0, 500));

    // render_tick_ bootstraps to tick(1) - kInterpolationDelayTicks, which
    // is <= 0 here (clamped to 0) since only 2 ticks exist - force it to the
    // midpoint between the two snapshots directly via advance() so t=0.5
    // regardless of the bootstrap value.
    // Bootstrap after first push_snapshot(tick=0) sets render_tick_=0 (since
    // 0 - 2 clamps to 0). We want render_tick_ = 0.5 (halfway between tick 0
    // and tick 1). Advance by exactly that much of "tick space": dt such
    // that rate * kTickRateHz * dt == 0.5. depth = newest(1) - render(0) = 1
    // > kInterpolationDelayTicks(2)? No, 1 < 2, so rate = 0.98.
    const double dt = 0.5 / (0.98 * config::kTickRateHz);
    interp.advance(dt);
    CHECK(interp.render_tick() == doctest::Approx(0.5).epsilon(0.01));

    const PlayerState sampled = interp.sample(1);

    // Expected via the signed-16-bit-delta formula from README §6.4:
    // delta = (int16)(500 - 65000) = (int16)(uint16)(500-65000) -> short arc.
    const int16_t expected_delta =
        static_cast<int16_t>(static_cast<uint16_t>(500 - 65000));
    double expected = 65000.0 + expected_delta * 0.5;
    expected = std::fmod(expected, 65536.0);
    if (expected < 0) expected += 65536.0;
    const uint16_t expected_angle = static_cast<uint16_t>(expected + 0.5);

    CHECK(sampled.aim_angle == expected_angle);
    // Sanity: the short arc keeps the result near the 65000/500 wrap point
    // (either just under 65536 or just over 0), never near the naive
    // linear-interpolation midpoint (~32750, almost a full spin away).
    const bool near_wrap_point =
        sampled.aim_angle > 65000 || sampled.aim_angle < 3000;
    CHECK(near_wrap_point);
}

TEST_CASE("interpolation: on stall (no new snapshots), the rendered "
         "position holds exactly at the last known value - no "
         "extrapolation") {
    Interpolation interp;
    interp.push_snapshot(make_snap(0, 1, 1000, 2000, 0));
    interp.push_snapshot(make_snap(1, 1, 1100, 2000, 0));
    interp.push_snapshot(make_snap(2, 1, 1200, 2000, 0));

    // Advance far enough that render_tick_ should try to reach/exceed the
    // newest tick.
    for (int i = 0; i < 30; ++i) interp.advance(1.0 / config::kTickRateHz);
    const PlayerState held = interp.sample(1);
    CHECK(held.motion.position.x == 1200);
    CHECK(interp.render_tick() == doctest::Approx(2.0));

    // Keep advancing with no new snapshots arriving - position must not
    // move (would indicate extrapolation past the last real data point).
    for (int i = 0; i < 60; ++i) {
        interp.advance(1.0 / config::kTickRateHz);
        const PlayerState still = interp.sample(1);
        CHECK(still.motion.position.x == 1200);
        CHECK(still.motion.position.y == 2000);
    }
    CHECK(interp.render_tick() == doctest::Approx(2.0)); // clamped, never past newest
}

TEST_CASE("interpolation: drift correction nudges the render_tick advance "
         "rate (1.02x/0.98x) rather than ever jumping it") {
    Interpolation interp;
    // Push many snapshots quickly (tick spacing 1, "instant" wall time) so
    // buffer depth (newest - render_tick_) grows deeper than the 2-tick
    // target, forcing the 1.02x catch-up branch.
    for (uint32_t t = 0; t <= 20; ++t) interp.push_snapshot(make_snap(t, 1, 0, 0, 0));

    // No time has passed yet (only push_snapshot calls), so render_tick_ is
    // still at its bootstrap value: tick(0) - kInterpolationDelayTicks,
    // clamped to 0.
    CHECK(interp.render_tick() == doctest::Approx(0.0));
    const double depth_before = 20.0 - interp.render_tick();
    CHECK(depth_before > config::kInterpolationDelayTicks);

    // One small step: verify the *rate* used is 1.02x, not a hard jump to
    // the target depth. Expected advance = 1.02 * kTickRateHz * dt.
    const double dt = 1.0 / config::kTickRateHz;
    const double before = interp.render_tick();
    interp.advance(dt);
    const double actual_step = interp.render_tick() - before;
    const double expected_step = 1.02 * config::kTickRateHz * dt;
    CHECK(actual_step == doctest::Approx(expected_step).epsilon(0.0001));
    // Explicitly not a jump straight to (newest - target).
    CHECK(interp.render_tick() != doctest::Approx(20.0 - config::kInterpolationDelayTicks));

    // Now drain the buffer down near/at target depth and confirm the rate
    // settles (no longer forced to 1.02x) - advance until depth <= target.
    for (int i = 0; i < 2000 && (20.0 - interp.render_tick()) >
                              config::kInterpolationDelayTicks;
        ++i) {
        interp.advance(dt);
    }
    const double depth_after = 20.0 - interp.render_tick();
    CHECK(depth_after <= config::kInterpolationDelayTicks + 0.01);
}

TEST_CASE("interpolation: bracket sampling lerps position linearly between "
         "the two snapshots straddling render_tick") {
    Interpolation interp;
    interp.push_snapshot(make_snap(0, 1, 0, 0, 0));
    interp.push_snapshot(make_snap(1, 1, 1000, 2000, 0));

    // Force render_tick_ to exactly 0.25 via the same dt-solving trick as
    // the angle test above.
    const double depth = 1.0 - 0.0; // newest(1) - render(0)
    const double rate = depth < config::kInterpolationDelayTicks ? 0.98 : 1.02;
    const double dt = 0.25 / (rate * config::kTickRateHz);
    interp.advance(dt);
    REQUIRE(interp.render_tick() == doctest::Approx(0.25).epsilon(0.01));

    const PlayerState sampled = interp.sample(1);
    CHECK(sampled.motion.position.x == doctest::Approx(250).epsilon(2));
    CHECK(sampled.motion.position.y == doctest::Approx(500).epsilon(2));
}

TEST_CASE("interpolation: reordered/duplicate snapshots (tick <= newest "
         "already held) are dropped, keeping the ring tick-ascending") {
    Interpolation interp;
    interp.push_snapshot(make_snap(5, 1, 100, 0, 0));
    CHECK(interp.snapshot_count() == 1);
    interp.push_snapshot(make_snap(3, 1, 999, 0, 0)); // stale/reordered
    CHECK(interp.snapshot_count() == 1); // dropped, not inserted out of order
    interp.push_snapshot(make_snap(5, 1, 999, 0, 0)); // duplicate tick
    CHECK(interp.snapshot_count() == 1);
    interp.push_snapshot(make_snap(6, 1, 100, 0, 0));
    CHECK(interp.snapshot_count() == 2);
}

TEST_CASE("interpolation: ring never exceeds config::kSnapshotRingSize") {
    Interpolation interp;
    for (uint32_t t = 0; t < 200; ++t) interp.push_snapshot(make_snap(t, 1, 0, 0, 0));
    CHECK(interp.snapshot_count() <= static_cast<size_t>(config::kSnapshotRingSize));
}

TEST_CASE("interpolation: a player absent from both bracketing snapshots "
         "yields a default PlayerState rather than garbage") {
    Interpolation interp;
    interp.push_snapshot(make_snap(0, 1, 0, 0, 0));
    interp.push_snapshot(make_snap(1, 1, 100, 0, 0));
    const PlayerState sampled = interp.sample(/*player_id=*/42); // not present
    CHECK(sampled.id == 0);
}

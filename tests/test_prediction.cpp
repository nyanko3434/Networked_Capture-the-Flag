#include <doctest.h>

#include "game_config.h"
#include "map.h"
#include "movement.h"
#include "prediction.h"
#include "protocol.h"

using namespace ctf;

namespace {

// A safe, wall-free starting position for movement tests (verified against
// the real shared/map.cpp tile grid, not assumed): (0,0) sits on the border
// wall, so tests seed from an open tile instead.
Vec2Fixed safe_start() { return Vec2Fixed{300 * 256, 48 * 256}; }

// Advances `p` for `n` ticks with a fixed input, returning the resulting
// local_state() position each time isn't needed by callers - this just
// drives on_local_tick n times.
void tick_n(Prediction& p, InputCmd cmd, int n) {
    for (int i = 0; i < n; ++i) p.on_local_tick(cmd);
}

WorldSnapshot make_snapshot(uint32_t tick, uint32_t ack, uint8_t my_id,
                            Vec2Fixed auth_position) {
    WorldSnapshot snap;
    snap.tick = tick;
    snap.last_input_seq = ack;
    snap.player_count = 1;
    PlayerState& p = snap.players[0];
    p.id = my_id;
    p.motion.position = auth_position;
    p.motion.velocity = Vec2Fixed{}; // never carried on the wire (README §5.5)
    return snap;
}

} // namespace

TEST_CASE("prediction: on_local_tick assigns strictly increasing seq and "
         "advances local_state_ via movement_step") {
    Prediction p;
    // Prediction has no way to seed a starting position other than the
    // PlayerMotion default {0,0} plus replay, and (0,0) is a wall tile, so
    // this test only checks seq/history bookkeeping, not real displacement -
    // displacement is covered once we seed via on_snapshot() below.
    CHECK(p.current_seq() == 0);
    tick_n(p, InputCmd{kInputRight, 0}, 3);
    CHECK(p.current_seq() == 3);
    REQUIRE(p.history().size() == 3);
    CHECK(p.history()[0].seq == 1);
    CHECK(p.history()[1].seq == 2);
    CHECK(p.history()[2].seq == 3);
}

TEST_CASE("prediction: replay-always is a genuine no-op when the server "
         "agrees with every prediction (README §6.3)") {
    Prediction p;
    const uint8_t my_id = 7;

    // Seed a real, wall-free position via an initial snapshot (ack=0, no
    // history yet to drop/replay).
    p.on_snapshot(make_snapshot(/*tick=*/1, /*ack=*/0, my_id, safe_start()),
                  my_id);
    REQUIRE(p.local_state().position.x == safe_start().x);

    // Predict 5 ticks of rightward movement.
    tick_n(p, InputCmd{kInputRight, 0}, 5);
    const PlayerMotion predicted_before = p.local_state();
    REQUIRE(predicted_before.position.x > safe_start().x); // actually moved

    // Server agrees exactly with what we predicted through seq 5: feed an
    // authoritative snapshot whose position matches history[4].state_after
    // (the predicted state at seq 5) precisely.
    const Vec2Fixed auth_matching_prediction = p.history().back().state_after.position;
    p.on_snapshot(make_snapshot(/*tick=*/2, /*ack=*/5, my_id,
                                auth_matching_prediction),
                  my_id);

    // Zero mispredictions, and the final local_state is unchanged from
    // before reconciliation - replay reproduced exactly the same state.
    CHECK(p.mispredictions() == 0);
    CHECK(p.local_state().position.x == predicted_before.position.x);
    CHECK(p.local_state().position.y == predicted_before.position.y);
    // Everything through seq 5 was acked and dropped; nothing left unacked.
    CHECK(p.history().empty());
}

TEST_CASE("prediction: a genuine misprediction is detected and corrected - "
         "snap to authority then replay remaining unacked inputs") {
    Prediction p;
    const uint8_t my_id = 3;
    p.on_snapshot(make_snapshot(1, 0, my_id, safe_start()), my_id);

    // Predict 4 ticks right.
    tick_n(p, InputCmd{kInputRight, 0}, 4);
    REQUIRE(p.history().size() == 4);

    // Server disagrees with what we predicted for seq 2: pretend a
    // misprediction happened by ack-ing seq 2 with a position that does NOT
    // match history[seq==2].state_after (simulate e.g. a dropped input the
    // server applied as zero-movement, per README §6.1).
    const Vec2Fixed wrong_predicted = p.history()[1].state_after.position; // seq 2
    const Vec2Fixed authoritative_seq2{wrong_predicted.x - 500, wrong_predicted.y};
    REQUIRE(authoritative_seq2.x != wrong_predicted.x);

    p.on_snapshot(make_snapshot(2, /*ack=*/2, my_id, authoritative_seq2), my_id);

    CHECK(p.mispredictions() == 1);
    // Seqs 1-2 dropped; 3-4 remain and were replayed on top of the new
    // authoritative base.
    REQUIRE(p.history().size() == 2);
    CHECK(p.history()[0].seq == 3);
    CHECK(p.history()[1].seq == 4);

    // Manually replay 3 and 4 from authoritative_seq2 to compute the
    // expected corrected state, and confirm Prediction matches it exactly -
    // "snap to authority, then replay remaining unacked inputs", not the
    // old (wrong) predicted state.
    Map map;
    PlayerMotion expected{authoritative_seq2, Vec2Fixed{0, 0}};
    expected = movement_step(expected, p.history()[0].input, map);
    expected = movement_step(expected, p.history()[1].input, map);
    CHECK(p.local_state().position.x == expected.position.x);
    CHECK(p.local_state().position.y == expected.position.y);
    // And explicitly NOT the stale, wrong prediction from before correction.
    CHECK(p.local_state().position.x != wrong_predicted.x);
}

TEST_CASE("prediction: stale snapshots (tick <= last_applied_tick) are "
         "dropped - no state change, ack stays put") {
    Prediction p;
    const uint8_t my_id = 1;
    p.on_snapshot(make_snapshot(10, 0, my_id, safe_start()), my_id);
    tick_n(p, InputCmd{kInputRight, 0}, 2);
    const PlayerMotion before = p.local_state();
    const size_t history_before = p.history().size();
    const uint32_t mispredictions_before = p.mispredictions();

    // Out-of-order: tick=8 arrives after tick=10 already applied.
    Vec2Fixed some_other_position{safe_start().x + 99999, safe_start().y};
    p.on_snapshot(make_snapshot(/*tick=*/8, /*ack=*/1, my_id,
                                some_other_position),
                  my_id);

    CHECK(p.local_state().position.x == before.position.x);
    CHECK(p.local_state().position.y == before.position.y);
    CHECK(p.history().size() == history_before);
    CHECK(p.mispredictions() == mispredictions_before);
}

TEST_CASE("prediction: history is trimmed on ack - no entry with seq <= ack "
         "survives, preventing stale replay (README §6.7 item 5)") {
    Prediction p;
    const uint8_t my_id = 2;
    p.on_snapshot(make_snapshot(1, 0, my_id, safe_start()), my_id);
    tick_n(p, InputCmd{kInputRight, 0}, 10);
    REQUIRE(p.history().size() == 10);

    const Vec2Fixed auth = p.history()[5].state_after.position; // seq 6
    p.on_snapshot(make_snapshot(2, /*ack=*/6, my_id, auth), my_id);

    for (const auto& e : p.history()) {
        CHECK(e.seq > 6);
    }
    CHECK(p.history().size() == 4); // seqs 7,8,9,10
}

TEST_CASE("prediction: history never exceeds the safety cap even with many "
         "un-acked ticks (README §6.3 - 'a 64-entry ring is ample')") {
    Prediction p;
    tick_n(p, InputCmd{kInputRight, 0}, 200);
    CHECK(p.history().size() <= 64);
    // Newest entries survive; oldest are the ones dropped.
    CHECK(p.history().back().seq == 200);
}

TEST_CASE("prediction: a snapshot that doesn't include our player_id is "
         "ignored without touching state") {
    Prediction p;
    const uint8_t my_id = 5;
    p.on_snapshot(make_snapshot(1, 0, my_id, safe_start()), my_id);
    tick_n(p, InputCmd{kInputRight, 0}, 2);
    const PlayerMotion before = p.local_state();

    WorldSnapshot snap = make_snapshot(2, 1, /*different id=*/9, safe_start());
    p.on_snapshot(snap, my_id);

    CHECK(p.local_state().position.x == before.position.x);
    CHECK(p.local_state().position.y == before.position.y);
}

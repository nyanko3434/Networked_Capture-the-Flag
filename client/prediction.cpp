#include "prediction.h"

#include <algorithm>

#include "game_config.h"
#include "movement.h"

// Prediction and reconciliation for the local player only (README §6.2,
// §6.3, §6.5). Own movement only - never shooting, hits, flag pickup, or
// death (README §6.5); that instruction is enforced by construction here
// simply by never touching anything but position/velocity via
// movement_step.

namespace ctf {

namespace {
// "A 64-entry ring is ample" (README §6.3). `history` never exceeds
// RTT * 30 in practice (2-4 entries on a LAN); config::kInputHistoryRingSize
// is the safety cap, not the expected steady-state size.
} // namespace

Prediction::Prediction() = default;

void Prediction::on_local_tick(const InputCmd& cmd) {
    ++seq_;

    local_state_ = movement_step(local_state_, cmd, map_);

    InputHistoryEntry entry;
    entry.seq = seq_;
    entry.input = cmd;
    entry.state_after = local_state_; // debug HUD only (README §6.2)
    history_.push_back(entry);

    // Bound the ring even though steady-state is 2-4 entries on a LAN
    // (README §6.3) - a stalled connection must not grow this unbounded.
    if (history_.size() > static_cast<size_t>(config::kInputHistoryRingSize)) {
        history_.erase(history_.begin(),
                       history_.begin() +
                           static_cast<long>(history_.size() -
                                             config::kInputHistoryRingSize));
    }
}

void Prediction::on_snapshot(const WorldSnapshot& snap, uint8_t my_player_id) {
    // UDP reorders - drop stale snapshots (README §6.3, desync checklist
    // item 4). This is the one place that check is made; net_client.cpp
    // deliberately does not make it (see the ownership note in
    // net_client.h).
    if (snap.tick <= last_applied_tick_) return;
    last_applied_tick_ = snap.tick;

    const PlayerState* auth_ps = nullptr;
    for (uint8_t i = 0; i < snap.player_count; ++i) {
        if (snap.players[i].id == my_player_id) {
            auth_ps = &snap.players[i];
            break;
        }
    }
    // We're not in this snapshot yet (e.g. GAME_START hasn't landed) -
    // nothing to reconcile against.
    if (auth_ps == nullptr) return;

    const uint32_t ack = snap.last_input_seq;
    const PlayerMotion auth = auth_ps->motion;

    // Instrumentation only (README §6.3): compare what we predicted for
    // the acked seq against what the server actually computed, purely as
    // a HUD/misprediction counter - never a branch. Velocity is never
    // compared: WorldSnapshot never carries it over the wire (decode
    // always zeroes PlayerState::motion.velocity, since movement_step
    // derives velocity from input alone, never from a stored value), so
    // an "authoritative velocity" doesn't exist to compare against.
    for (const auto& e : history_) {
        if (e.seq == ack) {
            if (e.state_after.position.x != auth.position.x ||
                e.state_after.position.y != auth.position.y) {
                ++mispredictions_;
            }
            break;
        }
    }

    // Drop history through the acked seq (README §6.3, desync checklist
    // item 5 - stale entries left in history re-apply old inputs on replay).
    history_.erase(std::remove_if(history_.begin(), history_.end(),
                                  [ack](const InputHistoryEntry& e) {
                                      return e.seq <= ack;
                                  }),
                   history_.end());

    // Snap to authority, then ALWAYS replay every remaining unacked entry -
    // no branch on whether a misprediction occurred (README §6.3's explicit
    // instruction: replay is a no-op when prediction was correct, and
    // skipping it "adds a branch for bugs to hide in").
    local_state_.position = auth.position;
    local_state_.velocity = Vec2Fixed{0, 0}; // recomputed by the first
                                             // replayed movement_step call
    for (auto& e : history_) {
        local_state_ = movement_step(local_state_, e.input, map_);
        e.state_after = local_state_; // debug HUD only
    }
}

} // namespace ctf

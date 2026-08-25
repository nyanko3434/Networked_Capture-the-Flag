#pragma once

// Prediction and reconciliation for the local player only (README §6,
// §6.2, §6.3). Remote players are never predicted; the local player is
// never interpolated.

#include <cstdint>

#include "game_types.h"
#include "map.h"
#include "ring_buffer.h"

namespace ctf {

// One entry of the client's input history (README §6.2). state_after is
// debug-HUD only.
struct InputHistoryEntry {
    uint32_t seq = 0;
    InputCmd input;
    PlayerMotion state_after;
};

class Prediction {
public:
    Prediction();

    // Runs one local tick given an already-sampled input (input sampling
    // itself is a platform concern - raylib for the real client, synthetic
    // for the bot - and lives one layer up): assigns the next seq, appends
    // to history, and advances local_state_ through movement_step (README
    // §6.2). Sending the resulting PLAYER_INPUT packet is the caller's job
    // (see current_seq()/history() below) so this class has no socket
    // dependency.
    void on_local_tick(const InputCmd& cmd);

    // Reconciles against an authoritative snapshot (README §6.3): drops
    // acked history, snaps to authority, and always replays the remainder.
    void on_snapshot(const WorldSnapshot& snap, uint8_t my_player_id);

    const PlayerMotion& local_state() const { return local_state_; }
    uint32_t mispredictions() const { return mispredictions_; }

    // For the caller to build the PLAYER_INPUT packet (README §5.4/§6.2):
    // the current seq and the input history in ascending-seq order, from
    // which it takes the last config::kInputRedundancy entries. Kept as a
    // caller responsibility (rather than Prediction owning a NetClient)
    // so this class stays networking-free and independently testable.
    uint32_t current_seq() const { return seq_; }
    const RingBuffer<InputHistoryEntry, config::kInputHistoryRingSize>& history() const { return history_; }

private:
    uint32_t seq_ = 0;
    uint32_t last_applied_tick_ = 0;
    PlayerMotion local_state_;
    RingBuffer<InputHistoryEntry, config::kInputHistoryRingSize> history_;
    uint32_t mispredictions_ = 0;
    Map map_; // stateless tile grid (README §7.1) - movement_step needs one
};

} // namespace ctf

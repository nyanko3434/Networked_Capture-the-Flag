#pragma once

// The ONLY place mutexes appear (README §4 structural rule 1, §3.2). Two
// queues, no shared game state: the network thread and simulation thread
// communicate exclusively through InboundQueue and OutboundQueue.
//
// Lock discipline (README §3.2), enforced by keeping every lock inside this
// file:
//   1. Never hold a lock across a syscall.
//   2. Never hold both locks at once.
//   3. Lock only inside the queue implementation.
//
// MutexGuard is the ~8-line RAII wrapper so an early return cannot leak a
// lock, while still satisfying the raw-pthreads requirement.

#include <array>
#include <cstdint>
#include <deque>
#include <pthread.h>

#include "game_types.h"

namespace ctf {

class MutexGuard {
public:
    explicit MutexGuard(pthread_mutex_t& mutex);
    ~MutexGuard();

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    pthread_mutex_t& mutex_;
};

enum class InboundCommandType : uint8_t {
    PlayerJoined,
    PlayerLeft,
    PlayerInput,
};

// Commands flowing network thread -> sim thread (README §3.2: join/leave
// are commands, not direct mutations).
//
// - PlayerJoined: player_id, team, spawn_index (from the lobby's GAME_START
//   team assignment, applied by the network thread).
// - PlayerLeft: player_id.
// - PlayerInput: player_id + one decoded input with its wire seq. The
//   network thread pushes one command per redundant input entry; the sim's
//   per-player ring buffer ignores seqs it has already queued.
struct InboundCommand {
    InboundCommandType type = InboundCommandType::PlayerJoined;
    uint8_t player_id = 0;
    Team team = Team::Red;
    uint8_t spawn_index = 0;
    uint32_t seq = 0;
    InputCmd input;
};

enum class OutboundEventType : uint8_t {
    TcpBroadcast,
    UdpSnapshot,      // full WORLD_SNAPSHOT body (keyframe)
    UdpDeltaSnapshot, // DELTA_SNAPSHOT body vs the previous publish
    UdpEvent, // cosmetic SHOT_FIRED tracers (loss-tolerant)
};

// Snapshots/events flowing sim thread -> network thread.
//
// UdpSnapshot carries the serialized WORLD_SNAPSHOT body (payload) plus the
// per-player last_input_seq table so broadcast can patch 4 bytes per
// recipient (README §5.5).
//
// TcpBroadcast/UdpEvent payloads are [u8 type][encoded fields]; the network
// side prepends the u16 TCP length when framing.
//
// Payload uses a fixed-size array to avoid heap allocation per event.
// Snapshots are ~128 bytes, events are ~20 bytes; 512 is ample.
struct OutboundEvent {
    OutboundEventType type = OutboundEventType::TcpBroadcast;
    std::array<uint8_t, 512> payload_data{};
    uint16_t payload_size = 0;
    uint32_t acks[config::kMaxPlayers] = {};
    uint32_t tick = 0; // UdpSnapshot/UdpEvent transport header

    // Convenience accessors for compatibility.
    const uint8_t* payload_ptr() const { return payload_data.data(); }
    uint8_t* payload_ptr() { return payload_data.data(); }
    size_t payload_len() const { return payload_size; }
    bool payload_empty() const { return payload_size == 0; }
};

class InboundQueue {
public:
    void push(const InboundCommand& cmd);
    bool pop(InboundCommand& out);

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::deque<InboundCommand> items_;
};

class OutboundQueue {
public:
    void push(const OutboundEvent& event);
    bool pop(OutboundEvent& out);

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::deque<OutboundEvent> items_;
};

} // namespace ctf

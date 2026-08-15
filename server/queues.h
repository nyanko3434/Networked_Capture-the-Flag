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

#include <cstdint>
#include <pthread.h>
#include <vector>

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
struct InboundCommand {
    InboundCommandType type = InboundCommandType::PlayerJoined;
    uint8_t player_id = 0;
    InputCmd input;
};

enum class OutboundEventType : uint8_t {
    TcpBroadcast,
    UdpSnapshot,
};

// Snapshots/events flowing sim thread -> network thread.
struct OutboundEvent {
    OutboundEventType type = OutboundEventType::TcpBroadcast;
    std::vector<uint8_t> payload;
};

class InboundQueue {
public:
    void push(const InboundCommand& cmd);
    bool pop(InboundCommand& out);

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::vector<InboundCommand> items_;
};

class OutboundQueue {
public:
    void push(const OutboundEvent& event);
    bool pop(OutboundEvent& out);

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::vector<OutboundEvent> items_;
};

} // namespace ctf

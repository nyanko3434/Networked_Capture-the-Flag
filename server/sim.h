#pragma once

// Simulation thread (README §3.2, §4). Owns all game state — positions,
// health, flags, scores, active roster. Never touches a socket.
//
// Fixed tick order: drain inbound -> apply commands -> pop one input per
// player -> movement -> combat -> flags -> win check -> publish snapshot
// and events.

#include "game_types.h"
#include "map.h"
#include "queues.h"

namespace ctf {

class Sim {
public:
    Sim(InboundQueue& inbound, OutboundQueue& outbound);

    // Runs the 30 Hz clock_nanosleep(TIMER_ABSTIME) tick loop (README §3.2)
    // until stop() is called.
    void run();
    void stop();

private:
    void tick();

    InboundQueue& inbound_;
    OutboundQueue& outbound_;
    Map map_;
    WorldSnapshot snapshot_;
    bool running_ = false;
};

} // namespace ctf

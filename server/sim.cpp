#include "sim.h"

// Scaffolding only — the real tick loop (README §3.2) is written together
// in week 2 when the sim is split into its own pthread (README §11).

namespace ctf {

Sim::Sim(InboundQueue& inbound, OutboundQueue& outbound)
    : inbound_(inbound), outbound_(outbound) {}

void Sim::run() {}

void Sim::stop() { running_ = false; }

void Sim::tick() {}

} // namespace ctf

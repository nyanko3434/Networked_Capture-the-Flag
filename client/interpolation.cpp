#include "interpolation.h"

// Scaffolding only — real interpolation bodies are written together in
// week 3 (README §11).

namespace ctf {

Interpolation::Interpolation() = default;

void Interpolation::push_snapshot(const WorldSnapshot&) {}

void Interpolation::advance(double) {}

PlayerState Interpolation::sample(uint8_t) const { return PlayerState{}; }

} // namespace ctf

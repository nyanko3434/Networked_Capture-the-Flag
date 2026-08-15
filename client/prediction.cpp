#include "prediction.h"

// Scaffolding only — real prediction/reconciliation bodies are written
// together in week 3 (README §11), built on top of shared/movement.cpp.

namespace ctf {

Prediction::Prediction() = default;

void Prediction::on_local_tick(const InputCmd&) {}

void Prediction::on_snapshot(const WorldSnapshot&, uint8_t) {}

} // namespace ctf

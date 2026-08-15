#pragma once

// THE physics step (README §7.2). Must never fork between client and
// server — any divergence, including from different compiler optimization
// flags, makes reconciliation fight itself. Integer-only, deterministic.

#include <cstdint>

#include "game_types.h"
#include "map.h"

namespace ctf {

// Fixed-point multiply/divide helpers (README §5.1), operating on the
// 1/256 px internal representation (config::kFixedShift).
int32_t fp_mul(int32_t a, int32_t b);
int32_t fp_div(int32_t a, int32_t b);

// Position, velocity, and input in; position and velocity out. No weapon
// state, no health, no team, no timers (README §7.2). Must be unit-testable
// as deterministic: 10,000 random inputs run twice must be byte-identical.
PlayerMotion movement_step(const PlayerMotion& in, const InputCmd& cmd, const Map& map);

} // namespace ctf

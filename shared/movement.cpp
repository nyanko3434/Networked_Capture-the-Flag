#include "movement.h"

// Scaffolding only — the real movement step (diagonal normalization, AABB
// collision resolved X-then-Y) is written together in the shared/ pairing
// session (README §11 week 1). The stub below is a deterministic no-op: it
// returns the input motion unchanged.

namespace ctf {

int32_t fp_mul(int32_t, int32_t) { return 0; }
int32_t fp_div(int32_t, int32_t) { return 0; }

PlayerMotion movement_step(const PlayerMotion& in, const InputCmd&, const Map&) {
    return in;
}

} // namespace ctf

#include "render.h"

// Scaffolding only — real raylib drawing is written together in week 1
// (README §11). The include below proves the raylib dependency resolves at
// compile time without opening a window; ctf_client's stub main() never
// calls into this class yet.
#include <raylib.h>

namespace ctf {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::init(int, int, const char*) { return false; }

void Renderer::draw_frame(const WorldSnapshot&) {}

bool Renderer::should_close() const { return true; }

void Renderer::shutdown() {}

} // namespace ctf

#include "lobby.h"

// Scaffolding only — real lobby bodies are written together in week 1
// (README §11).

namespace ctf {

void Lobby::add_player(uint8_t) {}

void Lobby::remove_player(uint8_t) {}

uint8_t Lobby::host_id() const { return host_id_; }

bool Lobby::can_start() const { return false; }

} // namespace ctf

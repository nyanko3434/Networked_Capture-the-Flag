#include "client_registry.h"

// Scaffolding only — real registry bodies (README §3.2) are written when
// the server's network thread is implemented (README §11 week 1-2).

namespace ctf {

ClientEntry* ClientRegistry::add(int, const std::string&) { return nullptr; }

void ClientRegistry::remove(uint8_t) {}

ClientEntry* ClientRegistry::find_by_id(uint8_t) { return nullptr; }

ClientEntry* ClientRegistry::find_by_fd(int) { return nullptr; }

} // namespace ctf

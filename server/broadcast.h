#pragma once

// Outbound send helpers used by the network thread (README §3.2 "Outbound
// sends"). UDP snapshots are patched per-recipient (4 bytes: last_input_seq)
// then sent with one sendto() each; TCP events go through each client's
// pending output buffer, POLLOUT registered only while it is non-empty.

#include <cstdint>
#include <vector>

#include "client_registry.h"

namespace ctf {

// Sends a pre-serialized TCP event payload to every registered client,
// appending to each client's pending output buffer.
void broadcast_tcp_event(ClientRegistry& registry, const std::vector<uint8_t>& payload);

// Patches last_input_seq into `body` per recipient and issues one sendto
// per client (README §5.5).
void send_udp_snapshots(ClientRegistry& registry, const std::vector<uint8_t>& body);

} // namespace ctf

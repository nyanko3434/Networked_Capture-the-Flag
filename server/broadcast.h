#pragma once

// Outbound send helpers used by the network thread (README §3.2 "Outbound
// sends"). UDP snapshots are patched per-recipient (4 bytes: last_input_seq)
// then sent with one sendto() each; TCP events go into each client's pending
// output buffer, with POLLOUT managed by net_server while it is non-empty.
//
// Runs on the network thread only: touches the registry and sockets, never
// a mutex, never game state.

#include <cstdint>
#include <vector>

#include "client_registry.h"

namespace ctf {

// Appends a pre-serialized [type][payload] TCP event to every connected
// client's pending output buffer (fan-out). Returns how many clients the
// event reached.
int broadcast_tcp_event(ClientRegistry& registry,
                        const std::vector<uint8_t>& payload);

// Wraps a serialized snapshot body (full WORLD_SNAPSHOT or DELTA_SNAPSHOT;
// both start with the per-recipient last_input_seq at payload offset 0) in
// the 8-byte UDP header, patches the ack per recipient, and issues one
// sendto per client with a registered UDP address. `udp_msg_type` is the
// transport-header type byte (README §5.5).
void send_udp_snapshots(ClientRegistry& registry, int udp_fd,
                        const uint8_t* body, size_t body_len,
                        const uint32_t* acks, uint32_t tick,
                        uint8_t udp_msg_type = 9 /* WorldSnapshot */);

} // namespace ctf

#include "net_server.h"

// Scaffolding only — real accept/framing/recv/send bodies are written
// together in week 1 (README §11).

namespace ctf {

NetServer::NetServer(uint16_t port, InboundQueue& inbound, OutboundQueue& outbound,
                      std::unique_ptr<IPoller> poller)
    : port_(port), inbound_(inbound), outbound_(outbound), poller_(std::move(poller)) {}

NetServer::~NetServer() = default;

bool NetServer::start() { return false; }

void NetServer::stop() { running_ = false; }

void NetServer::run() {}

} // namespace ctf

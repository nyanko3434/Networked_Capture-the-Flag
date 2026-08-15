#pragma once

// Network thread (README §3.2, §4): owns every socket, the client registry,
// and all socket buffers. Never reads a player position. Accepts TCP
// connections, handles TCP framing, and does UDP recv/send, driven by an
// IPoller.
//
// The network thread blocks in poll()/epoll(); after the sim thread
// publishes a snapshot it writes 8 bytes to an eventfd registered in the
// poll set so the network thread wakes immediately (README §3.2).

#include <cstdint>
#include <memory>

#include "client_registry.h"
#include "poller.h"
#include "queues.h"

namespace ctf {

class NetServer {
public:
    NetServer(uint16_t port, InboundQueue& inbound, OutboundQueue& outbound,
              std::unique_ptr<IPoller> poller);
    ~NetServer();

    bool start();
    void stop();

    // Runs the poll/accept/recv/send loop until stop() is called.
    void run();

private:
    uint16_t port_;
    InboundQueue& inbound_;
    OutboundQueue& outbound_;
    std::unique_ptr<IPoller> poller_;
    ClientRegistry registry_;

    int tcp_listen_fd_ = -1;
    int udp_fd_ = -1;
    int event_fd_ = -1;
    bool running_ = false;
};

} // namespace ctf

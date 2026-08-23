#pragma once

// Network thread (README §3.2, §4): owns every socket, the client registry,
// and all socket buffers. Never reads a player position. Accepts TCP
// connections, handles TCP framing, and does UDP recv/send, driven by an
// IPoller.
//
// The network thread blocks in poll()/epoll(); after the sim thread
// publishes a snapshot it writes 8 bytes to an eventfd registered in the
// poll set so the network thread wakes immediately (README §3.2).
//
// Testability: port 0 picks an ephemeral port (see local_port()); the poll
// timeout and UDP-silence threshold are injectable so disconnect paths can
// be exercised in milliseconds instead of seconds.

#include <cstdint>
#include <map>
#include <memory>

#include "client_registry.h"
#include "lobby.h"
#include "poller.h"
#include "protocol.h"
#include "queues.h"

namespace ctf {

namespace protocol = ::ctf::protocol;

class NetServer {
public:
    NetServer(uint16_t port, InboundQueue& inbound, OutboundQueue& outbound,
              std::unique_ptr<IPoller> poller,
              int poll_timeout_ms = 100,
              int udp_silence_ms = config::kUdpSilenceTimeoutSec * 1000);
    ~NetServer();

    bool start();   // bind/listen TCP + UDP, create + register eventfd
    void stop();
    void run();     // poll/accept/recv/send loop until stop()

    // --- test/main accessors ---
    ClientRegistry& registry() { return registry_; }
    uint64_t udp_snapshots_sent() const { return udp_snapshots_sent_; }
    uint64_t tcp_events_fanned_out() const { return tcp_events_fanned_out_; }
    uint16_t local_port() const { return local_port_; }
    uint16_t udp_port() const { return udp_port_; }
    int wake_fd() const { return event_fd_; } // sim writes 8 bytes here

private:
    void accept_new_clients();
    std::vector<uint8_t>& accum_for(int fd);
    void handle_tcp_readable(int fd);
    void handle_udp_readable();
    void handle_wake();
    void flush_pending(int fd);
    void enforce_pending_cap();
    void check_silence_timeouts();

    void disconnect(ClientEntry& entry, bool notify_sim);

    // TCP frame handlers
    void on_join_lobby(int fd, const protocol::MsgJoinLobby& msg);
    void on_start_request(int fd, const protocol::MsgStartRequest& msg);
    void begin_match_and_notify();
    void broadcast_lobby_state();

    // UDP packet handlers
    ClientEntry* auth_udp_sender(const sockaddr_in& src, uint8_t player_id,
                                 uint32_t token);
    void on_udp_packet(const sockaddr_in& src, const uint8_t* data, size_t len);
    void on_udp_hello(const sockaddr_in& src, uint32_t tick,
                      const protocol::MsgUdpHello& msg);
    void on_player_input(const sockaddr_in& src, uint32_t tick,
                         const protocol::MsgPlayerInput& msg);

    uint32_t now_ms() const;
    uint32_t make_token();

    uint16_t port_;
    InboundQueue& inbound_;
    OutboundQueue& outbound_;
    std::unique_ptr<IPoller> poller_;
    ClientRegistry registry_;
    Lobby lobby_;

    int tcp_listen_fd_ = -1;
    int udp_fd_ = -1;
    int event_fd_ = -1;
    uint16_t local_port_ = 0;
    uint16_t udp_port_ = 0;
    bool running_ = false;

    const int poll_timeout_ms_;
    const int udp_silence_ms_;

    // Per-connection TCP accumulation buffers (README §5.3 framing;
    // capped at kTcpFrameCapBytes).
    std::vector<uint8_t> tcp_in_[config::kMaxPlayers];
    std::map<int, std::vector<uint8_t>> pre_join_; // fd -> bytes, pre-join

    uint32_t last_silence_check_ms_ = 0;
    uint64_t udp_snapshots_sent_ = 0;
    uint64_t tcp_events_fanned_out_ = 0;
};

} // namespace ctf

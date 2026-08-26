#pragma once

// Client-side networking: TCP handshake (README §5.6) plus the UDP input/
// snapshot stream. Shared verbatim with the bot (via client_core), minus
// rendering.
//
// Ownership note (implementation_guide.md §3.1 checklist item 5): this file
// does NOT drop stale WORLD_SNAPSHOTs (snap.tick <= last_applied_tick). Every
// snapshot that decodes successfully is handed off in arrival order via
// take_snapshots(). The staleness check is owned by Prediction::on_snapshot
// (README §6.3's pseudocode performs the check as the first line of
// reconciliation), so there is exactly one place that decision is made.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "game_types.h"
#include "protocol.h"

namespace ctf {

// Handshake progress (README §5.6 steps 1-5), exposed so callers/tests can
// assert on state rather than only scraping log lines.
enum class NetClientState {
    Disconnected,
    WaitingJoinAccept, // TCP connected, JOIN_LOBBY sent
    Rejected,          // JOIN_REJECT received - terminal
    InLobby,           // JOIN_ACCEPT received, UDP_HELLO resend loop active
    InGame,            // GAME_START received
};

// TCP-delivered one-time events (README §5.4) plus the cosmetic UDP
// SHOT_FIRED, surfaced together so render.cpp has one queue to drain.
// LobbyState/GameStart are included here too, in addition to the dedicated
// lobby_state()/game_start() accessors, so a caller that only wants to react
// to *changes* doesn't have to poll the accessors every frame.
using GameEvent = std::variant<protocol::MsgLobbyState, protocol::MsgGameStart,
                               protocol::MsgPlayerKilled,
                               protocol::MsgPlayerRespawned,
                               protocol::MsgFlagPickedUp,
                               protocol::MsgFlagDropped,
                               protocol::MsgFlagReturned,
                               protocol::MsgFlagCaptured,
                               protocol::MsgMatchEnd, protocol::MsgShotFired>;

class NetClient {
public:
    NetClient();
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Connects TCP, sends JOIN_LOBBY, and blocks (bounded by timeout_ms)
    // waiting for JOIN_ACCEPT or JOIN_REJECT (README §5.6 steps 1-2). On
    // JOIN_ACCEPT: stores player_id/session_token, resolves the server's UDP
    // endpoint from the accept payload's udp_port (never hardcoded - the
    // kernel-assigned UDP port is unrelated to the TCP port, README §5.6),
    // and returns true. On JOIN_REJECT or timeout, returns false; call
    // join_reject_reason() to see why (only meaningful after a JOIN_REJECT).
    bool connect_and_join(const std::string& host, uint16_t port,
                          const std::string& name, int timeout_ms = 5000);

    // Sends one UDP_HELLO datagram immediately. poll() also resends this
    // automatically every config::kUdpHelloResendMs while
    // first_snapshot_received() is still false (README §5.6 - "the hello can
    // be lost"). Exposed publicly too so a caller/test can force a send.
    void send_udp_hello();

    // Sends up to config::kInputRedundancy inputs in one PLAYER_INPUT packet
    // (README §5.4). `count` is clamped to config::kInputRedundancy.
    void send_input(uint32_t base_seq, const InputCmd* inputs, uint8_t count);

    // Sends START_REQUEST (README §5.6 step 5). Only meaningful if this
    // client is the host (see lobby_state().host_id) - the server enforces
    // that, this call does not check locally.
    void send_start_request();

    // Pumps both sockets without blocking: drains all currently-available
    // TCP frames and UDP datagrams, dispatches WORLD_SNAPSHOTs into the
    // snapshot queue and everything else into the event queue, drives the
    // UDP_HELLO resend timer, and sends HEARTBEAT over TCP every
    // config::kHeartbeatIntervalSec (README §5.4). Call this once per client
    // tick (README §6.2).
    void poll();

    // Drains and returns every WORLD_SNAPSHOT decoded since the last call,
    // in arrival order. Not filtered for staleness - see the ownership note
    // above.
    std::vector<WorldSnapshot> take_snapshots();

    // Drains and returns every TCP/cosmetic-UDP event decoded since the last
    // call, in arrival order.
    std::vector<GameEvent> take_events();

    NetClientState state() const { return state_; }
    bool connected() const { return tcp_fd_ >= 0; }
    bool first_snapshot_received() const { return first_snapshot_received_; }
    bool game_started() const { return state_ == NetClientState::InGame; }

    uint8_t player_id() const { return player_id_; }
    uint32_t session_token() const { return session_token_; }
    uint16_t server_udp_port() const { return server_udp_port_; }
    protocol::JoinRejectReason join_reject_reason() const {
        return join_reject_reason_;
    }

    const protocol::MsgLobbyState& lobby_state() const { return lobby_state_; }
    const protocol::MsgGameStart& game_start() const { return game_start_; }
    bool is_host() const {
        return state_ != NetClientState::Disconnected &&
              state_ != NetClientState::Rejected &&
              player_id_ == lobby_state_.host_id;
    }

    // Closes both sockets. Safe to call multiple times.
    void disconnect();

private:
    void log(const char* fmt, ...) const;
    bool wait_for_tcp_frame(int timeout_ms, uint8_t& out_type,
                            std::vector<uint8_t>& out_payload);
    void drain_tcp();
    void drain_udp();
    void dispatch_tcp_frame(uint8_t type, const uint8_t* payload, size_t len);
    // `tick` is the transport-header tick (README §5.3) — DELTA_SNAPSHOTs
    // need it because the delta body itself carries no tick field.
    void dispatch_udp_payload(protocol::MessageType type, uint32_t tick,
                              const uint8_t* payload, size_t len);
    void send_tcp_frame(protocol::MessageType type, const uint8_t* payload,
                        size_t len);
    void maybe_resend_udp_hello();
    void maybe_send_heartbeat();

    int tcp_fd_ = -1;
    int udp_fd_ = -1;
    uint32_t server_ipv4_be_ = 0; // network-byte-order IPv4, resolved once
    uint16_t server_udp_port_ = 0;

    NetClientState state_ = NetClientState::Disconnected;
    uint8_t player_id_ = 0;
    uint32_t session_token_ = 0;
    protocol::JoinRejectReason join_reject_reason_ = protocol::JoinRejectReason::Full;

    protocol::MsgLobbyState lobby_state_;
    protocol::MsgGameStart game_start_;

    bool first_snapshot_received_ = false;
    // Cache of the last applied snapshot (full or reconstructed delta).
    // DELTA_SNAPSHOTs are decoded against it; on a baseline mismatch
    // (packet loss) the cache is invalidated and updates stall until the
    // next full keyframe.
    WorldSnapshot last_snapshot_cache_{};
    bool have_last_snapshot_ = false;
    uint32_t last_udp_hello_sent_ms_ = 0;
    uint32_t last_heartbeat_sent_ms_ = 0;

    std::vector<uint8_t> tcp_rx_buf_;
    std::vector<WorldSnapshot> pending_snapshots_;
    std::vector<GameEvent> pending_events_;
};

} // namespace ctf

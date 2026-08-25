#include "net_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bytebuffer.h"
#include "game_config.h"
#include "net_util.h"

namespace ctf {

namespace {

uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec) * 1000u +
          static_cast<uint32_t>(ts.tv_nsec) / 1000000u;
}

} // namespace

NetClient::NetClient() {
    tcp_rx_buf_.reserve(4096);
    pending_snapshots_.reserve(8);
    pending_events_.reserve(16);
}

NetClient::~NetClient() { disconnect(); }

void NetClient::log(const char* fmt, ...) const {
    std::fprintf(stderr, "[net_client] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void NetClient::disconnect() {
    if (tcp_fd_ >= 0) {
        ::close(tcp_fd_);
        tcp_fd_ = -1;
    }
    if (udp_fd_ >= 0) {
        ::close(udp_fd_);
        udp_fd_ = -1;
    }
    state_ = NetClientState::Disconnected;
}

// ---------------------------------------------------------------------------
// Handshake (README §5.6)
// ---------------------------------------------------------------------------

bool NetClient::connect_and_join(const std::string& host, uint16_t port,
                                 const std::string& name, int timeout_ms) {
    disconnect();
    state_ = NetClientState::Disconnected;

    // Resolve the host once; reused for both the TCP connect below and the
    // UDP endpoint once we learn its port from JOIN_ACCEPT.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
        log("getaddrinfo(%s) failed", host.c_str());
        return false;
    }
    sockaddr_in server_addr{};
    std::memcpy(&server_addr, res->ai_addr, sizeof(server_addr));
    server_ipv4_be_ = server_addr.sin_addr.s_addr;
    freeaddrinfo(res);

    tcp_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        log("socket() failed: %s", std::strerror(errno));
        return false;
    }
    if (::connect(tcp_fd_, reinterpret_cast<sockaddr*>(&server_addr),
                 sizeof(server_addr)) != 0) {
        log("connect(%s:%u) failed: %s", host.c_str(),
            static_cast<unsigned>(port), std::strerror(errno));
        disconnect();
        return false;
    }
    log("tcp connected to %s:%u", host.c_str(), static_cast<unsigned>(port));

    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        log("udp socket() failed: %s", std::strerror(errno));
        disconnect();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0; // kernel-assigned local port; unrelated to the TCP one
    if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        log("udp bind() failed: %s", std::strerror(errno));
        disconnect();
        return false;
    }
    set_nonblocking(udp_fd_);

    // README §5.4: JOIN_LOBBY carries a fixed 16-byte NUL-padded name.
    protocol::MsgJoinLobby join{};
    std::snprintf(join.name, sizeof(join.name), "%s", name.c_str());
    uint8_t payload[16];
    ByteWriter w(payload, sizeof(payload));
    protocol::encode_join_lobby(w, join);
    send_tcp_frame(protocol::MessageType::JoinLobby, payload, w.size());
    state_ = NetClientState::WaitingJoinAccept;
    log("sent JOIN_LOBBY name=%s", name.c_str());

    uint8_t frame_type = 0;
    std::vector<uint8_t> frame_payload;
    if (!wait_for_tcp_frame(timeout_ms, frame_type, frame_payload)) {
        log("timed out waiting for JOIN_ACCEPT/JOIN_REJECT");
        disconnect();
        return false;
    }

    if (frame_type == static_cast<uint8_t>(protocol::MessageType::JoinAccept)) {
        ByteReader r(frame_payload.data(), frame_payload.size());
        protocol::MsgJoinAccept accept;
        if (!protocol::decode_join_accept(r, accept) || !r.ok()) {
            log("malformed JOIN_ACCEPT");
            disconnect();
            return false;
        }
        player_id_ = accept.player_id;
        session_token_ = accept.session_token;
        server_udp_port_ = accept.udp_port; // never hardcoded (README §5.6)
        state_ = NetClientState::InLobby;
        log("JOIN_ACCEPT: player_id=%u token=0x%08x udp_port=%u",
            static_cast<unsigned>(player_id_), session_token_,
            static_cast<unsigned>(server_udp_port_));
        return true;
    }

    if (frame_type == static_cast<uint8_t>(protocol::MessageType::JoinReject)) {
        ByteReader r(frame_payload.data(), frame_payload.size());
        protocol::MsgJoinReject reject;
        protocol::decode_join_reject(r, reject);
        join_reject_reason_ = reject.reason;
        log("JOIN_REJECT: reason=%d", static_cast<int>(reject.reason));
        disconnect(); // closes sockets; sets state_ = Disconnected...
        state_ = NetClientState::Rejected; // ...then mark the terminal reason
        return false;
    }

    log("unexpected frame type %u while waiting for JOIN_ACCEPT",
        static_cast<unsigned>(frame_type));
    disconnect();
    return false;
}

bool NetClient::wait_for_tcp_frame(int timeout_ms, uint8_t& out_type,
                                   std::vector<uint8_t>& out_payload) {
    const uint32_t deadline = now_ms() + static_cast<uint32_t>(timeout_ms);
    for (;;) {
        // First, see if a full frame is already sitting in the accumulation
        // buffer from a previous (over-)read.
        uint8_t type = 0;
        const uint8_t* payload = nullptr;
        uint16_t payload_len = 0;
        const size_t consumed = recv_framed(tcp_rx_buf_.data(), tcp_rx_buf_.size(),
                                            type, payload, payload_len);
        if (consumed == SIZE_MAX) {
            log("TCP frame exceeds cap - disconnecting");
            return false;
        }
        if (consumed > 0) {
            out_type = type;
            out_payload.assign(payload, payload + payload_len);
            tcp_rx_buf_.erase(tcp_rx_buf_.begin(),
                              tcp_rx_buf_.begin() + static_cast<long>(consumed));
            return true;
        }

        const uint32_t nowv = now_ms();
        if (nowv >= deadline) return false;
        const int remaining = static_cast<int>(deadline - nowv);

        pollfd pfd{tcp_fd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) continue; // loop back around to the deadline check

        uint8_t buf[4096];
        const ssize_t n = ::recv(tcp_fd_, buf, sizeof(buf), 0);
        if (n <= 0) {
            log("TCP closed/error while waiting for a frame");
            return false;
        }
        tcp_rx_buf_.insert(tcp_rx_buf_.end(), buf, buf + n);
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void NetClient::send_tcp_frame(protocol::MessageType type, const uint8_t* payload,
                               size_t len) {
    if (tcp_fd_ < 0) return;
    std::vector<uint8_t> frame(len + config::kTcpFrameHeaderBytes);
    frame[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
    frame[1] = static_cast<uint8_t>(len & 0xFF);
    frame[2] = static_cast<uint8_t>(type);
    if (len > 0) std::memcpy(frame.data() + 3, payload, len);
    send_all(tcp_fd_, frame.data(), frame.size());
}

void NetClient::send_udp_hello() {
    if (udp_fd_ < 0) return;
    uint8_t buf[config::kUdpHeaderBytes + 8];
    ByteWriter w(buf, sizeof(buf));
    protocol::encode_udp_header(
        w, protocol::UdpHeader{protocol::kMagic, protocol::kProtocolVersion,
                               static_cast<uint8_t>(protocol::MessageType::UdpHello),
                               0});
    protocol::encode_udp_hello(w, protocol::MsgUdpHello{player_id_, session_token_});

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = server_ipv4_be_;
    dest.sin_port = htons(server_udp_port_);
    ::sendto(udp_fd_, buf, w.size(), 0, reinterpret_cast<sockaddr*>(&dest),
            sizeof(dest));
    last_udp_hello_sent_ms_ = now_ms();
    log("sent UDP_HELLO");
}

void NetClient::send_input(uint32_t base_seq, const InputCmd* inputs, uint8_t count) {
    if (udp_fd_ < 0) return;
    if (count > config::kInputRedundancy) count = config::kInputRedundancy;

    protocol::MsgPlayerInput msg;
    msg.player_id = player_id_;
    msg.session_token = session_token_;
    msg.base_seq = base_seq;
    msg.count = count;
    for (uint8_t i = 0; i < count; ++i) msg.inputs[i] = inputs[i];

    uint8_t buf[config::kUdpHeaderBytes + 4 + 4 + 4 + 1 +
               config::kInputRedundancy * 3];
    ByteWriter w(buf, sizeof(buf));
    protocol::encode_udp_header(
        w, protocol::UdpHeader{
              protocol::kMagic, protocol::kProtocolVersion,
              static_cast<uint8_t>(protocol::MessageType::PlayerInput), base_seq});
    protocol::encode_player_input(w, msg);
    if (!w.ok()) {
        log("PLAYER_INPUT encode overflow (count=%u) - not sent",
            static_cast<unsigned>(count));
        return;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = server_ipv4_be_;
    dest.sin_port = htons(server_udp_port_);
    ::sendto(udp_fd_, buf, w.size(), 0, reinterpret_cast<sockaddr*>(&dest),
            sizeof(dest));
}

void NetClient::send_start_request() {
    protocol::MsgStartRequest req;
    uint8_t payload[1];
    ByteWriter w(payload, sizeof(payload));
    protocol::encode_start_request(w, req);
    send_tcp_frame(protocol::MessageType::StartRequest, payload, w.size());
    log("sent START_REQUEST");
}

void NetClient::maybe_resend_udp_hello() {
    if (first_snapshot_received_) return;
    if (state_ != NetClientState::InLobby && state_ != NetClientState::InGame) return;
    const uint32_t nowv = now_ms();
    if (nowv - last_udp_hello_sent_ms_ >=
        static_cast<uint32_t>(config::kUdpHelloResendMs)) {
        send_udp_hello();
    }
}

void NetClient::maybe_send_heartbeat() {
    if (tcp_fd_ < 0) return;
    const uint32_t nowv = now_ms();
    const uint32_t interval_ms =
        static_cast<uint32_t>(config::kHeartbeatIntervalSec) * 1000u;
    if (nowv - last_heartbeat_sent_ms_ < interval_ms) return;
    protocol::MsgHeartbeat hb;
    uint8_t payload[1];
    ByteWriter w(payload, sizeof(payload));
    protocol::encode_heartbeat(w, hb);
    send_tcp_frame(protocol::MessageType::Heartbeat, payload, w.size());
    last_heartbeat_sent_ms_ = nowv;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void NetClient::poll() {
    if (tcp_fd_ >= 0) drain_tcp();
    if (udp_fd_ >= 0) drain_udp();
    maybe_resend_udp_hello();
    maybe_send_heartbeat();
}

void NetClient::drain_tcp() {
    for (;;) {
        uint8_t buf[4096];
        const ssize_t n = ::recv(tcp_fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            tcp_rx_buf_.insert(tcp_rx_buf_.end(), buf, buf + n);
            continue;
        }
        if (n == 0) {
            log("TCP connection closed by server");
            disconnect();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        log("TCP recv error: %s", std::strerror(errno));
        disconnect();
        return;
    }

    for (;;) {
        uint8_t type = 0;
        const uint8_t* payload = nullptr;
        uint16_t payload_len = 0;
        const size_t consumed = recv_framed(tcp_rx_buf_.data(), tcp_rx_buf_.size(),
                                            type, payload, payload_len);
        if (consumed == SIZE_MAX) {
            log("TCP frame exceeds cap - disconnecting");
            disconnect();
            return;
        }
        if (consumed == 0) break;
        dispatch_tcp_frame(type, payload, payload_len);
        tcp_rx_buf_.erase(tcp_rx_buf_.begin(),
                          tcp_rx_buf_.begin() + static_cast<long>(consumed));
    }
}

void NetClient::drain_udp() {
    for (;;) {
        uint8_t buf[2048];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = ::recvfrom(udp_fd_, buf, sizeof(buf), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            log("UDP recv error: %s", std::strerror(errno));
            break;
        }
        if (n < static_cast<ssize_t>(config::kUdpHeaderBytes)) {
            continue; // too short even for the header - dropped, no crash
        }
        ByteReader r(buf, static_cast<size_t>(n));
        protocol::UdpHeader hdr;
        if (!protocol::decode_udp_header(r, hdr) || !r.ok()) {
            continue; // wrong magic/version/truncated - dropped (README §5.2)
        }
        const uint8_t* payload = buf + config::kUdpHeaderBytes;
        const size_t payload_len =
            static_cast<size_t>(n) - config::kUdpHeaderBytes;
        dispatch_udp_payload(static_cast<protocol::MessageType>(hdr.type), payload,
                             payload_len);
    }
}

void NetClient::dispatch_tcp_frame(uint8_t type, const uint8_t* payload, size_t len) {
    ByteReader r(payload, len);
    switch (static_cast<protocol::MessageType>(type)) {
        case protocol::MessageType::LobbyState: {
            protocol::MsgLobbyState msg;
            if (!protocol::decode_lobby_state(r, msg) || !r.ok()) return;
            lobby_state_ = msg;
            log("LOBBY_STATE: %u players, host=%u",
                static_cast<unsigned>(msg.player_count),
                static_cast<unsigned>(msg.host_id));
            pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::GameStart: {
            protocol::MsgGameStart msg;
            if (!protocol::decode_game_start(r, msg) || !r.ok()) return;
            game_start_ = msg;
            state_ = NetClientState::InGame;
            log("GAME_START: %u players, start_tick=%u",
                static_cast<unsigned>(msg.player_count), msg.start_tick);
            pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::PlayerKilled: {
            protocol::MsgPlayerKilled msg;
            if (protocol::decode_player_killed(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::PlayerRespawned: {
            protocol::MsgPlayerRespawned msg;
            if (protocol::decode_player_respawned(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::FlagPickedUp: {
            protocol::MsgFlagPickedUp msg;
            if (protocol::decode_flag_picked_up(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::FlagDropped: {
            protocol::MsgFlagDropped msg;
            if (protocol::decode_flag_dropped(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::FlagReturned: {
            protocol::MsgFlagReturned msg;
            if (protocol::decode_flag_returned(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::FlagCaptured: {
            protocol::MsgFlagCaptured msg;
            if (protocol::decode_flag_captured(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        case protocol::MessageType::MatchEnd: {
            protocol::MsgMatchEnd msg;
            if (protocol::decode_match_end(r, msg) && r.ok()) {
                pending_events_.push_back(msg);
                log("MATCH_END: winner=%d %u-%u", static_cast<int>(msg.winning_team),
                    static_cast<unsigned>(msg.score_red),
                    static_cast<unsigned>(msg.score_blue));
            }
            break;
        }
        default:
            break; // JOIN_ACCEPT/JOIN_REJECT/HEARTBEAT (C->S only)/unknown: ignore
    }
}

void NetClient::dispatch_udp_payload(protocol::MessageType type,
                                     const uint8_t* payload, size_t len) {
    ByteReader r(payload, len);
    switch (type) {
        case protocol::MessageType::WorldSnapshot: {
            WorldSnapshot snap;
            if (!protocol::decode_world_snapshot(r, snap) || !r.ok()) return;
            first_snapshot_received_ = true; // stops UDP_HELLO resends
            pending_snapshots_.push_back(snap);
            break;
        }
        case protocol::MessageType::ShotFired: {
            protocol::MsgShotFired msg;
            if (protocol::decode_shot_fired(r, msg) && r.ok())
                pending_events_.push_back(msg);
            break;
        }
        default:
            break; // UDP_HELLO/PLAYER_INPUT are C->S only; unknown types ignored
    }
}

std::vector<WorldSnapshot> NetClient::take_snapshots() {
    std::vector<WorldSnapshot> out;
    out.swap(pending_snapshots_);
    return out;
}

std::vector<GameEvent> NetClient::take_events() {
    std::vector<GameEvent> out;
    out.swap(pending_events_);
    return out;
}

} // namespace ctf

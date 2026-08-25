#include "net_server.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "bytebuffer.h"
#include "broadcast.h"
#include "map.h"
#include "net_util.h"

// The network thread's whole world (README §3.2): sockets, framing, the
// registry, and the queues' edge. It never reads a player position — game
// state flows in as commands and out as pre-serialized events.

namespace ctf {

namespace {

constexpr size_t kAccumCap = config::kTcpFrameCapBytes;

// Wraps a [u8 type][encoded fields] payload into a TCP frame
// ([u16 len][u8 type][payload], README §5.3).
std::vector<uint8_t> frame_payload(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> framed;
    const uint16_t n = static_cast<uint16_t>(payload.empty()
                                                 ? 0
                                                 : payload.size() - 1);
    framed.push_back(static_cast<uint8_t>(n >> 8));
    framed.push_back(static_cast<uint8_t>(n & 0xFF));
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
}

void fill_sockaddr(sockaddr_in& addr, uint16_t port) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
}

} // namespace

NetServer::NetServer(uint16_t port, InboundQueue& inbound,
                     OutboundQueue& outbound, std::unique_ptr<IPoller> poller,
                     int poll_timeout_ms, int udp_silence_ms)
    : port_(port),
      inbound_(inbound),
      outbound_(outbound),
      poller_(std::move(poller)),
      poll_timeout_ms_(poll_timeout_ms),
      udp_silence_ms_(udp_silence_ms) {}

NetServer::~NetServer() {
    for (const auto& entry : registry_.storage()) {
        if (entry.tcp_fd >= 0) {
            close(entry.tcp_fd);
        }
    }
    if (tcp_listen_fd_ >= 0) close(tcp_listen_fd_);
    if (udp_fd_ >= 0) close(udp_fd_);
    if (event_fd_ >= 0) close(event_fd_);
}

uint32_t NetServer::now_ms() const {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

uint32_t NetServer::make_token() {
    // Session tokens only need to be unguessable-ish on a LAN; rand() is
    // fine at this threat model and keeps the code dependency-free.
    static uint32_t counter = 0;
    const uint32_t r = static_cast<uint32_t>(rand()) << 16 ^
                       static_cast<uint32_t>(rand()) ^ (++counter * 2654435761u);
    return r | 1; // never zero
}

bool NetServer::start() {
    tcp_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_listen_fd_ < 0) return false;

    const int one = 1;
    setsockopt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr;
    fill_sockaddr(addr, port_);
    if (bind(tcp_listen_fd_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        return false;
    }
    if (listen(tcp_listen_fd_, 8) < 0) return false;
    socklen_t len = sizeof(addr);
    getsockname(tcp_listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    local_port_ = ntohs(addr.sin_port);

    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) return false;
    fill_sockaddr(addr, port_); // port 0 -> kernel picks; same policy as TCP
    if (bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return false;
    }
    len = sizeof(addr);
    getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    udp_port_ = ntohs(addr.sin_port);

    event_fd_ = eventfd(0, EFD_NONBLOCK);
    if (event_fd_ < 0) return false;

    set_nonblocking(tcp_listen_fd_);
    set_nonblocking(udp_fd_);

    poller_->add(tcp_listen_fd_, false);
    poller_->add(udp_fd_, false);
    poller_->add(event_fd_, false);
    last_silence_check_ms_ = now_ms();
    return true;
}

void NetServer::stop() { running_ = false; }

void NetServer::run() {
    running_ = true;
    PollResult results[32];

    while (running_) {
        const int n = poller_->wait(results, 32, poll_timeout_ms_);
        for (int i = 0; i < n && running_; ++i) {
            const PollResult& r = results[i];
            if (r.fd == event_fd_) {
                handle_wake();
            } else if (r.fd == udp_fd_) {
                if (r.readable) handle_udp_readable();
            } else if (r.fd == tcp_listen_fd_) {
                accept_new_clients();
            } else if (r.readable) {
                handle_tcp_readable(r.fd);
            } else if (r.writable) {
                flush_pending(r.fd);
            }
        }

        // POLLOUT interest follows pending-buffer occupancy.
        for (const auto& entry : registry_.storage()) {
            if (entry.tcp_fd >= 0) {
                poller_->set_watch_write(entry.tcp_fd,
                                         !entry.pending_out.empty());
            }
        }

        enforce_pending_cap();

        const uint32_t now = now_ms();
        if (now - last_silence_check_ms_ >= 100) {
            check_silence_timeouts();
        }
    }
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------

void NetServer::accept_new_clients() {
    for (;;) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = accept(tcp_listen_fd_,
                              reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) break;
        set_nonblocking(fd);
        poller_->add(fd, false);
    }
}

template <typename Msg, typename EncodeFn>
static void send_frame_generic(int fd, protocol::MessageType type, const Msg& msg,
                               EncodeFn encode) {
    std::vector<uint8_t> payload(512);
    ByteWriter w(payload.data(), payload.size());
    encode(w, msg);
    payload.resize(w.size());

    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    frame.push_back(static_cast<uint8_t>(type));
    frame.insert(frame.end(), payload.begin(), payload.end());
    send_all(fd, frame.data(), frame.size());
}

std::vector<uint8_t>& NetServer::accum_for(int fd) {
    // Registered clients accumulate under their player_id slot; a fresh
    // pre-join connection accumulates in its own buffer until JOIN_LOBBY
    // assigns it an entry.
    ClientEntry* entry = registry_.find_by_fd(fd);
    if (entry != nullptr) {
        return tcp_in_[entry->player_id];
    }
    return pre_join_[fd];
}

void NetServer::handle_tcp_readable(int fd) {
    ClientEntry* entry = registry_.find_by_fd(fd);
    const uint8_t id = entry ? entry->player_id : 0xFF;
    std::vector<uint8_t>& accum = accum_for(fd);

    uint8_t chunk[2048];
    for (;;) {
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
            if (entry != nullptr) disconnect(*entry, true);
            else close_unregistered(fd);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // hard error: definitive (README §5.7)
            if (entry != nullptr) disconnect(*entry, true);
            else close_unregistered(fd);
            return;
        }
        accum.insert(accum.end(), chunk, chunk + n);

        if (accum.size() > kAccumCap) {
            if (entry != nullptr) disconnect(*entry, true); // framing cap exceeded
            else close_unregistered(fd);
            return;
        }
        if (n < static_cast<ssize_t>(sizeof(chunk))) break;
    }

    // Dispatch every complete frame.
    size_t pos = 0;
    bool dead = false;
    while (!dead && accum.size() - pos >= config::kTcpFrameHeaderBytes) {
        uint8_t type = 0;
        const uint8_t* payload = nullptr;
        uint16_t plen = 0;
        const size_t consumed =
            recv_framed(accum.data() + pos, accum.size() - pos, type,
                        payload, plen);
        if (consumed == 0) break;                       // need more bytes
        if (consumed == SIZE_MAX || plen > kAccumCap) {  // oversize frame
            dead = true;
            break;
        }

        ByteReader r(payload, plen);
        switch (static_cast<protocol::MessageType>(type)) {
            case protocol::MessageType::JoinLobby: {
                protocol::MsgJoinLobby m;
                if (decode_join_lobby(r, m)) on_join_lobby(fd, m);
                break;
            }
            case protocol::MessageType::StartRequest: {
                protocol::MsgStartRequest m;
                if (protocol::decode_start_request(r, m)) on_start_request(fd, m);
                break;
            }
            case protocol::MessageType::Heartbeat: {
                // Liveness is owned by the two §7.5 detectors; consume the
                // message so it is not treated as unknown/malformed.
                protocol::MsgHeartbeat hb;
                protocol::decode_heartbeat(r, hb);
                break;
            }
            default:
                break; // unknown/malformed: ignore frame
        }
        pos += consumed;
    }

    if (dead) {
        ClientEntry* e = registry_.find_by_fd(fd);
        if (e != nullptr) disconnect(*e, true);
        else close_unregistered(fd); // oversize frame from a pre-join fd
        return;
    }
    if (pos > 0) {
        accum.erase(accum.begin(), accum.begin() + pos);
    }
}

void NetServer::flush_pending(int fd) {
    ClientEntry* entry = registry_.find_by_fd(fd);
    if (entry == nullptr) return;
    const uint8_t id = entry->player_id;

    std::vector<uint8_t>& pending = entry->pending_out;
    while (!pending.empty()) {
        const ssize_t n = send(fd, pending.data(), pending.size(),
                               MSG_NOSIGNAL);
        if (n > 0) {
            pending.erase(pending.begin(), pending.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        disconnect(*entry, true);
        return;
    }
    (void)id;
}

// ---------------------------------------------------------------------------
// lobby / handshake
// ---------------------------------------------------------------------------

void NetServer::on_join_lobby(int fd, const protocol::MsgJoinLobby& msg) {
    ClientEntry* existing = registry_.find_by_fd(fd);
    if (existing != nullptr) return; // already joined

    ClientEntry* entry = registry_.add(fd, msg.name);
    const JoinResult lobby_result =
        entry != nullptr ? lobby_.add_player(entry->player_id)
                         : JoinResult::Full;
    if (lobby_result != JoinResult::Ok) {
        // Reject with the real reason and roll the registry back so the
        // connection slot is not leaked.
        if (entry != nullptr) {
            registry_.remove(entry->player_id);
        }
        send_frame_generic(
            fd, protocol::MessageType::JoinReject,
            protocol::MsgJoinReject{
                lobby_result == JoinResult::InProgress
                    ? ctf::protocol::JoinRejectReason::InProgress
                    : ctf::protocol::JoinRejectReason::Full},
            protocol::encode_join_reject);
        return;
    }
    entry->session_token = make_token();
    entry->last_input_tick = now_ms(); // silence clock starts at join

    send_frame_generic(fd, protocol::MessageType::JoinAccept,
                       protocol::MsgJoinAccept{entry->player_id,
                                               entry->session_token,
                                               udp_port_},
                       protocol::encode_join_accept);
    broadcast_lobby_state();
}

void NetServer::broadcast_lobby_state() {
    protocol::MsgLobbyState st;
    const auto& storage = registry_.storage();
    st.player_count = 0;
    for (const auto& e : storage) {
        if (e.tcp_fd < 0) continue;
        if (st.player_count >= config::kMaxPlayers) break;
        st.ids[st.player_count] = e.player_id;
        std::snprintf(st.names[st.player_count], sizeof(st.names[st.player_count]),
                      "%s", e.name.c_str());
        ++st.player_count;
    }
    st.host_id = lobby_.host_id();

    std::vector<uint8_t> payload(600);
    ByteWriter w(payload.data(), payload.size());
    protocol::encode_lobby_state(w, st);
    payload.resize(w.size());
    payload.insert(payload.begin(),
                   static_cast<uint8_t>(protocol::MessageType::LobbyState));
    broadcast_tcp_event(registry_, frame_payload(payload));
}

void NetServer::on_start_request(int fd, const protocol::MsgStartRequest&) {
    ClientEntry* entry = registry_.find_by_fd(fd);
    if (entry == nullptr) return;
    if (!lobby_.can_issue_start(entry->player_id)) {
        send_frame_generic(fd, protocol::MessageType::JoinReject,
                           protocol::MsgJoinReject{ctf::protocol::JoinRejectReason::InProgress},
                           protocol::encode_join_reject);
        return;
    }
    begin_match_and_notify();
}

void NetServer::begin_match_and_notify() {
    if (!lobby_.begin_match()) return;

    auto [red, blue] = lobby_.assign_teams();

    protocol::MsgGameStart gs;
    gs.start_tick = 0;
    int red_spawn = 0;
    int blue_spawn = 0;
    auto add_side = [&](const std::vector<uint8_t>& ids, Team team) {
        for (uint8_t id : ids) {
            if (gs.player_count >= config::kMaxPlayers) return;
            gs.ids[gs.player_count] = id;
            gs.teams[gs.player_count] = team;
            const Vec2Fixed* spawns =
                team == Team::Blue ? kBlueSpawnPoints : kRedSpawnPoints;
            const uint8_t idx =
                static_cast<uint8_t>(team == Team::Blue ? blue_spawn++
                                                        : red_spawn++);
            gs.spawn_points[gs.player_count] = spawns[idx];

            InboundCommand cmd;
            cmd.type = InboundCommandType::PlayerJoined;
            cmd.player_id = id;
            cmd.team = team;
            cmd.spawn_index = idx;
            inbound_.push(cmd);
            ++gs.player_count;
        }
    };
    add_side(red, Team::Red);
    add_side(blue, Team::Blue);

    std::vector<uint8_t> payload(1200);
    ByteWriter w(payload.data(), payload.size());
    protocol::encode_game_start(w, gs);
    payload.resize(w.size());
    payload.insert(payload.begin(),
                   static_cast<uint8_t>(protocol::MessageType::GameStart));
    broadcast_tcp_event(registry_, frame_payload(payload));
}

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------

void NetServer::handle_udp_readable() {
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in src{};
        socklen_t len = sizeof(src);
        const ssize_t n = recvfrom(udp_fd_, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break; // EAGAIN or gone
        }
        on_udp_packet(src, buf, static_cast<size_t>(n));
    }
}

void NetServer::on_udp_packet(const sockaddr_in& src, const uint8_t* data,
                              size_t len) {
    ByteReader r(data, len);
    protocol::UdpHeader hdr;
    if (!protocol::decode_udp_header(r, hdr)) {
        return; // bad magic/version/truncated: dropped before any routing
    }
    // Reconstruct the payload position: header is fixed 8 bytes.
    const uint8_t* payload = data + config::kUdpHeaderBytes;
    const size_t payload_len =
        len > config::kUdpHeaderBytes ? len - config::kUdpHeaderBytes : 0;

    switch (static_cast<protocol::MessageType>(hdr.type)) {
        case protocol::MessageType::UdpHello: {
            ByteReader pr(payload, payload_len);
            protocol::MsgUdpHello m;
            if (protocol::decode_udp_hello(pr, m)) on_udp_hello(src, hdr.tick, m);
            break;
        }
        case protocol::MessageType::PlayerInput: {
            ByteReader pr(payload, payload_len);
            protocol::MsgPlayerInput m;
            if (protocol::decode_player_input(pr, m)) on_player_input(src, hdr.tick, m);
            break;
        }
        default:
            break; // unknown UDP types ignored
    }
}

ClientEntry* NetServer::auth_udp_sender(const sockaddr_in& src,
                                        uint8_t player_id, uint32_t token) {
    ClientEntry* entry = registry_.find_by_id(player_id);
    if (entry == nullptr) return nullptr;
    if (entry->session_token != token) {
        return nullptr; // spoofed id: LAN anyone can send, token gates it
    }
    if (entry->has_udp_addr &&
        (entry->udp_addr.sin_port != src.sin_port ||
         entry->udp_addr.sin_addr.s_addr != src.sin_addr.s_addr)) {
        return nullptr; // rebound mid-match without re-hello
    }
    return entry;
}

void NetServer::on_udp_hello(const sockaddr_in& src, uint32_t tick,
                             const protocol::MsgUdpHello& msg) {
    (void)tick;
    ClientEntry* entry = auth_udp_sender(src, msg.player_id, msg.session_token);
    if (entry == nullptr) return;
    entry->udp_addr = src;
    entry->has_udp_addr = true;
    entry->last_input_tick = now_ms();
}

void NetServer::on_player_input(const sockaddr_in& src, uint32_t tick,
                                const protocol::MsgPlayerInput& msg) {
    (void)tick;
    ClientEntry* entry = auth_udp_sender(src, msg.player_id, msg.session_token);
    if (entry == nullptr) return; // token/address mismatch: dropped
    entry->last_input_tick = now_ms();

    // One command per redundant input; seqs are base_seq-2..base_seq.
    const uint8_t count =
        msg.count < config::kInputRedundancy ? msg.count : config::kInputRedundancy;
    for (int i = 0; i < count; ++i) {
        InboundCommand cmd;
        cmd.type = InboundCommandType::PlayerInput;
        cmd.player_id = msg.player_id;
        cmd.seq = msg.base_seq - count + 1 + static_cast<uint32_t>(i);
        cmd.input = msg.inputs[i];
        inbound_.push(cmd);
    }
}

// ---------------------------------------------------------------------------
// outbound queue + detectors
// ---------------------------------------------------------------------------

void NetServer::handle_wake() {
    uint64_t val;
    while (read(event_fd_, &val, sizeof(val)) > 0) {
    } // drain

    OutboundEvent ev;
    while (outbound_.pop(ev)) {
        switch (ev.type) {
            case OutboundEventType::UdpSnapshot:
                send_udp_snapshots(registry_, udp_fd_, ev.payload_ptr(),
                                   ev.payload_len(), ev.acks, ev.tick);
                ++udp_snapshots_sent_;
                break;
            case OutboundEventType::TcpBroadcast:
                // Frame it once ([u16 len][u8 type][payload], README §5.3):
                // sim events arrive as [u8 type][encoded fields].
                if (!ev.payload_empty()) {
                    std::vector<uint8_t> payload_vec(
                        ev.payload_ptr(), ev.payload_ptr() + ev.payload_len());
                    broadcast_tcp_event(registry_, frame_payload(payload_vec));
                    ++tcp_events_fanned_out_;
                }
                break;
            case OutboundEventType::UdpEvent:
                // Cosmetic tracers go to every registered UDP address.
                // ev.payload is [u8 type][encoded fields] from ser_event();
                // the type byte goes into the UDP header (dgram[3]), and
                // only the encoded fields go into the body (skip byte 0).
                registry_.for_each_live([&](const ClientEntry& entry) {
                    if (!entry.has_udp_addr) return;
                    ClientEntry* e =
                        registry_.find_by_id(entry.player_id);
                    if (e == nullptr) return;
                    const size_t body_len =
                        ev.payload_len() > 1 ? ev.payload_len() - 1 : 0;
                    std::vector<uint8_t> dgram(
                        config::kUdpHeaderBytes + body_len);
                    dgram[0] = protocol::kMagic >> 8;
                    dgram[1] = protocol::kMagic & 0xFF;
                    dgram[2] = protocol::kProtocolVersion;
                    dgram[3] =
                        ev.payload_empty() ? 0 : ev.payload_data[0];
                    if (body_len > 0) {
                        std::copy(ev.payload_ptr() + 1,
                                  ev.payload_ptr() + ev.payload_len(),
                                  dgram.begin() +
                                      config::kUdpHeaderBytes);
                    }
                    sendto(udp_fd_, dgram.data(), dgram.size(), 0,
                           reinterpret_cast<const sockaddr*>(
                               &e->udp_addr),
                           sizeof(e->udp_addr));
                });
                break;
            case OutboundEventType::MatchReset:
                // Sim has reset — return to lobby.
                lobby_.end_match();
                broadcast_lobby_state();
                break;
        }
    }
}

void NetServer::enforce_pending_cap() {
    for (const auto& entry : registry_.storage()) {
        if (entry.tcp_fd < 0) continue;
        if (entry.pending_out.size() >
            config::kTcpPendingBufferCapBytes) {
            ClientEntry* e = registry_.find_by_id(entry.player_id);
            if (e != nullptr) disconnect(*e, true);
        }
    }
}

void NetServer::check_silence_timeouts() {
    last_silence_check_ms_ = now_ms();
    for (const auto& entry : registry_.storage()) {
        if (entry.tcp_fd < 0) continue;
        if (last_silence_check_ms_ - entry.last_input_tick >
            static_cast<uint32_t>(udp_silence_ms_)) {
            ClientEntry* e = registry_.find_by_id(entry.player_id);
            if (e != nullptr) disconnect(*e, true);
        }
    }
}

void NetServer::disconnect(ClientEntry& entry, bool notify_sim) {
    poller_->remove(entry.tcp_fd);
    close(entry.tcp_fd);
    pre_join_.erase(entry.tcp_fd);
    entry.tcp_fd = -1;

    const uint8_t id = entry.player_id;
    const bool was_in_progress = lobby_.in_progress();
    registry_.remove(id);
    lobby_.remove_player(id);
    tcp_in_[id].clear();

    if (notify_sim && was_in_progress) {
        InboundCommand cmd;
        cmd.type = InboundCommandType::PlayerLeft;
        cmd.player_id = id;
        inbound_.push(cmd);
    }
    broadcast_lobby_state();
}

void NetServer::close_unregistered(int fd) {
    // fd has no ClientEntry: either it never completed JOIN_LOBBY, or
    // on_join_lobby() already rolled the entry back after sending
    // JOIN_REJECT (README §5.6/§5.7 - a rejected client still closes its
    // socket like any other disconnect, and that must not crash the server).
    poller_->remove(fd);
    close(fd);
    pre_join_.erase(fd);
}

} // namespace ctf

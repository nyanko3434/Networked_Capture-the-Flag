// ctf_client entry point (README §9, §3.5): connects, runs the handshake,
// then a single loop that runs a fixed 30Hz client tick (README §6.2 - "same
// rate as server", independent of raylib's render/frame rate) alongside
// variable-rate rendering, until the window closes or SIGINT.
//
// IMPORTANT correctness note, not spelled out anywhere in README/the guide -
// found while wiring this together, not assumed:
//
// The server's two liveness mechanisms are UDP silence (README §7.5/§2.2:
// 3s of no PLAYER_INPUT/UDP_HELLO = disconnect) and the respawn timer
// (README §8: 90 ticks @ 30Hz = exactly 3.0s, README §7.4). Those two
// numbers coincide. If this client only sent PLAYER_INPUT while the local
// player is alive and predicting movement, a player who dies would stop
// sending input for up to 3s while waiting to respawn - landing exactly on
// the UDP silence threshold and risking a false disconnect right as they
// respawn. The same problem exists during a lobby wait longer than 3s: once
// the first WORLD_SNAPSHOT arrives, NetClient stops resending UDP_HELLO
// (net_client.cpp), and nothing else refreshes the server's liveness clock
// until PLAYER_INPUT starts flowing.
//
// Fix applied here: the fixed 30Hz tick - and therefore a PLAYER_INPUT send
// every tick - runs continuously from right after the handshake through the
// whole session, in and out of matches. Only Prediction::on_local_tick()
// (actual movement simulation) is gated on "alive and in a live match";
// sending input is not (verified against server/net_server.cpp's
// on_player_input(), which refreshes last_input_tick unconditionally, even
// for an already-seen/duplicate seq - so resending the same idle packet
// every tick while not predicting is both safe and sufficient).

#include <raylib.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game_config.h"
#include "interpolation.h"
#include "net_client.h"
#include "prediction.h"
#include "render.h"

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop = true; }

struct ClientArgs {
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    std::string name = "player";
};

ClientArgs parse_args(int argc, char** argv) {
    ClientArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--name" && i + 1 < argc) {
            args.name = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: ctf_client [--host HOST] [--port N] [--name NAME]\n");
            std::exit(0);
        }
    }
    return args;
}

// Raylib-based input sampling. This is a platform concern that deliberately
// lives one layer up from prediction.cpp/net_client.cpp (see the comment in
// prediction.h): the bot (§3.6) samples input too, but synthetically,
// without raylib.
ctf::InputCmd sample_input(ctf::Vec2Fixed local_player_position) {
    using namespace ctf;
    uint8_t buttons = 0;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) buttons |= kInputUp;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) buttons |= kInputDown;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) buttons |= kInputLeft;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) buttons |= kInputRight;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) buttons |= kInputFire;

    // Aim: angle from the player's own last predicted position to the mouse
    // cursor. Relies on render.cpp's documented 1:1 world-to-screen pixel
    // mapping. Not specified anywhere in README - main.cpp's own choice of
    // aiming scheme, noted here rather than assumed silently.
    const Vector2 mouse = GetMousePosition();
    const float cx = static_cast<float>(local_player_position.x) /
                         config::kFixedScale +
                     config::kPlayerSizePx / 2.0f;
    const float cy = static_cast<float>(local_player_position.y) /
                         config::kFixedScale +
                     config::kPlayerSizePx / 2.0f;
    float radians = std::atan2(mouse.y - cy, mouse.x - cx);
    if (radians < 0.0f) radians += 2.0f * PI;
    const uint16_t aim =
        static_cast<uint16_t>(radians / (2.0f * PI) * 65536.0f);

    return InputCmd{buttons, aim};
}

const ctf::PlayerState* find_self(const ctf::WorldSnapshot& snap, uint8_t id) {
    for (uint8_t i = 0; i < snap.player_count; ++i) {
        if (snap.players[i].id == id) return &snap.players[i];
    }
    return nullptr;
}

// Sends one PLAYER_INPUT packet from whatever Prediction currently has -
// the last config::kInputRedundancy history entries, or a harmless idle
// packet if history is empty (pre-GAME_START, or no ticks predicted since
// respawn). See the file-level comment: this is called every tick
// regardless of alive/match state, only the *content* varies.
void send_current_input(ctf::NetClient& net, const ctf::Prediction& prediction) {
    const auto& history = prediction.history();
    if (history.empty()) {
        ctf::InputCmd idle{0, 0};
        net.send_input(0, &idle, 1);
        return;
    }
    ctf::InputCmd last[ctf::config::kInputRedundancy];
    uint8_t count = 0;
    const size_t cap = static_cast<size_t>(ctf::config::kInputRedundancy);
    const size_t start = history.size() > cap ? history.size() - cap : 0;
    for (size_t i = start; i < history.size(); ++i) last[count++] = history[i].input;
    net.send_input(prediction.current_seq(), last, count);
}

} // namespace

int main(int argc, char** argv) {
    ClientArgs args = parse_args(argc, argv);
    std::signal(SIGINT, on_sigint);

    ctf::NetClient net;
    if (!net.connect_and_join(args.host, args.port, args.name)) {
        const bool rejected = net.state() == ctf::NetClientState::Rejected;
        std::fprintf(stderr, "ctf_client: failed to join %s:%u (%s)\n",
                     args.host.c_str(), static_cast<unsigned>(args.port),
                     rejected ? "rejected" : "timed out");
        return 1;
    }
    const uint8_t my_id = net.player_id();

    ctf::Renderer renderer;
    if (!renderer.init(ctf::config::kMapWidthPx, ctf::config::kMapHeightPx,
                       "ctf_client")) {
        std::fprintf(stderr, "ctf_client: failed to open window\n");
        return 1;
    }

    ctf::Prediction prediction;
    ctf::Interpolation interpolation;
    ctf::WorldSnapshot latest_snapshot{};
    bool have_snapshot = false;

    const double tick_dt = 1.0 / ctf::config::kTickRateHz;
    double accumulator = 0.0;
    double last_time = GetTime();

    int ticks_this_window = 0;
    double window_start = GetTime();
    double measured_tick_hz = 0.0;

    while (!g_stop && !renderer.should_close()) {
        const double now = GetTime();
        const double frame_dt = now - last_time;
        last_time = now;
        accumulator += frame_dt;

        net.poll();
        for (const auto& snap : net.take_snapshots()) {
            latest_snapshot = snap;
            have_snapshot = true;
            interpolation.push_snapshot(snap);
            prediction.on_snapshot(snap, my_id);
        }
        for (const auto& ev : net.take_events()) renderer.on_event(ev);

        if (!net.game_started() && net.is_host() && IsKeyPressed(KEY_ENTER)) {
            net.send_start_request();
        }

        // Fixed 30Hz client tick (README §6.2), decoupled from render rate.
        // Capped per frame so a long stall (e.g. window drag) can't spiral
        // into catching up hundreds of ticks at once.
        int ticks_this_frame = 0;
        while (accumulator >= tick_dt && ticks_this_frame < 10) {
            accumulator -= tick_dt;
            ++ticks_this_frame;

            const bool alive_in_match = [&] {
                if (!net.game_started() || !have_snapshot) return false;
                const ctf::PlayerState* self = find_self(latest_snapshot, my_id);
                return self != nullptr && self->alive;
            }();

            if (alive_in_match) {
                const ctf::InputCmd cmd = sample_input(prediction.local_state().position);
                prediction.on_local_tick(cmd);
            }
            // Always sent - see the file-level comment on liveness timing.
            send_current_input(net, prediction);
            ++ticks_this_window;
        }

        interpolation.advance(frame_dt);

        const double t = GetTime();
        if (t - window_start >= 1.0) {
            measured_tick_hz = ticks_this_window / (t - window_start);
            ticks_this_window = 0;
            window_start = t;
        }

        if (net.game_started() && have_snapshot) {
            // If match just ended, reset the match state flag.
            renderer.reset_match_state();

            ctf::HudMetrics hud;
            hud.unacked_input_count =
                static_cast<uint32_t>(prediction.history().size());
            // README §6.3: "The unacked count *is* the RTT made visible" -
            // the protocol has no explicit ping/pong, so RTT is derived from
            // it rather than measured directly.
            hud.rtt_ms = hud.unacked_input_count /
                        static_cast<double>(ctf::config::kTickRateHz) * 1000.0;
            hud.misprediction_rate =
                prediction.current_seq() > 0
                    ? static_cast<double>(prediction.mispredictions()) /
                          static_cast<double>(prediction.current_seq())
                    : 0.0;
            hud.snapshot_buffer_depth =
                static_cast<uint32_t>(interpolation.snapshot_count());
            hud.actual_tick_rate_hz = measured_tick_hz;
            renderer.draw_frame(latest_snapshot, my_id, prediction, interpolation,
                               hud);
        } else if (renderer.match_ended()) {
            // Match ended, waiting for server to reset and return to lobby.
            renderer.draw_results_screen();
        } else if (net.is_host()) {
            renderer.draw_waiting_screen(
                "Waiting for players...",
                "You are the host - press ENTER to start");
        } else {
            renderer.draw_waiting_screen("Waiting for the host to start...",
                                        nullptr);
        }
    }

    renderer.shutdown();
    net.disconnect(); // closes the TCP fd -> server sees recv()==0
                      // immediately (README §5.7/§2.2's definitive path)
    return 0;
}

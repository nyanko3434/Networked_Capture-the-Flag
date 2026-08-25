// ctf_client entry point (implementation_guide.md §3.5). Wires
// net_client.cpp (§3.1), prediction.cpp (§3.2), interpolation.cpp (§3.3),
// and render.cpp (§3.4) into the real client loop: connect, run the
// handshake, then per tick: sample input -> predict -> send -> receive
// snapshot -> reconcile/interpolate -> render (README §6.2).

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>

#include <raylib.h>

#include "game_config.h"
#include "game_types.h"
#include "interpolation.h"
#include "net_client.h"
#include "prediction.h"
#include "render.h"

using namespace ctf;
using Clock = std::chrono::steady_clock;

namespace {

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
        }
    }
    return args;
}

// Samples raylib key state into one InputCmd's buttons (README §5.4
// bitmask). This is the one place platform input is read; the bot (§3.6)
// supplies its own synthetic sampler with the same shape instead of this
// one, since it has no raylib window to read from. aim_angle is filled in
// separately by aim_angle_towards_mouse() below, once the caller knows the
// local player's actual render position.
InputCmd sample_input() {
    InputCmd cmd;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) cmd.buttons |= kInputUp;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) cmd.buttons |= kInputDown;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) cmd.buttons |= kInputLeft;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) cmd.buttons |= kInputRight;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) cmd.buttons |= kInputFire;
    return cmd;
}

// aim_angle: mouse position relative to the local player's own predicted
// position, converted to the u16 0..2*pi encoding (README §5.4/§5.5).
// render.cpp draws 1:1 with map pixels (same convention documented there),
// so GetMousePosition() is already in map space, no extra transform.
uint16_t aim_angle_towards_mouse(Vec2Fixed local_fp_position) {
    const Vector2 mouse = GetMousePosition();
    const float cx = static_cast<float>(local_fp_position.x) /
                         config::kFixedScale +
                     config::kPlayerSizePx / 2.0f;
    const float cy = static_cast<float>(local_fp_position.y) /
                         config::kFixedScale +
                     config::kPlayerSizePx / 2.0f;
    float radians = std::atan2(mouse.y - cy, mouse.x - cx);
    if (radians < 0.0f) radians += 2.0f * PI;
    const float turns = radians / (2.0f * PI); // 0..1
    return static_cast<uint16_t>(turns * 65536.0f);
}

const PlayerState* find_player(const WorldSnapshot& snap, uint8_t id) {
    for (uint8_t i = 0; i < snap.player_count; ++i) {
        if (snap.players[i].id == id) return &snap.players[i];
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    ClientArgs args = parse_args(argc, argv);
    std::printf("ctf_client: host=%s port=%u name=%s\n", args.host.c_str(),
               static_cast<unsigned>(args.port), args.name.c_str());

    // --- Handshake steps 1-2 (README §5.6): TCP connect, JOIN_LOBBY, wait
    // for JOIN_ACCEPT/JOIN_REJECT. Blocks (bounded by its own timeout_ms)
    // before any window exists - nothing to render yet.
    NetClient net;
    if (!net.connect_and_join(args.host, args.port, args.name)) {
        if (net.state() == NetClientState::Rejected) {
            const char* reason = "unknown";
            switch (net.join_reject_reason()) {
                case protocol::JoinRejectReason::Full: reason = "lobby full"; break;
                case protocol::JoinRejectReason::InProgress: reason = "match in progress"; break;
                case protocol::JoinRejectReason::BadVersion: reason = "version mismatch"; break;
            }
            std::fprintf(stderr, "ctf_client: join rejected: %s\n", reason);
        } else {
            std::fprintf(stderr, "ctf_client: failed to connect/join (timeout)\n");
        }
        return 1;
    }
    std::printf("ctf_client: joined as player_id=%u\n",
               static_cast<unsigned>(net.player_id()));

    // Window sized 1:1 to the map (README §7.1, matching render.cpp's own
    // pixel-space assumption). Renderer owns the window's lifetime
    // (init/shutdown/should_close) so this file never calls
    // InitWindow/CloseWindow directly.
    Renderer renderer;
    const std::string title = "ctf_client - " + args.name;
    if (!renderer.init(config::kMapWidthPx, config::kMapHeightPx, title.c_str())) {
        std::fprintf(stderr, "ctf_client: failed to open window\n");
        return 1;
    }
    SetTargetFPS(0); // uncapped - the client tick below is what must stay
                     // fixed-rate, not the render loop (checklist:
                     // "independent of raylib's render/frame rate").

    // --- Handshake steps 3-5: UDP_HELLO, wait for LOBBY_STATE/GAME_START.
    // net.poll() resends UDP_HELLO automatically every
    // config::kUdpHelloResendMs until first_snapshot_received() (README
    // §5.6 - "the hello can be lost"); send the first one explicitly here.
    net.send_udp_hello();
    auto last_lobby_hello = Clock::now();

    while (!renderer.should_close() && !net.game_started()) {
        net.poll();
        net.take_events(); // LobbyState/GameStart are already reflected in
                           // net.lobby_state()/state(); nothing else can
                           // arrive pre-GAME_START, so the drained events
                           // themselves aren't needed here.

        // KNOWN CROSS-FILE ISSUE (flagged, not fixed at the source - see
        // handoff notes): server/sim.cpp ticks and publishes
        // WORLD_SNAPSHOT unconditionally from startup, and
        // server/broadcast.cpp sends it to every client with a
        // registered UDP address - including ones still sitting in the
        // lobby. That first snapshot sets NetClient::first_snapshot_
        // received_, which permanently stops net.poll()'s automatic
        // UDP_HELLO resends (net_client.cpp's documented behavior for
        // "handshake complete"). With no PLAYER_INPUT sent pre-GAME_START
        // (correctly - README §5.4), nothing else refreshes the server's
        // last_input_tick, and the 3s silence timeout (README §5.7)
        // disconnects an idle lobby. Sending our own HELLO here,
        // independent of that flag, keeps the connection alive for
        // however long the lobby wait actually takes - this is the fix
        // scoped to what this file owns; bot/main.cpp needs the same
        // treatment for the same reason.
        if (Clock::now() - last_lobby_hello >=
            std::chrono::milliseconds(config::kUdpHelloResendMs)) {
            net.send_udp_hello();
            last_lobby_hello = Clock::now();
        }

        BeginDrawing();
        ClearBackground(Color{20, 20, 24, 255});
        char buf[128];
        std::snprintf(buf, sizeof(buf), "waiting for lobby... %u players",
                     static_cast<unsigned>(net.lobby_state().player_count));
        DrawText(buf, 20, 20, 20, RAYWHITE);
        if (net.is_host()) {
            DrawText("You are the host. Press SPACE to start.", 20, 50, 18,
                    YELLOW);
            if (IsKeyPressed(KEY_SPACE)) net.send_start_request();
        } else {
            DrawText("Waiting for host to start...", 20, 50, 18, GRAY);
        }
        EndDrawing();
    }

    if (renderer.should_close()) {
        return 0; // window closed before GAME_START; net's destructor
                 // closes the TCP socket -> server's recv()==0 path
                 // (README §5.7), a clean immediate disconnect.
    }

    // --- Main loop (README §6.2): fixed-timestep client tick decoupled
    // from render rate via an accumulator, so the input-send/prediction
    // rate stays ~30 Hz whether raylib renders at 30, 500, or 10 FPS
    // (checklist: "independent of raylib's render/frame rate if those
    // differ"). Mirrors the intent of server/sim.cpp's tick loop (README
    // §3.2) without blocking the render thread, since this process has
    // exactly one thread and it must also pump the window.
    Prediction prediction;
    Interpolation interpolation;

    // seq -> send timestamp, for a real (not derived/placeholder) RTT
    // metric on the HUD (README §6.6). Trimmed in lockstep with the acked
    // seq so it never grows unbounded, mirroring Prediction's own history
    // trim in on_snapshot().
    std::deque<std::pair<uint32_t, Clock::time_point>> send_times;
    double last_rtt_ms = 0.0;

    // Tracks whether the local player is currently alive, per the most
    // recent snapshot reconciled against. Gates on_local_tick() below:
    // dead players "receive no input" (README §7.4), and the server
    // already drops PLAYER_INPUT for a dead player_id (server/sim.cpp's
    // drain_inbound: `if (!p->alive) break;`) - predicting movement
    // locally while dead would run local_state_ ahead of an authority
    // that never moves, i.e. a ghost walking around a corpse. This is the
    // "dead" gating decision flagged as open before 3.5; resolved here in
    // main.cpp rather than in prediction.cpp, since Prediction is
    // deliberately movement-only with no concept of "alive" by design
    // (README §6.5, §7.2's exact PlayerMotion signature).
    bool locally_alive = true;
    WorldSnapshot latest_snapshot;
    bool have_snapshot = false;

    using namespace std::chrono;
    const auto tick_duration = nanoseconds(config::kTickDurationNs);
    auto accumulator = nanoseconds(0);
    auto last_time = Clock::now();

    // Actual-tick-rate HUD metric (README §6.6): ticks counted per
    // rolling wall-clock second.
    int ticks_this_second = 0;
    double actual_tick_rate_hz = 0.0;
    auto second_start = Clock::now();

    while (!renderer.should_close()) {
        net.poll();

        for (const GameEvent& ev : net.take_events()) {
            renderer.on_event(ev);
        }
        for (const WorldSnapshot& snap : net.take_snapshots()) {
            latest_snapshot = snap;
            have_snapshot = true;
            interpolation.push_snapshot(snap);
            prediction.on_snapshot(snap, net.player_id());

            // RTT: look up when the newly-acked seq was sent. Anything
            // older than the ack (input redundancy re-sends an already-
            // acked seq range) is stale and dropped without contributing
            // an RTT sample.
            const uint32_t ack = snap.last_input_seq;
            while (!send_times.empty() && send_times.front().first <= ack) {
                if (send_times.front().first == ack) {
                    last_rtt_ms = duration<double, std::milli>(
                                     Clock::now() - send_times.front().second)
                                     .count();
                }
                send_times.pop_front();
            }

            if (const PlayerState* me = find_player(snap, net.player_id())) {
                locally_alive = me->alive;
            }
        }

        // Fixed-timestep accumulator (see comment above the loop). Capped
        // at a handful of catch-up ticks per frame, mirroring
        // server/sim.cpp §3.2's "resync rather than spiral" rule - a
        // stalled render frame (e.g. window drag) must not fire a burst
        // of hundreds of ticks once it resumes.
        const auto now = Clock::now();
        accumulator += duration_cast<nanoseconds>(now - last_time);
        last_time = now;
        int catchup = 0;
        while (accumulator >= tick_duration && catchup < 5) {
            InputCmd cmd = sample_input();
            cmd.aim_angle = aim_angle_towards_mouse(prediction.local_state().position);

            if (locally_alive) {
                prediction.on_local_tick(cmd);

                // Last config::kInputRedundancy history entries, ascending
                // seq (README §5.4's wire order: base_seq is the newest,
                // inputs[0] the oldest of the three - matches
                // server/net_server.cpp's `cmd.seq = base_seq - count + 1
                // + i`).
                const auto& history = prediction.history();
                const int redundancy = config::kInputRedundancy;
                const int start =
                    static_cast<int>(history.size()) > redundancy
                        ? static_cast<int>(history.size()) - redundancy
                        : 0;
                InputCmd batch[config::kInputRedundancy];
                uint8_t count = 0;
                for (int i = start; i < static_cast<int>(history.size()); ++i) {
                    batch[count++] = history[static_cast<size_t>(i)].input;
                }
                net.send_input(prediction.current_seq(), batch, count);
                send_times.push_back({prediction.current_seq(), Clock::now()});
            }
            // Dead: no on_local_tick(), no PLAYER_INPUT sent this tick -
            // "receive no input" (README §7.4). local_state_ stays frozen
            // wherever on_snapshot() last snapped it (the death position)
            // until a future snapshot reports alive again, at which point
            // reconciliation snaps it to the respawn position.

            accumulator -= tick_duration;
            ++catchup;
            ++ticks_this_second;
        }
        if (accumulator > tick_duration * 3) {
            // Fell far behind (e.g. window minimized/dragged): resync
            // rather than keep burning catch-up ticks next frame too,
            // same resync-not-catch-up rule server/sim.cpp applies
            // (README §3.2).
            accumulator = nanoseconds(0);
        }

        if (Clock::now() - second_start >= seconds(1)) {
            actual_tick_rate_hz = static_cast<double>(ticks_this_second);
            ticks_this_second = 0;
            second_start = Clock::now();
        }

        interpolation.advance(GetFrameTime());

        if (have_snapshot) {
            HudMetrics hud;
            hud.rtt_ms = last_rtt_ms;
            hud.unacked_input_count =
                static_cast<uint32_t>(prediction.history().size());
            hud.misprediction_rate =
                latest_snapshot.tick > 0
                    ? static_cast<double>(prediction.mispredictions()) /
                         static_cast<double>(latest_snapshot.tick)
                    : 0.0;
            hud.snapshot_buffer_depth =
                static_cast<uint32_t>(interpolation.snapshot_count());
            hud.actual_tick_rate_hz = actual_tick_rate_hz;

            renderer.draw_frame(latest_snapshot, net.player_id(), prediction,
                               interpolation, hud);
        }
    }

    net.disconnect(); // explicit close -> server sees TCP recv()==0
                      // immediately (README §5.7), not the 3s UDP-silence
                      // path. ~NetClient() would do this anyway on scope
                      // exit; explicit here to document which of the two
                      // disconnect detectors this path exercises.
    return 0;
}
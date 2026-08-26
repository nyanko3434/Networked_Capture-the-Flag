// ctf_bot entry point: a headless client that reuses net_client +
// prediction, minus raylib and minus interpolation (README §4, §11 - bots
// don't render anyone, including remote players).
//
// Process model, not spelled out by README/the guide - a deliberate choice,
// flagged here rather than assumed silently: `--count N` forks N
// independent child processes, each running exactly one bot connection,
// rather than N bot objects cooperatively multiplexed inside one process.
// This is what makes the guide's own acceptance criterion possible as
// written - "Killing a subset of bots with SIGKILL mid-run" only makes
// sense if a "bot" is a real, individually-killable OS process; SIGKILL
// aimed at one multi-bot process would take all of them down at once, which
// isn't "a subset." Standard fork() semantics also mean an interactive
// Ctrl-C on the parent's foreground process group reaches every child
// automatically, with no explicit signal-forwarding code needed here.

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "game_config.h"
#include "map.h"
#include "net_client.h"
#include "prediction.h"
#include "bot_ai.h"
#include "protocol.h"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct BotArgs {
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    int count = 1;
};

BotArgs parse_args(int argc, char** argv) {
    BotArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--count" && i + 1 < argc) {
            args.count = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: ctf_bot [--host HOST] [--port N] [--count N]\n");
            std::exit(0);
        }
    }
    if (args.count < 1) args.count = 1;
    return args;
}

// Same idle/repeat-last-history pattern as client/main.cpp's
// send_current_input(), and for the same reason: PLAYER_INPUT must keep
// flowing every tick regardless of alive/match state, or a bot sitting dead
// waiting to respawn (up to 90 ticks = exactly the 3s UDP silence timeout,
// README §7.5/§8) risks a false disconnect right as it respawns. See the
// long comment in client/main.cpp for the full reasoning - identical logic,
// duplicated rather than shared since it's ~10 lines and pulling it into
// client_core would mean giving prediction.cpp or net_client.cpp a
// dependency on the other, which neither needs otherwise.
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

const ctf::PlayerState* find_self(const ctf::WorldSnapshot& snap, uint8_t id) {
    for (uint8_t i = 0; i < snap.player_count; ++i) {
        if (snap.players[i].id == id) return &snap.players[i];
    }
    return nullptr;
}

// One bot's full lifetime: connect, (self-elect to start an otherwise-empty
// lobby - see the comment below), then tick at a fixed 30Hz until stopped.
// Returns a process exit code.
int run_single_bot(const BotArgs& args, int bot_index) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "bot%d", bot_index);

    ctf::NetClient net;
    if (!net.connect_and_join(args.host, args.port, name_buf)) {
        std::fprintf(stderr, "ctf_bot[%s]: failed to join %s:%u\n", name_buf,
                     args.host.c_str(), static_cast<unsigned>(args.port));
        return 1;
    }
    const uint8_t my_id = net.player_id();

    ctf::Prediction prediction;
    ctf::WorldSnapshot latest_snapshot{};
    bool have_snapshot = false;

    // Wait for other bots to join before requesting start. The first bot
    // becomes host; if it sends start_request immediately, the match fails
    // to begin (< 2 players) and never retries. A 2-second delay lets all
    // forked children connect first.
    auto connect_time = std::chrono::steady_clock::now();
    bool can_start = false;

    // BotAI needs to know the player's team, which we learn from GAME_START.
    // Start with a placeholder; re-created once we know the team.
    ctf::Team my_team = ctf::Team::Red;
    std::unique_ptr<ctf::BotAI> ai;

    const double tick_dt = 1.0 / ctf::config::kTickRateHz;
    double accumulator = 0.0;
    auto last_time = std::chrono::steady_clock::now();

    while (!g_stop) {
        const auto now = std::chrono::steady_clock::now();
        const double frame_dt =
            std::chrono::duration<double>(now - last_time).count();
        last_time = now;
        accumulator += frame_dt;

        net.poll();
        for (const auto& snap : net.take_snapshots()) {
            latest_snapshot = snap;
            have_snapshot = true;
            prediction.on_snapshot(snap, my_id);
        }
        // Check GAME_START event to learn our team.
        for (const auto& ev : net.take_events()) {
            if (auto* gs = std::get_if<ctf::protocol::MsgGameStart>(&ev)) {
                for (uint8_t i = 0; i < gs->player_count; ++i) {
                    if (gs->ids[i] == my_id) {
                        my_team = gs->teams[i];
                        ai = std::make_unique<ctf::BotAI>(
                            static_cast<uint32_t>(bot_index) * 7919u +
                                static_cast<uint32_t>(::getpid()),
                            my_id, my_team);
                        break;
                    }
                }
            }
        }

        // Self-elect to start the match if this bot happens to be the host,
        // but only after a 2-second delay to let all bots connect first.
        if (!net.game_started() && net.is_host()) {
            if (!can_start) {
                const auto elapsed = std::chrono::steady_clock::now() - connect_time;
                if (std::chrono::duration<double>(elapsed).count() >= 2.0)
                    can_start = true;
            }
            if (can_start) net.send_start_request();
        }

        int ticks_this_frame = 0;
        while (accumulator >= tick_dt && ticks_this_frame < 200) {
            accumulator -= tick_dt;
            ++ticks_this_frame;

            const bool alive_in_match = [&] {
                if (!net.game_started() || !have_snapshot) return false;
                const ctf::PlayerState* self = find_self(latest_snapshot, my_id);
                return self != nullptr && self->alive;
            }();

            if (alive_in_match && ai) {
                prediction.on_local_tick(ai->tick(latest_snapshot));
            } else if (alive_in_match) {
                // Team not yet known — send idle.
                prediction.on_local_tick(ctf::InputCmd{0, 0});
            }
            send_current_input(net, prediction); // always - see comment above
        }

        // Not a real-time renderer - no need to spin faster than the
        // network layer can usefully produce work. Sleeping a fraction of
        // a tick keeps this from busy-looping at 100% CPU per bot while
        // still catching up promptly via the accumulator above.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    net.disconnect();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    BotArgs args = parse_args(argc, argv);

    if (args.count == 1) {
        return run_single_bot(args, 0);
    }

    std::printf("ctf_bot: spawning %d bot processes against %s:%u\n", args.count,
               args.host.c_str(), static_cast<unsigned>(args.port));

    std::vector<pid_t> children;
    children.reserve(static_cast<size_t>(args.count));
    for (int i = 0; i < args.count; ++i) {
        const pid_t pid = fork();
        if (pid < 0) {
            std::perror("ctf_bot: fork");
            continue;
        }
        if (pid == 0) {
            // Child: run exactly one bot for the rest of this process's life.
            std::exit(run_single_bot(args, i));
        }
        children.push_back(pid);
        // Small stagger so the server can accept each connection before
        // the next one arrives. Without this, all children connect
        // simultaneously and some may get dropped under load.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::printf("ctf_bot: all %zu bot processes spawned, waiting...\n",
               children.size());

    // The parent doesn't touch any socket itself - it only supervises. A
    // no-op-ish handler here just keeps the parent alive through Ctrl-C
    // long enough for waitpid() below to reap children after their own
    // graceful shutdown (default SIGINT disposition would otherwise kill
    // the parent immediately, orphaning still-cleaning-up children).
    std::signal(SIGINT, on_signal);

    int exit_code = 0;
    for (pid_t pid : children) {
        int status = 0;
        if (waitpid(pid, &status, 0) == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) exit_code = 1;
        }
    }
    return exit_code;
}

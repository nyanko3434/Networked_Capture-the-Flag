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

// Simple but functional bot AI: navigates toward objectives, shoots enemies.
// Has wall avoidance and imperfect aim so it doesn't feel robotic.
class BotAI {
public:
    explicit BotAI(uint32_t seed, uint8_t player_id, ctf::Team team)
        : rng_(seed), my_id_(player_id), my_team_(team),
          aim_spread_rng_(0.0f, 0.3f),  // ~17 degrees of random spread
          fire_chance_(0.4) {}           // 40% chance to fire per tick when enemy visible

    ctf::InputCmd tick(const ctf::WorldSnapshot& snap) {
        const ctf::PlayerState* self = find_self(snap);
        if (self == nullptr) return ctf::InputCmd{0, 0};

        const float my_x = static_cast<float>(self->motion.position.x) / ctf::config::kFixedScale;
        const float my_y = static_cast<float>(self->motion.position.y) / ctf::config::kFixedScale;
        const ctf::Vec2Fixed my_pos = self->motion.position;

        // Find nearest enemy for shooting.
        float nearest_dist = 99999.0f;
        float enemy_dx = 0, enemy_dy = 0;
        bool have_enemy = false;
        for (uint8_t i = 0; i < snap.player_count; ++i) {
            const auto& p = snap.players[i];
            if (p.id == my_id_ || p.team == my_team_ || !p.alive) continue;
            const float ex = static_cast<float>(p.motion.position.x) / ctf::config::kFixedScale;
            const float ey = static_cast<float>(p.motion.position.y) / ctf::config::kFixedScale;
            const float dx = ex - my_x;
            const float dy = ey - my_y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < nearest_dist) {
                nearest_dist = dist;
                enemy_dx = dx;
                enemy_dy = dy;
                have_enemy = true;
            }
        }

        // Determine target.
        float target_x, target_y;
        if (self->carrying_flag) {
            const auto& base = (my_team_ == ctf::Team::Red)
                ? ctf::kRedBasePosition : ctf::kBlueBasePosition;
            target_x = static_cast<float>(base.x) / ctf::config::kFixedScale;
            target_y = static_cast<float>(base.y) / ctf::config::kFixedScale;
        } else {
            const auto& flag_pos = (my_team_ == ctf::Team::Red)
                ? resolve_flag_pos(snap, ctf::Team::Blue)
                : resolve_flag_pos(snap, ctf::Team::Red);
            target_x = static_cast<float>(flag_pos.x) / ctf::config::kFixedScale;
            target_y = static_cast<float>(flag_pos.y) / ctf::config::kFixedScale;
        }

        // Movement with wall avoidance: try all 4 directions, pick the
        // one that gets closest to the target. Simple greedy approach
        // that handles corners and narrow passages.
        const float dx = target_x - my_x;
        const float dy = target_y - my_y;
        const int32_t step = ctf::config::kMoveSpeedFpPerTick;
        uint8_t buttons = 0;

        if (std::abs(dx) > 4.0f || std::abs(dy) > 4.0f) {
            struct Candidate { uint8_t btn; float dist; bool ok; };
            Candidate best = {0, 1e9f, false};

            // Test each of the 4 cardinal directions.
            const uint8_t dirs[] = {ctf::kInputRight, ctf::kInputLeft, ctf::kInputDown, ctf::kInputUp};
            const int32_t offsets_x[] = {step, -step, 0, 0};
            const int32_t offsets_y[] = {0, 0, step, -step};

            for (int d = 0; d < 4; ++d) {
                ctf::Vec2Fixed test = my_pos;
                test.x += offsets_x[d];
                test.y += offsets_y[d];
                if (map_.aabb_collides(test)) continue;

                const float nx = my_x + static_cast<float>(offsets_x[d]) / ctf::config::kFixedScale;
                const float ny = my_y + static_cast<float>(offsets_y[d]) / ctf::config::kFixedScale;
                const float ndx = target_x - nx;
                const float ndy = target_y - ny;
                const float ndist = ndx * ndx + ndy * ndy; // squared, no sqrt needed

                if (ndist < best.dist) {
                    best = {dirs[d], ndist, true};
                }
            }

            // Also try diagonal (combined) if both axes have distance.
            if (std::abs(dx) > 4.0f && std::abs(dy) > 4.0f) {
                ctf::Vec2Fixed diag = my_pos;
                diag.x += (dx > 0 ? step : -step);
                diag.y += (dy > 0 ? step : -step);
                if (!map_.aabb_collides(diag)) {
                    const float nx = my_x + static_cast<float>(dx > 0 ? step : -step) / ctf::config::kFixedScale;
                    const float ny = my_y + static_cast<float>(dy > 0 ? step : -step) / ctf::config::kFixedScale;
                    const float ndx = target_x - nx;
                    const float ndy = target_y - ny;
                    const float ndist = ndx * ndx + ndy * ndy;
                    if (ndist < best.dist) {
                        uint8_t combo = 0;
                        if (dx > 0) combo |= ctf::kInputRight; else combo |= ctf::kInputLeft;
                        if (dy > 0) combo |= ctf::kInputDown; else combo |= ctf::kInputUp;
                        best = {combo, ndist, true};
                    }
                }
            }

            if (best.ok) buttons = best.btn;
        }

        // Aim and shoot with imperfection.
        uint16_t aim = 0;
        if (have_enemy && nearest_dist < 350.0f) {
            // Add random spread to aim.
            const float spread = aim_spread_rng_(rng_);
            const float angle = std::atan2(enemy_dy, enemy_dx) + spread;
            aim = static_cast<uint16_t>(angle / (2.0f * 3.14159265f) * 65536.0f);
            // Don't fire every tick — makes bots feel more human.
            if (fire_chance_(rng_)) buttons |= ctf::kInputFire;
        } else {
            // Aim toward movement direction when no enemy.
            if (buttons & (ctf::kInputLeft | ctf::kInputRight | ctf::kInputUp | ctf::kInputDown)) {
                float mx = 0, my = 0;
                if (buttons & ctf::kInputRight) mx += 1;
                if (buttons & ctf::kInputLeft) mx -= 1;
                if (buttons & ctf::kInputDown) my += 1;
                if (buttons & ctf::kInputUp) my -= 1;
                aim = static_cast<uint16_t>(
                    std::atan2(my, mx) / (2.0f * 3.14159265f) * 65536.0f);
            }
        }

        return ctf::InputCmd{buttons, aim};
    }

private:
    const ctf::PlayerState* find_self(const ctf::WorldSnapshot& snap) {
        for (uint8_t i = 0; i < snap.player_count; ++i)
            if (snap.players[i].id == my_id_) return &snap.players[i];
        return nullptr;
    }

    ctf::Vec2Fixed resolve_flag_pos(const ctf::WorldSnapshot& snap, ctf::Team flag_team) {
        const bool is_red = (flag_team == ctf::Team::Red);
        const auto state = is_red ? snap.flag_state_red : snap.flag_state_blue;
        const auto carrier = is_red ? snap.flag_carrier_red : snap.flag_carrier_blue;
        if (state == ctf::FlagState::Carried && carrier < ctf::config::kMaxPlayers) {
            for (uint8_t i = 0; i < snap.player_count; ++i)
                if (snap.players[i].id == carrier) return snap.players[i].motion.position;
        }
        return is_red ? ctf::kRedBasePosition : ctf::kBlueBasePosition;
    }

    std::mt19937 rng_;
    uint8_t my_id_;
    ctf::Team my_team_;
    ctf::Map map_;
    std::uniform_real_distribution<float> aim_spread_rng_;
    std::bernoulli_distribution fire_chance_;
};

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
    std::unique_ptr<BotAI> ai;

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
                        ai = std::make_unique<BotAI>(
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

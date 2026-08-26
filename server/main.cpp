// ctf_server entry point (README §9): parses --port --tick
// --snapshot-rate --poller, wires the two threads + eventfd, and shuts down
// cleanly on SIGINT.

#include <poll.h>
#include <signal.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "game_config.h"
#include "net_server.h"
#include "poller.h"
#include "queues.h"
#include "sim.h"

namespace {

std::atomic<bool> g_stop{false};

void on_sigint(int) { g_stop = true; }

struct ServerArgs {
    uint16_t port = 7777;
    int tick_rate = ctf::config::kTickRateHz;
    int snapshot_rate = ctf::config::kTickRateHz;
    std::string poller = "poll";
    bool delta_snapshots = true;
};

ServerArgs parse_args(int argc, char** argv) {
    ServerArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--tick" && i + 1 < argc) {
            args.tick_rate = std::atoi(argv[++i]);
        } else if (arg == "--snapshot-rate" && i + 1 < argc) {
            args.snapshot_rate = std::atoi(argv[++i]);
        } else if (arg == "--poller" && i + 1 < argc) {
            args.poller = argv[++i];
        } else if (arg == "--snapshots" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode != "delta" && mode != "full") {
                std::fprintf(stderr,
                             "ctf_server: --snapshots must be delta|full\n");
                std::exit(1);
            }
            args.delta_snapshots = (mode == "delta");
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: ctf_server [--port N] [--tick HZ] "
                "[--snapshot-rate HZ] [--snapshots delta|full] "
                "[--poller poll|epoll]\n");
            std::exit(0);
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    ServerArgs args = parse_args(argc, argv);

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN); // send() to a dead peer must not kill us

    // Exactly two threads (README §3.2): network + simulation.
    auto inbound = std::make_unique<ctf::InboundQueue>();
    auto outbound = std::make_unique<ctf::OutboundQueue>();

    std::unique_ptr<ctf::IPoller> poller =
        args.poller == "epoll" ? ctf::make_epoll_poller()
                               : ctf::make_poll_poller();

    ctf::NetServer net(args.port, *inbound, *outbound, std::move(poller));
    if (!net.start()) {
        std::fprintf(stderr, "ctf_server: failed to bind port %u\n",
                     static_cast<unsigned>(args.port));
        return 1;
    }

    ctf::Sim sim(*inbound, *outbound);
    sim.set_wake_fd(net.wake_fd()); // exists before either thread runs
    sim.set_tick_rate(args.tick_rate);
    sim.set_snapshot_rate(args.snapshot_rate);
    sim.set_delta_snapshots(args.delta_snapshots);

    std::printf(
        "ctf_server: listening tcp=%u udp=%u tick=%d snapshot=%d "
        "snap_mode=%s poller=%s\n",
        static_cast<unsigned>(net.local_port()),
        static_cast<unsigned>(net.udp_port()), args.tick_rate,
        args.snapshot_rate, args.delta_snapshots ? "delta" : "full",
        args.poller.c_str());
    std::fflush(stdout);

    std::thread net_thread([&] { net.run(); });
    std::thread sim_thread([&] { sim.run(); });

    while (!g_stop) {
        ::poll(nullptr, 0, 100); // idle until SIGINT
    }

    net.stop();
    sim.stop();
    net_thread.join();
    sim_thread.join();

    std::printf("ctf_server: clean shutdown\n");
    return 0;
}

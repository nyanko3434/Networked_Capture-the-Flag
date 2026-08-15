// ctf_server entry point. Scaffolding only: parses the flags described in
// README §9, prints what it parsed, and exits. Networking, the sim thread,
// and the tick loop are wired up in later weeks (README §11).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game_config.h"

namespace {

struct ServerArgs {
    uint16_t port = 7777;
    int tick_rate = ctf::config::kTickRateHz;
    int snapshot_rate = ctf::config::kTickRateHz;
    std::string poller = "poll";
};

ServerArgs parse_args(int argc, char** argv) {
    ServerArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--tick" && i + 1 < argc) {
            args.tick_rate = std::atoi(argv[++i]);
        } else if (arg == "--snapshot-rate" && i + 1 < argc) {
            args.snapshot_rate = std::atoi(argv[++i]);
        } else if (arg == "--poller" && i + 1 < argc) {
            args.poller = argv[++i];
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    ServerArgs args = parse_args(argc, argv);
    std::printf(
        "ctf_server: port=%u tick=%d snapshot-rate=%d poller=%s\n",
        static_cast<unsigned>(args.port), args.tick_rate, args.snapshot_rate,
        args.poller.c_str());
    return 0;
}

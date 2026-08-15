// ctf_bot entry point: a headless client that reuses net_client +
// prediction, minus raylib (README §4, §11). Scaffolding only: parses the
// flags described in README §9, prints what it parsed, and exits.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

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
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    BotArgs args = parse_args(argc, argv);
    std::printf("ctf_bot: host=%s port=%u count=%d\n", args.host.c_str(),
                static_cast<unsigned>(args.port), args.count);
    return 0;
}

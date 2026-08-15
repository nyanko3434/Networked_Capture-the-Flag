// ctf_client entry point. Scaffolding only: parses the flags described in
// README §9, prints what it parsed, and exits. Networking, prediction,
// interpolation, and rendering are wired up in later weeks (README §11).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

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

} // namespace

int main(int argc, char** argv) {
    ClientArgs args = parse_args(argc, argv);
    std::printf("ctf_client: host=%s port=%u name=%s\n", args.host.c_str(),
                static_cast<unsigned>(args.port), args.name.c_str());
    return 0;
}

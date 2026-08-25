# manual.md — Build & Run Guide

Practical instructions for building, testing, and running this project on
**Arch Linux**. For design details see `README.md`; for task tracking see
`implementation_guide.md` and `AGENTS.md`.

---

## 1. Prerequisites (Arch Linux)

```bash
sudo pacman -S --needed base-devel cmake git ninja \
    libx11 libxrandr libxinerama libxcursor libxi
```

- **raylib:** you do NOT need `pacman -S raylib`. The build fetches its own
  pinned copy (raylib 5.0) from GitHub via CMake FetchContent. The
  pacman-installed raylib (6.0) is a different version and is not used.
  If it is already installed, it simply sits unused — leave it.
- **Compiler note:** this machine defaults to clang. GCC accepts two
  non-standard constructs that clang rejects; both have been fixed in the
  source (`server/net_server.h`, `tests/test_net_server.cpp`). Any compiler
  works now.

---

## 2. First-time build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)
```

Produces three binaries:

| Binary | Purpose |
|---|---|
| `build/ctf_server` | authoritative game server |
| `build/ctf_client` | graphical client (raylib window) |
| `build/ctf_bot`    | headless client for load testing |

Server-only machines (no X11 headers) can skip the client entirely:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCTF_BUILD_CLIENT=OFF
```

### Slow-GitHub workaround (needed on this network)

The very first configure clones raylib (~35 MB) from GitHub. On a slow
connection this times out. Workaround — clone once manually, then point CMake
at the local copy:

```bash
git clone --depth 1 --branch 5.0 https://github.com/raysan5/raylib.git ~/Documents/Projects/temp/raylib-test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DFETCHCONTENT_SOURCE_DIR_RAYLIB=$HOME/Documents/Projects/temp/raylib-test
cmake --build build -j$(nproc)
```

Once configured, the path is cached in `build/CMakeCache.txt` and plain
`cmake --build build` keeps working. The local clone currently lives at
`~/Documents/Projects/temp/raylib-test` — do not delete it, or the next
fresh configure will re-download from GitHub.

---

## 3. After a code update — DO NOT reinstall raylib

raylib is fetched **once** and cached under `build/_deps/`. Pulling new code
changes nothing about it. The routine is always just:

```bash
git pull
cmake --build build -j$(nproc)     # recompiles only what changed
```

Re-run CMake configure (`cmake -S . -B build ...`) **only** if:
- someone changes `CMakeLists.txt` (new source files, new options), or
- you deleted the `build/` directory.

If `build/` was deleted AND your network to GitHub is still slow, repeat the
workaround from §2 using the permanent local clone at
`~/Documents/Projects/temp/raylib-test`.

---

## 4. Run the tests

```bash
./build/tests/ctf_tests
# expected: 120 test cases / 12229 assertions, Status: SUCCESS!
```

Sanitizer builds (optional):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCTF_BUILD_CLIENT=OFF -DCTF_SANITIZE_ADDRESS=ON
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCTF_BUILD_CLIENT=OFF -DCTF_SANITIZE_THREAD=ON
```
(ASan and TSan cannot be combined.)

---

## 5. Running the game

Terminal 1 — server:

```bash
./build/ctf_server --port 7777 --tick 30
```

Useful server flags:

| Flag | Meaning |
|---|---|
| `--port N` | TCP listen port |
| `--tick N` | simulation tick rate (default 30) |
| `--snapshot-rate N` | UDP snapshots per second (lower = choppier demo) |
| `--poller poll\|epoll` | I/O multiplexer backend |

Startup prints `listening tcp=7777 udp=NNNNN` — the UDP port is ephemeral;
clients learn it automatically during the handshake. Never hardcode it.

Terminal 2+ — clients:

```bash
./build/ctf_client --host 127.0.0.1 --port 7777 --name player1   # HOST
./build/ctf_client --host 127.0.0.1 --port 7777 --name player2
```

- The **first joiner is HOST**; the match starts when the host sends
  START_REQUEST from their client.
- Roster max is 10; joining mid-match is rejected (`InProgress`).
- Ctrl-C stops the server cleanly (exit 0).

Headless bots (no window needed):

```bash
./build/ctf_bot --host 127.0.0.1 --port 7777 --count 9
```

LAN play: replace `127.0.0.1` with the server machine's LAN IP
(`ip addr` to find it). Port 7777/tcp must be reachable.

---

## 6. Tooling

| Tool | Use |
|---|---|
| `tools/loadgen.py` | headless Python reference client — protocol smoke test without building anything |
| `tools/netem.sh on [--delay MS] [--loss PCT]` | simulate bad network on loopback (needs root); `off` to remove |
| `tools/bench_pollers.sh` | poll-vs-epoll benchmark → `docs/benchmark.md` |

Example adverse-network session:

```bash
sudo ./tools/netem.sh on --delay 80 --jitter 20 --loss 5
# run server + clients as usual, observe prediction/interpolation behavior
sudo ./tools/netem.sh off
```

---

## 7. Troubleshooting

| Symptom | Fix |
|---|---|
| `error: redefinition of 'protocol'` | already fixed in source; make sure you pulled latest |
| `'auto' not allowed in function prototype` | already fixed; means you're on old sources with clang |
| Configure hangs at "Cloning into 'raylib-src'" | slow GitHub — use §2 workaround |
| Client fails at startup about X11/display | install X11 dev packages (§1) or use `-DCTF_BUILD_CLIENT=OFF`; on Wayland it runs through XWayland by default |
| Client connects but sees no players | client must send UDP_HELLO until first snapshot; check firewall allows UDP on the server's ephemeral port |
| `JOIN_REJECT reason=InProgress` | match already running; wait for MATCH_END then host restarts |
| Shots feel offset/quantized | fixed on branch `fix/shot-origin-and-ray-direction` (tracer origin = box center; full 16-bit aim direction) — merge the PR if not yet in your branch |

Git note: `gh` CLI is not installed on this machine, so PRs are opened via
the compare URL GitHub prints after `git push`. Install with
`sudo pacman -S github-cli` if you want `gh pr create` to work.

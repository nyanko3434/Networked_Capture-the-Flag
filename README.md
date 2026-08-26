# Networked Capture-the-Flag

A real-time, LAN-based, two-team Capture-the-Flag game for 2–10 players,
built as a network-programming project (ENCT 386). One authoritative server
runs the entire simulation; clients capture input and render.

The game is the vehicle — the engineering focus is socket programming,
concurrent server design, custom protocol design, and latency compensation.
Graphics are deliberately minimal.

**Platform:** Linux · C++17 · POSIX sockets · raw pthreads · raylib

## How it works

```
                       LAN
  [client 0] ──TCP──┐         ┌──TCP── [client n]
  [client 0] ──UDP──┤         ├──UDP── [client n]
                    ▼         ▼
              ┌──────────────────────┐
              │  Authoritative server │
              │  30 Hz fixed tick     │
              └──────────────────────┘
```

- **TCP** carries one-time critical events (lobby, kills, flag events, match end).
- **UDP** carries high-frequency player input and world snapshots.
- Exactly **two server threads** (network + simulation) communicating through
  two mutex-guarded queues; all game state is authoritative server-side.
- **Fixed-point integer physics** shared verbatim by client and server, so
  client-side prediction matches the authority exactly — no float divergence.
- Client applies **prediction + reconciliation** for its own player and
  **interpolation** for everyone else; debug toggles make each visible.
- **Delta-compressed snapshots**: field-level UDP diffs against the previous
  publish with periodic full keyframes (~46% bandwidth reduction, measured).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)
```

Produces `ctf_server`, `ctf_client`, and `ctf_bot` (headless load client).
Detailed setup (Arch Linux prerequisites, slow-GitHub/raylib workaround)
lives in [docs/manual.md](docs/manual.md).

## Run

```bash
./build/ctf_server --port 7777 --tick 30            # terminal 1
./build/ctf_client --host 127.0.0.1 --port 7777 --name p1   # HOST, terminal 2
./build/ctf_client --host 127.0.0.1 --port 7777 --name p2   # terminal 3
```

Useful server flags: `--snapshot-rate N`, `--snapshots delta|full`,
`--poller poll|epoll`. Headless bots: `./build/ctf_bot --host ... --count 9`.

## Tests

```bash
./build/tests/ctf_tests    # doctest suite: 135 cases / ~16.4k assertions
```

Sanitizer builds (`CTF_SANITIZE_ADDRESS=ON`, `CTF_SANITIZE_THREAD=ON`) and
the full acceptance criteria are described in the docs below.

## Documentation

All design documents live in [`docs/`](docs/):

| File | Purpose |
|---|---|
| [`docs/README.md`](docs/README.md) | Full design reference — every decision and its rationale |
| [`docs/implementation_guide.md`](docs/implementation_guide.md) | Ordered task checklist per owner with acceptance tests |
| [`docs/optimization.md`](docs/optimization.md) | Network optimizations: what, why, measured results |
| [`docs/manual.md`](docs/manual.md) | Build & run guide, troubleshooting |
| [`docs/HANDOFF.md`](docs/HANDOFF.md) | Byte-level protocol cheat sheet for client work |
| [`docs/benchmark.md`](docs/benchmark.md) | poll vs epoll recorded numbers |
| [`docs/benchmark_snapshots.md`](docs/benchmark_snapshots.md) | full vs delta snapshot bandwidth |

## Team

- Bibidh Subedi — server (`server/`, `shared/`)
- Nayan Khusu — client, bot, tooling (`client/`, `bot/`, `tools/`)

# Project Working Notes — Networked Capture-the-Flag

Reference file for AI-assisted development sessions. Bibidh Subedi owns the
server (`server/`); Nayan Khusu owns the client (`client/`, `bot/`, tooling).

## Primary documents (read these before any task)

| File | Purpose |
|---|---|
| `docs/README.md` | Single source of truth for every design decision. If a question isn't answered by a task's referenced section, the answer is here. |
| `docs/implementation_guide.md` | Ordered task checklist per owner with acceptance criteria stated as concrete tests. Work is tracked against §1 (shared) and §2 (Bibidh — server). |
| `AGENTS.md` | This file — execution plan, workflow rules, progress state. Kept at the repo root so AI coding tools auto-discover it. |

## Git workflow (imperative — no exceptions)

- **NEVER push directly to `main`. Ever.**
- Always:
  1. Create a feature branch off the latest `main`:
     `git checkout main && git pull && git checkout -b <feature-branch>`
     Branch naming: `<area>/<short-desc>` (e.g. `server/sim-tick-loop`,
     `shared/bytebuffer-tests`).
  2. Commit work on that branch.
  3. Push to GitHub: `git push -u origin <feature-branch>`.
  4. Open a PR against `main` (`gh pr create`) and merge via PR only.
- One branch per logical unit of work; keep PRs reviewable.
- Never commit secrets, build artifacts, or `build/`.

## Execution plan (2-day crunch, unit-test-driven)

TDD discipline per module: failing tests first → implement → green → next.
Every phase must leave `ctest` green before moving on. Existing header APIs
stay frozen (Nayan builds against them).

### Phase 0 — Test harness (~30 min)
- Vendor doctest single header into `tests/`.
- Rework `tests/CMakeLists.txt`: one `ctf_tests` target, explicit source lists.
- ASan/TSan build presets (acceptance criteria require sanitizer verification).

### Phase 1 — shared/ foundations (~4 h)
1. `bytebuffer` — round-trip (boundary values), truncation→`underflow`,
   overflow→flag, ASan clean.
2. `map` — left-right mirror property, AABB overlap X-only/Y-only, no sqrt/fp.
3. `movement` — determinism (10k random pairs ×2 byte-identical), diagonal
   normalization (181/256), X-then-Y corner resolution, no-float grep.
4. `protocol` — round-trip all 18 message types, malformed-packet fuzz
   (1-byte datagram / bad magic / bad version / truncated),
   snapshot `last_input_seq` patch test, TCP frame reassembly split-read test,
   4 KB overflow cap. Plus `net_util` (`set_nonblocking`, `send_all`,
   `recv_framed`).

### Phase 2 — server plumbing (~3 h)
1. `queues` — RAII guard early-return test, N-producer stress under TSan;
   mutexes appear ONLY here (grep-enforced).
2. `client_registry` — add/find-by-id/find-by-fd/remove, no stale pointers,
   no mutex.
3. `poller_poll` — bounded-timeout empty wait (no spin), read-ready on peer
   write, POLLOUT toggle on/off. Tested over pipes/socketpairs.

### Phase 3 — game logic (~6 h)
4. `lobby` — 11th join rejected "full", balanced teams (≤1 size delta across
   N shuffles), host promotion on lobby leave, post-MATCH_END restart,
   START_REQUEST host-only.
5. `sim` — pure single-step state machine (`run()` is a thin wrapper around
   testable `tick()`):
   - ring buffer of 8: exactly one input popped/tick; empty→zero-movement +
     ack unchanged; full→drop oldest
   - fixed tick order (drain→commands→input→movement→combat→flags→win→publish)
   - calls `shared::movement_step` directly (grep check, no reimplementation)
   - combat: friendly fire = no damage, wall occlusion via DDA, kill→drop
     flag at death spot→90-tick respawn at own base full health
   - flags: enemy pickup / own-dropped instant return / 450-tick auto-return /
     capture gated on own flag at base
   - win: MATCH_END at 3 captures or time limit, state frozen after;
     >3-tick stall resyncs deadline instead of catch-up

### Phase 4 — network layer (~4 h)
6. `broadcast` — two recipients' buffers differ ONLY in the patched 4-byte
   field (byte-diff test); TCP event fan-out reaches every client within one
   cycle.
7. `net_server` — extracted testable pieces driven by socketpairs:
   - UDP header validation rejects bad magic/version before any queue push
   - session-token mismatch on PLAYER_INPUT dropped
   - TCP `recv()==0` path removes client within one poll cycle
   - UDP-silence 3 s timeout path (independent detector)
   - 64 KB pending-output cap disconnects slow client
   - eventfd wake latency independent of poll timeout

### Phase 5 — wiring + extras (~2 h)
- `main.cpp`: `--port --tick --snapshot-rate --poller` flags, exactly two
  threads, eventfd wired before threads start, SIGINT clean shutdown smoke
  test.
- `poller_epoll` (only if everything else green): same IPoller interface,
  swappable at runtime via `--poller`.
- Deferred: load benchmarks, Wireshark captures, netem (Nayan's side).

## Progress log

- [x] Phase 0 — test harness
- [x] Phase 1.1 bytebuffer · [x] 1.2 map · [x] 1.3 movement · [x] 1.4 protocol/net_util
- [x] Phase 2 queues · registry · poller_poll
- [x] Phase 3 lobby · sim
- [x] Phase 4 broadcast · net_server
- [x] Phase 5 main.cpp · poller_epoll

Build commands used:
- Normal: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCTF_BUILD_CLIENT=OFF`
- ASan:   same + `-DCTF_SANITIZE_ADDRESS=ON` (mirror build kept under /tmp/opencode)
- TSan:   same + `-DCTF_SANITIZE_THREAD=ON` (/tmp/opencode/ctf-tsan)
- Tests:  `cmake --build build --target ctf_tests && ./build/tests/ctf_tests`
- Server: `./build/ctf_server --port 7777 --tick 30 [--poller poll|epoll]`

Status: ALL PHASES COMPLETE + acceptance closure. 91 test cases /
~11.9k assertions green, ASan clean, TSan clean on queue stress, live
loopback smoke test OK for both poller backends.

Acceptance-closure sweep (post-Phase-5):
- [x] JOIN_REJECT reason InProgress fixed (was always Full) + live
      mid-match-join test over a real socket
- [x] Tick-rate stability + stall-resync tests (test_sim_loop.cpp;
      run(max_ticks) + debug_pre_tick hook; TIMER_ABSTIME pacing and
      no-catch-up verified)
- [x] Snapshot-rate decimation test + udp_snapshots_sent counter
- [x] Grep sweeps: desync literals CLEAN, mutexes only in queues.*,
      no floats in movement/map paths
- [x] Poll-vs-epoll benchmark: tools/loadgen.py (n=10 bots) +
      tools/bench_pollers.sh -> docs/benchmark.md (both ~31.8 Hz,
      CPU 0.35s vs 0.32s over 8s)
- Fixed en route: send_udp_snapshots now emits the 8-byte UDP header
  (README 5.3) with per-tick field; OutboundEvent carries tick.

Session notes (2026-08-25, client-integration debugging):
- Compiler strictness: system default is now clang 22, which rejects two
  constructs GCC tolerated. Fixed (committed on dev in a5ca21a):
  server/net_server.h self-referential `namespace protocol` alias removed;
  tests/test_net_server.cpp send_frame C++20 auto params converted to an
  explicit template. Any compiler works now.
- raylib FetchContent times out on this network (~75 KB/s to GitHub).
  Permanent local clone at ~/Documents/Projects/temp/raylib-test; fresh
  configures need -DFETCHCONTENT_SOURCE_DIR_RAYLIB=<that path> (cached in
  build/CMakeCache.txt afterwards). Full run instructions live in
  docs/manual.md — keep it current when build/run steps change.
- Combat fixes (branch fix/shot-origin-and-ray-direction, PR into dev):
  SHOT_FIRED origin was the box top-left corner, not the center the aim
  angle and ray are computed from; cast_ray quantized directions through a
  256-entry trig table ((aim_angle >> 8)) — replaced with full 16-bit-angle
  direction + integer remainder-accumulator march. Server-only combat math,
  no desync-path impact.
- Test counts grew since the status line above: currently 120 cases /
  12,229 assertions green.

Session notes (2026-08-26, network optimization — delta snapshots):
- Branch `optimization/network`. DELTA_SNAPSHOT (type 19) implemented:
  field-level diffs vs the previous publish; full WORLD_SNAPSHOT keyframe
  every 10th publish (`config::kSnapshotKeyframeInterval`). Server flag
  `--snapshots delta|full` (default delta). Protocol version bumped 1→2
  (stale v1 binaries now get JOIN_REJECT BadVersion).
- Loss model: client caches last applied snapshot; baseline mismatch drops
  deltas until the next keyframe (~333 ms worst case at 30 Hz). No NACKs.
- Files touched: shared/protocol.{h,cpp} (codec + version), game_config.h
  (keyframe interval), queues.h (UdpDeltaSnapshot event), sim.{h,cpp}
  (baseline + keyframe cadence in publish()), broadcast.{h,cpp} (type-byte
  param, default preserves old behavior), net_server.{h,cpp} (routing +
  udp_delta_snapshots_sent counter), main.cpp (--snapshots flag),
  client/net_client.{h,cpp} (delta decode onto cache; dispatch_udp_payload
  gained a tick param).
- Tests grew to 135 cases / 16,411 assertions, green normal + ASan. ASan
  also caught a pre-existing test bug: the 64KB-pending-cap test claimed
  payload_size=1024 on the 512-byte OutboundEvent array (fixed in test).
- tools/loadgen.py: VERSION=2, counts keyframes/deltas/udp_bytes, host
  detection from LOBBY_STATE (was: bot0 assumed host — racy under
  concurrent connects, matches silently never started), JOIN_ACCEPT frame
  parse no longer discards bytes after it (TCP stream desync fix), bots
  move intermittently (~50% duty) with frozen aim while idle.
- New tools/bench_bandwidth.sh -> docs/benchmark_snapshots.md:
  full 3.42 vs delta 1.85 KiB/s/client at n=10 (~46% reduction; scales
  with idle time — constant motion is the pathological worst case ~15%).

Session notes (2026-08-26, bot navigation — flow fields):
- ctf_bot no longer sticks on walls. Old memoryless greedy step-steering
  oscillated at obstacles; replaced with precomputed BFS flow fields
  (bot/flow_field.{h,cpp}): descend strictly downhill, per-axis physical
  feasibility probes, active corridor re-centering (off-center boxes
  clipping wall-gap corners was the wedge root cause).
- BotAI extracted from bot/main.cpp into bot/bot_ai.{h,cpp} as the
  raylib-free `bot_core` static lib (built unconditionally so tests run
  with CTF_BUILD_CLIENT=OFF). Target selection: carry->home, enemy raider
  has our flag->hunt carrier, else->enemy base. Wire format carries no
  dropped-flag position, so chasing dropped enemy flags is not derivable.
- New tests/test_bot_ai.cpp: BFS descent property, full-sim navigation
  from every spawn to the enemy base via shared movement_step, flag-carry
  run home, raider hunt, seed determinism. Suite now 141 cases /
  18,444 assertions, green normal + ASan (/tmp/opencode/ctf-asan2).
- Docs: docs/optimization.md §3 documents the navigation design; HANDOFF
  and README snapshot layout corrected 10->9 bytes per player.
- Root .md reorganization: README/implementation_guide/manual/HANDOFF
  moved to docs/ (git mv), new landing README.md at root, AGENTS.md stays
  at root by convention. Committed directly on dev (304536c) before this
  branch existed — see history.

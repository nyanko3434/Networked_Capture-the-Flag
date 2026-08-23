# Project Working Notes — Networked Capture-the-Flag

Reference file for AI-assisted development sessions. Bibidh Subedi owns the
server (`server/`); Nayan Khusu owns the client (`client/`, `bot/`, tooling).

## Primary documents (read these before any task)

| File | Purpose |
|---|---|
| `README.md` | Single source of truth for every design decision. If a question isn't answered by a task's referenced section, the answer is here. |
| `implementation_guide.md` | Ordered task checklist per owner with acceptance criteria stated as concrete tests. Work is tracked against §1 (shared) and §2 (Bibidh — server). |
| `AGENTS.md` | This file — execution plan, workflow rules, progress state. |

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

Status: ALL PHASES COMPLETE. 86 test cases / ~11.8k assertions green,
ASan clean, TSan clean on queue stress, live loopback smoke test OK for
both poller backends.

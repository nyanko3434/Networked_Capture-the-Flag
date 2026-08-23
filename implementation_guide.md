# CTF Implementation Guide — Task Breakdown by Owner

This document turns every decision already recorded in `README.md` into an ordered,
checkable task list. It does not re-explain or revisit any design decision — every
`(README §x.x)` reference points to the section that already made that call. If a
question comes up mid-task that isn't answered by the referenced section, the
answer is in `README.md`, not here.

Each task has a **checklist** (what to build) and **acceptance criteria** (how to
know it's done, stated as a concrete test with input, expected output, and failure
mode — not "add a unit test").

---

## 1. Shared work (joint, week 1)

Build in this order. Each item only depends on items above it.

### 1.0 `CMakeLists.txt`

**References:** README §4 (directory structure), §9 (Build and run)

Checklist:
- [ ] Root `CMakeLists.txt` builds the three targets named in §9:
      `ctf_server`, `ctf_client`, `ctf_bot`, from the directory layout in §4
      (`shared/`, `server/`, `client/`, `bot/`).
- [ ] `shared/` compiles once and links into all three targets (§4's comment on
      `shared/` — "compiled into server, client, AND bot").
- [ ] Explicit source file lists per target, not a wildcard/glob — a file added
      later must be added to `CMakeLists.txt` deliberately, not picked up
      silently.
- [ ] `build/` (or whatever out-of-source build directory is used) is excluded
      from version control.
- [ ] `ctf_bot` links only `shared/` and its own `bot/` sources plus whichever
      `client/` files it needs directly (per README §4's note that `bot/main.cpp`
      "reuses net_client + prediction") — no compiler include-path pointing into
      `client/` as a substitute for listing the actual source files it depends
      on.
- [ ] `--poller poll|epoll` and other runtime flags (§9) are program arguments,
      not build-time `#ifdef`s — both poller backends are compiled into
      `ctf_server` and selected at runtime (needed for the poll-vs-epoll
      benchmark in §2.9 to run both from one binary).

Acceptance criteria:
- `mkdir build && cd build && cmake .. && make -j` (the exact commands in §9)
  produces `ctf_server`, `ctf_client`, `ctf_bot` and nothing else unexpected.
- Adding a new `.cpp` file under `shared/` without editing `CMakeLists.txt`
  causes a build that does **not** pick it up (confirms no glob is in use) —
  the new file must be added to the list explicitly before it compiles.
- `ctf_bot`'s build succeeds with `client/render.cpp` deleted or renamed,
  proving it does not depend on the rendering path.
- A fresh clone with an untouched `build/` directory (i.e. checking `.gitignore`
  or equivalent) shows `build/` is not tracked.

### 1.1 `shared/game_config.h`

**References:** README §8 (Configuration table), §4 note ("every constant lives in
`game_config.h`")

Checklist:
- [ ] Declare every constant from the §8 table as a `constexpr`, grouped by section
      comment (`// timing`, `// movement`, `// combat`, `// map`) matching the
      table's own grouping.
- [ ] Tick rate, tile size, player size, move speed (in fp-units/tick), diagonal
      factor, max health, input redundancy count are all present — these are the
      seven "shared" constants the README calls out as the dangerous ones.
- [ ] Server-only constants (damage per shot, fire cooldown, respawn delay, flag
      auto-return, score to win, match time limit) and client-only (interpolation
      delay) are declared here too, even though only the seven shared ones are
      desync-dangerous — single source of truth, no exceptions.
- [ ] No constant of the same name or same numeric meaning exists anywhere else in
      the codebase (grep check before moving on).

Acceptance criteria:
- Every numeric literal that appears in the §8 table exists as a named constant in
  this file and nowhere else. Test: `grep -rn` for the literal values (e.g. `160`,
  `1365`, `181`, `256`, `34`, `450`) across `shared/`, `server/`, `client/`, `bot/`
  outside this file returns zero hits once movement/combat code is written.
- Failure mode this prevents: a movement constant duplicated in a client file
  (README §6.7, desync checklist item 1).

---

### 1.2 `shared/game_types.h`

**References:** README §5.5 (Snapshot layout), §7.4 (flag states), §5.1
(fixed-point positions)

Checklist:
- [ ] `PlayerState`: id, position (`i16 x, y` wire form / `int32` internal form
      per §5.1), aim_angle (`u16`), health (`u8`), flags bitfield (alive |
      carrying | team | firing) per §5.5's per-player 10-byte layout.
- [ ] `FlagState`: team, state enum (`at_base` / `carried` / `dropped`), position
      (used only when dropped).
- [ ] `WorldSnapshot`: header fields exactly as listed in §5.5 (last_input_seq,
      player_count, flag_carrier_red/blue, flag_state_red/blue, score_red/blue,
      seconds_remaining) plus a fixed-size array of `PlayerState`.
- [ ] `InputCmd`: buttons (`u8`), aim_angle (`u16`) — matches the per-input
      payload in the `PLAYER_INPUT` message (§5.4).
- [ ] `PlayerMotion`: position + velocity only, per the exact signature given in
      §7.2 (`movement_step` takes/returns this) — no weapon, health, team, or
      timer fields, matching the constraint that `movement_step` "must never
      fork."
- [ ] All position fields use `int32` internally (§5.1), never `float`.

Acceptance criteria:
- `PlayerMotion` has exactly the fields listed in §7.2 and no others. Test:
  compile-time check (e.g. `static_assert(sizeof(PlayerMotion) == expected)`) or
  manual field count — fails the review if a `flags` or `team` field sneaks in.
- No `float`/`double` type appears anywhere in this file. Test: `grep -n "float\|double"` returns nothing.

---

### 1.3 `shared/bytebuffer.h/.cpp`

**References:** README §5.2, §10 ("Serialization round-trip for every message
type", "Malformed-packet fuzzing")

Checklist:
- [ ] `ByteWriter`: `u8`, `u16` (via `htons`), `u32`, `i16`, `ok()`, `size()`, and
      an internal `overflow` flag that sets instead of writing past `cap`.
- [ ] `ByteReader`: `u8`, `u16`, `u32`, `i16`, `ok()`, and an internal
      `underflow` flag. Reads past the end return zero and set `underflow` rather
      than reading out of bounds.
- [ ] No `memcpy` of a struct onto the wire anywhere in this file (§5.3 — "Never
      `memcpy` a struct onto the wire").

Acceptance criteria:
- **Round-trip test:** write one value of every supported type (`u8`, `u16`,
  `u32`, `i16`, including boundary values `0`, max, and negative for `i16`) with
  `ByteWriter`, read them back in the same order with `ByteReader`; every value
  is bit-identical to the input and `reader.ok()` is `true` after the full
  sequence.
- **Truncation test:** construct a `ByteReader` over a buffer 1 byte shorter than
  a full valid message, read through it — every read past the truncation point
  returns `0`, `reader.ok()` becomes `false`, and no read causes a crash, an
  out-of-bounds access (verify under ASan), or a hang.
- **Overflow test:** `ByteWriter` with a small fixed `cap`, write more bytes than
  `cap` allows — `writer.ok()` becomes `false`, and no write goes past `cap`
  (verify under ASan).

---

### 1.4 `shared/map.h/.cpp`

**References:** README §7.1 (map layout), §7.2 (AABB collision)

Checklist:
- [ ] Hardcoded `const char*` tile array, 40×25 tiles, mirrored left-right, two
      tile types (empty, wall), per §7.1.
- [ ] Red base coordinate constant on the left, blue base on the right; spawn
      points as coordinate constants — not tile types (§7.1 explicit note).
- [ ] Collision query: given a 24×24 AABB and a position, report whether it
      overlaps a wall tile.
- [ ] No square-root or floating-point math anywhere in collision queries (§7.2 —
      AABB chosen specifically to avoid this).

Acceptance criteria:
- The tile array is exactly mirrored left-right. Test: for every tile `(x, y)`,
  `tile(x, y) == tile(39 - x, y)`.
- A 24×24 AABB placed fully inside an empty tile reports no collision; the same
  AABB shifted so it overlaps a wall tile by 1px reports a collision. Test both
  directions (X-only overlap, Y-only overlap) since §7.2 requires X-then-Y axis
  resolution to be well-defined.
- `grep -n "sqrt\|sqrtf"` in this file returns nothing.

---

### 1.5 `shared/movement.h/.cpp`

**References:** README §7.2 (movement rules), §5.1 (fixed-point), §6.7 (desync
checklist items 1, 2, 6)

Checklist:
- [ ] `fp_mul`/`fp_div` fixed-point helpers, used consistently for all position
      math (§5.1).
- [ ] `movement_step(const PlayerMotion&, const InputCmd&, const Map&) ->
      PlayerMotion` — exact signature from §7.2, no additional parameters.
- [ ] No acceleration/momentum: input maps directly to velocity, releasing a key
      stops instantly (§7.2).
- [ ] Diagonal movement normalized by `181/256` when both axes are non-zero
      (§7.2 — the 41%-too-fast bug this prevents).
- [ ] Collision resolves X fully, then Y, in that fixed order, against the 24×24
      AABB and the tile grid from `map.h` (§7.2, and §6.7 item 6 — order must be
      identical on both call sites by construction, since there's only one
      function).
- [ ] Function contains no I/O, no logging, no randomness, no static/global
      mutable state.

Acceptance criteria:
- **Determinism test (named in README §10):** generate 10,000 random
  `(PlayerMotion, InputCmd)` pairs, run each through `movement_step` twice,
  assert byte-identical `PlayerMotion` output both times.
- **Diagonal speed test:** feed an input with both movement axes held for one
  tick from a fixed start position with no walls nearby; resulting displacement
  magnitude is within fixed-point rounding tolerance of straight-axis speed
  (i.e. not 41% faster) — compare `move_speed` diagonal displacement vs. two
  single-axis displacements added in quadrature.
- **No-float test:** `grep -n "float\|double"` in `movement.cpp` and
  `movement.h` returns nothing.
- **Corner-sliding test:** position the AABB approaching a corner such that
  X-then-Y resolution and Y-then-X resolution would produce different final
  positions (construct this case deliberately); confirm the function is only
  called with one fixed order everywhere in the codebase (grep all call sites).

---

### 1.6 `shared/protocol.h/.cpp`

**References:** README §5.3 (framing), §5.4 (message set table), §5.5 (snapshot
layout), §5.6 (handshake), §10 ("Serialization round-trip for every message
type", "TCP frame reassembly with artificially split reads")

Checklist:
- [ ] Message type enum covering every row in the §5.4 table (`JOIN_LOBBY`
      through `HEARTBEAT`).
- [ ] `encode`/`decode` pair for every message type, built on `ByteWriter` /
      `ByteReader` — no message serialized via `memcpy`.
- [ ] TCP frame format: `[u16 payload_len][u8 type][payload...]` (§5.3).
- [ ] TCP reassembly loop logic: buffer accumulation, peek length once ≥3 bytes
      buffered, dispatch + `memmove` remainder once `buffered >= len + 3`, cap at
      4 KB with disconnect on overflow (§5.3).
- [ ] UDP header: `u16 magic (0x4346)`, `u8 version`, `u8 type`, `u32 tick` — 8
      bytes total; reject on magic mismatch or version mismatch immediately
      (§5.3).
- [ ] `WORLD_SNAPSHOT` encode/decode matches the exact byte layout in §5.5,
      including the per-recipient `last_input_seq` patch point (4 bytes, patched
      post-serialization, not baked in per-player).
- [ ] `PLAYER_INPUT` encode/decode carries `base_seq` plus 3× `{buttons, aim_angle}`
      per §5.4/§5.6.
- [ ] Angle field is `u16` covering `0..2π` per §5.5 — encode/decode does the
      scaling, not the caller.
- [ ] Every decode path checks `reader.ok()` after reading and drops the packet
      (returns a "invalid" sentinel / `std::optional`) on failure — no decode
      path can crash or hang on malformed input (§5.2, §5.3).

Acceptance criteria:
- **Round-trip test (named in README §10):** for every message type in the
  §5.4 table, construct a representative instance, encode it, decode it, and
  assert every field matches the original.
- **TCP reassembly test (named in README §10):** feed the reassembly loop a
  correctly framed message split across an arbitrary number of `recv()`-sized
  chunks (test at least: 1 byte at a time, and split mid-header) — the dispatched
  message is identical to feeding it in one shot, and no message is dispatched
  twice or dropped.
- **Malformed-packet fuzzing (named in README §10):** feed the UDP decode path a
  1-byte datagram, a datagram with wrong magic, a datagram with a bad version —
  in every case the decode path returns "invalid" (no crash, no hang, no output
  message dispatched). This must be tested directly since "the UDP socket
  accepts datagrams from anyone on the LAN" (§5.2).
- **Snapshot patch test:** serialize a `WORLD_SNAPSHOT` body once, patch the
  `last_input_seq` field for two different recipient values, decode both — only
  the patched field differs between the two decoded results, everything else is
  identical.
- **TCP overflow test:** feed the reassembly loop a stream that never completes a
  valid frame and exceeds 4 KB — the caller-visible result is "disconnect this
  client," not a crash or unbounded buffer growth (verify buffer size is capped).

---

### 1.7 `shared/net_util.h/.cpp`

**References:** README §3.2 (outbound sends — non-blocking sockets, `EAGAIN`
handling)

Checklist:
- [ ] `set_nonblocking(fd)` — sets the fd non-blocking via `fcntl`.
- [ ] `send_all(fd, buf, len)` — loops on partial writes, returns early (without
      looping forever) on `EAGAIN`, signaling the caller how many bytes were
      actually sent so the caller can buffer the remainder.
- [ ] `recv_framed` helper — thin wrapper used by the TCP reassembly loop in
      `protocol.cpp` / `net_server.cpp` to read available bytes into the
      accumulation buffer without blocking.

Acceptance criteria:
- `set_nonblocking` applied to a socket, then a `send`/`recv` call on that socket
  with no data available returns immediately with `EAGAIN`/`EWOULDBLOCK` rather
  than blocking — verify with a timeout wrapper in the test (test fails if the
  call blocks longer than e.g. 100ms).
- `send_all` on a socket whose send buffer is artificially filled (e.g. via a
  small `SO_SNDBUF` and a large payload) returns a partial-bytes-sent count
  rather than blocking or crashing, and calling it again with the remainder
  eventually sends the full payload once the peer drains it.

---

## 2. Bibidh — server (`server/`)

Build in dependency order. Each item assumes everything above it (including all of
§1) is complete.

### 2.1 `server/poller.h` + `server/poller_poll.cpp`

**References:** README §4.1 line 11 (poll first, epoll week 4), §3.2 (network
thread owns all sockets)

Checklist:
- [ ] `IPoller` interface: register fd (with interest: read/write), deregister
      fd, update interest (e.g. toggle `POLLOUT`), `wait()` returning ready fds.
- [ ] `poller_poll.cpp` implements `IPoller` using `poll()`.
- [ ] Interface makes no assumption specific to `poll()` internals — must be
      swappable for the epoll implementation in week 4 without changing any
      caller.

Acceptance criteria:
- A poller instance with one fd registered for read interest and no data
  available returns an empty ready set from `wait()` with a bounded timeout (not
  a busy loop — README §3.2 explicitly calls out "a spin loop pretending to be a
  design" as wrong).
- Writing data to the peer end of a registered fd causes the next `wait()` call
  to report that fd as read-ready.
- Toggling `POLLOUT` interest on, writing data such that the socket becomes
  writable, causes `wait()` to report write-ready; toggling it back off stops
  further write-ready reports for that fd.

---

### 2.2 `server/net_server.h/.cpp`

**References:** README §3.2 (network thread ownership, outbound sends), §5.3
(TCP framing), §5.6 (handshake, `UDP_HELLO`), §7.5 (disconnect handling as two
independent detectors)

Checklist:
- [ ] Owns the listening TCP socket, all connected client TCP sockets, and the
      single UDP socket — nothing outside this file (and `client_registry.cpp`,
      which it drives) touches a raw fd (§3.2 ownership rule).
- [ ] Accept loop: new TCP connections registered with the poller.
- [ ] Per-client TCP accumulation buffer for frame reassembly (uses
      `protocol.cpp`'s reassembly logic), capped at 4 KB (§5.3).
- [ ] UDP recv: validate 8-byte header (magic, version) before touching payload
      (§5.3); route `UDP_HELLO` to register the client's UDP source
      address (§5.6).
- [ ] Resend logic is NOT the server's job for `UDP_HELLO` (client resends per
      §5.6) — server just needs to handle receiving it whenever it arrives.
- [ ] Session token re-validated against source address on every
      `PLAYER_INPUT` packet, not just the first (§5.4 — "since a client's socket
      can rebind mid-match").
- [ ] Outbound TCP: per-client pending output buffer; `POLLOUT` registered only
      while that client's buffer is non-empty, deregistered when drained (§3.2).
- [ ] Pending buffer exceeding 64 KB disconnects that client (§3.2).
- [ ] Two independent disconnect detectors wired in: TCP `recv() == 0` or error
      closes the fd immediately; UDP silence tracked via `last_input_tick` per
      player with a 3-second timeout checked periodically (§7.5).
- [ ] `HEARTBEAT` messages (TCP, C→S, every 1s per §5.4) are received and
      decoded on this path — at minimum consumed as a no-op so they don't get
      treated as an unknown/malformed frame; if used as a liveness signal
      alongside the two detectors above, that's this file's job to wire in
      (§5.4 lists the message but the disconnect logic itself is fully
      specified by the two detectors in §7.5 — heartbeat handling here is about
      correctly parsing and not choking on the message, not inventing a third
      detector).
- [ ] eventfd registered in the poll set; sim thread's 8-byte write wakes this
      thread immediately after publishing a snapshot (§3.2 — "Do not use a short
      poll timeout and busy-check").
- [ ] This file never reads or writes game state directly — it only pushes
      commands onto the inbound queue and reads pre-serialized output from the
      outbound queue (§3.2 — "It never reads a player position").

Acceptance criteria:
- A UDP packet with wrong magic or wrong version is dropped before any
  further processing — verify via a log/counter that increments on rejection,
  and confirm no downstream command is pushed to the inbound queue.
- A `PLAYER_INPUT` packet with a valid magic/version but a session token that
  doesn't match the registered token for that `player_id` is dropped (no
  movement command reaches the sim thread) — test by sending one valid packet
  to establish state, then one with a tampered token.
- Killing a client with `SIGKILL` (simulating TCP `recv() == 0`/error) results
  in that client's fd being closed and removed from the poller within one
  poll cycle; observable via the client no longer appearing in
  `client_registry`.
- A client that stops sending UDP input (but keeps its TCP socket open) is
  detected as timed out within 3–4 seconds (3s threshold + one check interval)
  — observable via the same removal path as the TCP case.
- Filling one client's pending TCP output buffer past 64 KB (e.g. by not
  draining the peer) results in that client being disconnected — verify the fd
  is closed and no unbounded memory growth occurs (check buffer size stays
  capped, not that it grows without limit).
- After the sim thread writes to the eventfd, `wait()` returns within one poll
  cycle even if no client socket has activity — measure wake latency is
  bounded, not dependent on the poll timeout value.

---

### 2.3 `server/client_registry.h/.cpp`

**References:** README §3.2 (roster ownership, no lock needed), §5.6 (fields
tracked)

Checklist:
- [ ] Per-client record: fd, UDP address, session token, name, `player_id`,
      `last_input_tick`.
- [ ] Touched only by the network thread (§3.2 — "only one thread touches it,"
      no mutex here).
- [ ] Add/remove operations correspond to the network thread's connect/disconnect
      handling in `net_server.cpp`.

Acceptance criteria:
- `grep -n "pthread_mutex"` in this file returns nothing (§3.2 lock discipline
  rule 3 — locking only inside the queue implementation).
- Adding a client, then looking it up by `player_id`, returns the exact record
  inserted; removing it makes subsequent lookups fail cleanly (no crash, no
  stale/dangling reference).

---

### 2.4 `server/lobby.h/.cpp`

**References:** README §5.6 (handshake steps 1–2, 4–5), §7.5 (team assignment,
host promotion)

Checklist:
- [ ] `JOIN_LOBBY` → validate player count (2–10 cap from the proposal, §3 of
      proposal doc) → `JOIN_ACCEPT` with `player_id` + session token, or
      `JOIN_REJECT` with reason (full / in-progress / bad version).
- [ ] `LOBBY_STATE` broadcast on every roster change.
- [ ] `START_REQUEST` accepted only from the host.
- [ ] `GAME_START`: balanced random team assignment (shuffle, alternate) per
      §7.5, spawn point assignment, tick 0.
- [ ] Host = first joined; if host leaves during lobby (not mid-match), promote
      lowest remaining `player_id` (§7.5).
- [ ] `MATCH_END` handling: return to lobby with roster intact, ready for another
      `START_REQUEST` (§7.5).

Acceptance criteria:
- Attempting to join with 10 players already connected returns `JOIN_REJECT`
  with reason "full"; the 11th connection is never assigned a `player_id`.
- Team assignment across repeated `GAME_START` calls (same roster, run the
  shuffle N times) does not produce the same team split every time — verify
  statistically (not deterministic team assignment) — and each run's team sizes
  differ by at most 1 (balanced).
- Host leaving during the lobby phase (before `GAME_START`) results in the
  lowest remaining `player_id` becoming the new host, verified via the next
  `LOBBY_STATE` broadcast's host field.
- After `MATCH_END`, the same connected roster can issue a new
  `START_REQUEST` and receive a new `GAME_START` without reconnecting.

---

### 2.5 `server/queues.h/.cpp`

**References:** README §3.2 (two queues, two mutexes, lock discipline three
rules)

Checklist:
- [ ] Inbound queue (network → sim): carries commands (`CMD_PLAYER_JOINED`,
      `CMD_PLAYER_LEFT`, `CMD_PLAYER_INPUT`, etc.).
- [ ] Outbound queue (sim → network): carries serialized snapshot bodies and TCP
      events ready to send.
- [ ] Exactly two `pthread_mutex_t`, one per queue — this is the **only** file in
      the entire codebase where `pthread_mutex_lock`/`unlock` appears (§3.2 rule
      3).
- [ ] RAII lock guard (~8 lines) wrapping `pthread_mutex_t` so an early `return`
      cannot leak a lock (§3.2).
- [ ] No code path holds both queues' mutexes simultaneously (§3.2 rule 2 — "no
      nesting, deadlock is impossible by construction").
- [ ] No code path holds a lock across a `send`/`recv` syscall — queue push/pop
      only copies data in/out of the buffer under the lock, then releases before
      any I/O happens elsewhere (§3.2 rule 1).

Acceptance criteria:
- `grep -rn "pthread_mutex_lock\|pthread_mutex_unlock"` across the entire
  `server/` directory returns hits only in this file.
- Two threads pushing to the same queue concurrently (stress test: N producer
  threads, each pushing 10,000 items) results in all N×10,000 items present on
  the consumer side with no torn/corrupted entries — run under ThreadSanitizer
  with zero data-race reports.
- RAII guard test: a function that locks and then hits an early `return` (inject
  this deliberately in a test) releases the lock — verify by attempting to
  acquire the same lock immediately after from another thread and confirming it
  succeeds without blocking.
- Code review check (not automated): no function in `sim.cpp` or `net_server.cpp`
  holds a queue lock while calling `send`, `recv`, or any blocking syscall.

---

### 2.6 `server/sim.h/.cpp`

**References:** README §3.2 (tick loop, fixed order inside `tick()`), §6.1
(server input queue, ring buffer of 8), §7.2–7.5 (game rules), §5.5 (snapshot
publish)

Checklist:
- [ ] Owns all game state exclusively — positions, health, flags, scores, active
      roster (§3.2 — "It never touches a socket").
- [ ] Tick loop using `clock_nanosleep` with `CLOCK_MONOTONIC` and
      `TIMER_ABSTIME` against an absolute deadline, per the exact code shape in
      §3.2. Resyncs `next` to now if more than 3 ticks behind, rather than
      catching up.
- [ ] Fixed order inside `tick()`, exactly as listed in §3.2: drain inbound →
      apply commands → pop one input per player → movement → combat → flags →
      win check → publish snapshot and events.
- [ ] Per-player ring buffer of 8 input entries (§6.1). Exactly one input popped
      per tick, no more, no less.
- [ ] Empty buffer on a given tick: apply a zero-movement input, leave the ack
      unchanged (§6.1 — not repeat-last, to prevent exploiting packet loss into
      free movement).
- [ ] Full buffer: drop the oldest incoming input (§6.1).
- [ ] Movement: calls `shared/movement.cpp`'s `movement_step` — no local
      reimplementation.
- [ ] Combat: hitscan per §7.3 — DDA-march to nearest wall, then nearest living
      enemy AABB on the segment, damage if player is nearer than wall. No
      friendly fire. `SHOT_FIRED` event generated (cosmetic, UDP).
- [ ] Death/respawn: drop carried flag at death position, mark dead, 90-tick
      respawn timer, no input/collision/render while dead, respawn at own base
      full health (§7.4).
- [ ] Flags: `at_base` / `carried` / `dropped` states; enemy flag touch = pickup;
      own dropped flag touch = instant return; 15s (450 ticks) auto-return;
      capture requires own flag at base (§7.4).
- [ ] Win check: `MATCH_END` at 3 captures or 10-minute time limit (§7.5).
- [ ] Snapshot publish: serializes the `WORLD_SNAPSHOT` body once per tick per
      the §5.5 layout, hands it to the outbound path for per-recipient
      `last_input_seq` patching (patching itself happens in `broadcast.cpp` /
      `net_server.cpp`, not here — sim's job ends at "body + per-player ack
      table").
- [ ] After publish, writes 8 bytes to the eventfd to wake the network thread
      (§3.2).
- [ ] `grep -n "pthread_mutex"` in this file returns nothing (§3.2 rule 3).

Acceptance criteria:
- **Tick rate stability (named in README §4.8/§10):** run the sim loop under
  synthetic load (10 simulated players sending input) for 60 seconds, measure
  actual tick timestamps — mean tick interval within a small tolerance of
  33.3ms (30 Hz), and no drift accumulates over the 60s window (compare first-10s
  average interval to last-10s average interval).
- **Resync test:** artificially stall the loop (e.g. sleep injected mid-tick) for
  longer than 3 ticks' worth of time; verify the loop does not attempt to run 3+
  ticks back-to-back to "catch up" — instead resyncs `next` to current time.
- **Input ring buffer — empty case:** for one player, send zero `PLAYER_INPUT`
  packets for several ticks; verify that player's position doesn't change
  (zero-movement applied) and their `last_input_seq` ack does not advance.
- **Input ring buffer — full case:** send more than 8 inputs for one player
  within a single tick window (simulate faster-than-30Hz sending); verify the
  oldest entries are dropped and the sim still pops exactly one input for that
  tick.
- **Movement determinism cross-check:** confirm `sim.cpp` calls
  `shared::movement_step` directly (not a copy) — code review / grep check, not
  a runtime test, since this is a build-time invariant.
- **Combat — no friendly fire:** position two same-team players such that one's
  hitscan ray would hit the other; verify no damage is applied.
- **Combat — wall occlusion:** position an enemy behind a wall relative to the
  shooter; verify the shot registers as hitting the wall, not the enemy (no
  damage applied).
- **Flag capture gating:** carrier reaches the enemy base while their own flag
  is not at base (already picked up/dropped by the enemy); verify no capture is
  registered and the flag_state doesn't change to a captured/scored state.
- **Flag auto-return:** drop a flag and let 450 ticks (15s at 30Hz) pass with no
  one touching it; verify it returns to `at_base` automatically.
- **Win condition:** simulate one team reaching 3 captures; verify `MATCH_END`
  fires with the correct winning team and final scores, and no further gameplay
  state changes after that point until a new `START_REQUEST`.

---

### 2.7 `server/broadcast.h/.cpp`

**References:** README §3.2 (outbound sends — UDP per-recipient patching, TCP
event fan-out), §5.5 (`last_input_seq` per-recipient)

Checklist:
- [ ] Takes the sim-published snapshot body, patches the 4-byte
      `last_input_seq` field per recipient before each `sendto` (§3.2, §5.5 —
      "Serialize the body once per tick, then patch those 4 bytes per recipient
      before each `sendto`").
- [ ] Has access to per-client UDP addresses (via `client_registry`) — sim does
      not need them (§3.2).
- [ ] TCP events (`FLAG_PICKED_UP`, `PLAYER_KILLED`, etc.) generated by sim are
      fanned out to all connected clients' pending TCP output buffers.
- [ ] Does not itself touch a mutex — reads pre-serialized data handed off via
      the outbound queue (§2.5) and writes into per-client structures owned by
      `net_server.cpp`/`client_registry.cpp`.

Acceptance criteria:
- Given one serialized snapshot body and two recipients with different
  `last_input_seq` values, the two resulting per-recipient buffers are
  byte-identical except for the patched 4-byte field — verify via diff of the
  two byte arrays.
- A `PLAYER_KILLED` event generated on one tick appears in every connected
  client's pending TCP output buffer (not just the victim's or killer's) within
  one broadcast cycle.

---

### 2.8 `server/main.cpp`

**References:** README §4 (directory structure — entry point), §9 (Build and
run — CLI flags and usage), §3.2 (two threads to start)

Checklist:
- [ ] Parses CLI flags per §9's usage example: `--port`, `--tick`, plus the
      runtime flags called out as worth having: `--snapshot-rate` and
      `--poller poll|epoll`.
- [ ] Constructs the shared state: queues (§2.5), client registry (§2.3),
      chosen poller implementation (§2.1/§2.9) selected by the `--poller` flag.
- [ ] Starts exactly two threads: the network thread (running `net_server.cpp`'s
      poll/accept loop) and the simulation thread (running `sim.cpp`'s tick
      loop) — no more, per §3.2's "exactly two" server-threads decision.
- [ ] Wires the eventfd between the two threads (§3.2) at startup so it exists
      before either thread starts running.
- [ ] Clean shutdown path: both threads can be signaled to stop (e.g. on
      `SIGINT`) and joined, rather than the process being killed out from under
      open sockets.

Acceptance criteria:
- Running `./ctf_server --port 7777 --tick 30` (the exact example in §9) starts
  the server and it accepts a connecting client.
- Running with `--poller poll` and, separately, `--poller epoll` both work from
  the same built binary with no other change — required for the §2.9 benchmark
  to be a fair comparison.
- Running with `--snapshot-rate` set to a value below the tick rate (e.g. 10 Hz
  while ticking at 30 Hz per §9's suggested demo use) visibly reduces the
  outbound `WORLD_SNAPSHOT` frequency without changing the simulation tick
  rate itself — observable via a packet counter or Wireshark capture (§3.8).
- Sending `SIGINT` causes both threads to stop and the process to exit cleanly
  (no hang, no crash) — verify via process exit code and absence of a hung
  process in `ps` after signaling.

### 2.9 `server/poller_epoll.cpp` + poll-vs-epoll benchmark (week 4)

**References:** README §4.1 (epoll swap week 4), §4.8/§10 (measured comparison,
not asserted), §11 week 4 plan

Checklist:
- [ ] `poller_epoll.cpp` implements the same `IPoller` interface as
      `poller_poll.cpp`, swappable via `--poller poll|epoll` runtime flag (§9).
- [ ] Benchmark harness: run the server at n=10 clients under both poller
      implementations, measure throughput/latency metric(s) consistent with
      §4.8's evaluation metrics (tick rate stability, RTT for critical TCP
      events).
- [ ] Record actual numbers, not assertions — README explicitly reframes
      "select/poll/epoll" as "poll then epoll, both measured" (§12 deviations
      table).

Acceptance criteria:
- Server built and run identically against both poller backends (same client
  load, same test script) produces two recorded result sets (tick rate
  stability, and one latency metric) — the deliverable is the recorded numbers
  table, not a pass/fail assertion.
- Swapping `--poller poll` to `--poller epoll` requires no code change outside
  the flag itself — verify by running both from the same built binary.

---

## 3. Nayan — client (`client/`, `bot/`, tooling)

Kept flat per the README's directory listing. The `client_core` static-library
restructuring (packaging `net_client`/`prediction`/`interpolation` so `ctf_bot`
stops pulling files out of `client/` via the include-directory hack) is **not**
part of this build order — it's listed as optional cleanup at the end of this
section, to be picked up if time allows after the bot is working.

### 3.1 `client/net_client.h/.cpp`

**References:** README §5.6 (handshake from the client side), §5.5 (snapshot
parsing), §5.4 (`PLAYER_INPUT` sending)

Checklist:
- [ ] Handshake sequence: `JOIN_LOBBY` → wait `JOIN_ACCEPT`/`JOIN_REJECT` →
      send `UDP_HELLO` → wait `LOBBY_STATE` → wait `GAME_START` (§5.6 steps
      1–5).
- [ ] `UDP_HELLO` resent every 200ms until the first snapshot arrives (§5.6 —
      "The hello can be lost").
- [ ] Sends `PLAYER_INPUT` at 30 Hz with `base_seq` + last 3 inputs (§5.4,
      §6.2).
- [ ] Receives and decodes `WORLD_SNAPSHOT`, hands it to `prediction.cpp`
      (reconciliation) and `interpolation.cpp` (remote players).
- [ ] Drops stale snapshots (`snap.tick <= last_applied_tick`) before handing
      off (§6.3) — or confirms this check lives in `prediction.cpp` instead,
      whichever one file owns it (pick one, document which).
- [ ] Receives TCP events (`PLAYER_KILLED`, `FLAG_CAPTURED`, etc.) and exposes
      them for `render.cpp` to display / react to.
- [ ] `HEARTBEAT` sent every 1s over TCP (§5.4).

Acceptance criteria:
- Against a running server, the full handshake sequence completes and the
  client reaches `GAME_START` state — observable via a log line per handshake
  step, or a state-machine assertion at the end.
- If the first `UDP_HELLO` datagram is dropped (test via `netem` loss or a
  deliberate drop in a test harness), the client still reaches the point of
  receiving its first snapshot within a few resend intervals (i.e. within ~1s),
  not hung indefinitely.
- Sending a `PLAYER_INPUT` packet and inspecting its encoded bytes confirms it
  contains `base_seq` and exactly 3 `{buttons, aim_angle}` entries, matching the
  redundancy scheme in §5.4.

---

### 3.2 `client/prediction.h/.cpp`

**References:** README §6.2 (client tick), §6.3 (reconciliation), §6.5 (what is
never predicted)

Checklist:
- [ ] Client tick function matching the exact shape in §6.2: increment seq,
      sample input, push to history, send input packet, apply
      `movement_step` locally.
- [ ] History stores `{seq, input, state_after}` per entry; `state_after` is
      debug-HUD-only, not used for reconciliation logic itself.
- [ ] Reconciliation matching §6.3 exactly: drop stale snapshots
      (`snap.tick <= last_applied_tick`), drop history through the acked seq,
      snap to authoritative state, **always** replay every unacked history
      entry through `movement_step` — no branch on whether a misprediction
      occurred (§6.3 explicit instruction).
- [ ] Misprediction counter: compare `history.state_at(ack)` to the
      authoritative state purely as an instrumentation/HUD metric, never as a
      control-flow branch.
- [ ] History ring bounded (64 entries is "ample" per §6.3) — not unbounded
      growth.
- [ ] Only the local player's own movement is predicted here — no shooting,
      hits, flag pickup, or death prediction (§6.5).
- [ ] Uses `shared/movement.cpp`'s `movement_step` directly — no local
      reimplementation (same invariant as sim.cpp).

Acceptance criteria:
- **Replay-always test:** feed a sequence of snapshots where every one matches
  the client's own prediction exactly (zero mispredictions); verify the
  resulting local state after reconciliation is unchanged from before
  reconciliation (replay is a no-op when prediction was correct) — this test
  exists specifically because §6.3 says replay must run unconditionally, so it
  must be verified it's actually a no-op in the correct-prediction case, not
  skipped by a hidden branch.
- **Misprediction correction test:** feed a snapshot where the authoritative
  position differs from what the client predicted for that tick; verify the
  local state after reconciliation matches "snap to authority, then replay
  remaining unacked inputs" — not the old (wrong) predicted state.
- **Stale snapshot test:** feed two snapshots out of order (`tick=10` then
  `tick=8`); verify the second (stale) one is dropped — `last_applied_tick`
  stays at 10, no state change.
- **History trim test:** after reconciliation acks seq N, verify history no
  longer contains any entry with `seq <= N` (prevents desync checklist item 5 —
  README §6.7 — "Input history not trimmed on ack, so replay re-applies old
  inputs").
- Code review check: no call to `movement_step` (or equivalent) exists in this
  file for anything other than the local player's own movement — confirms §6.5.

---

### 3.3 `client/interpolation.h/.cpp`

**References:** README §6.4 (interpolation), §6.5 (remote players never
predicted)

Checklist:
- [ ] Ring buffer of the last ~30 snapshots.
- [ ] Fractional `render_tick` advancing at 30 ticks/sec of wall time, target
      `newest_received_tick - 2` (§6.4).
- [ ] Per-frame: find bracketing snapshots `a`/`b` around `render_tick`, lerp
      position using `t = (render_tick - a.tick) / (b.tick - a.tick)`.
- [ ] Angle interpolation computes a signed 16-bit delta and adds — shortest arc,
      not naive linear interpolation (§6.4 — the 65000→500 near-full-spin bug).
- [ ] Clock drift correction: nudge `render_tick` advance rate to 1.02× if buffer
      deeper than target, 0.98× if shallower — never a hard jump (§6.4).
- [ ] On stall (no new snapshots), hold last known position — do not
      extrapolate (§6.4).

Acceptance criteria:
- **Angle wraparound test:** interpolate between `aim_angle = 65000` and
  `aim_angle = 500` at `t = 0.5`; verify the result takes the short arc (near
  `0`, wrapping) rather than spinning almost a full circle through the large
  numeric gap. Compute expected value from the signed-delta formula and assert
  match.
- **Stall test:** stop feeding new snapshots for several frames after the last
  one received; verify the rendered position holds exactly at the last known
  interpolated position rather than continuing to move (no extrapolation).
- **Drift correction test:** feed snapshots such that the buffer depth
  consistently exceeds target; verify `render_tick`'s advance rate is nudged
  (1.02×) rather than jumped — check for absence of any single-frame position
  discontinuity larger than one normal interpolation step.
- Code review check: this file is never called for the local player's own
  entity — confirms §6.5 ("Remote players are interpolated, never predicted").

---

### 3.4 `client/render.h/.cpp`

**References:** README module breakdown (proposal §4.2, module 6), §6.6 (debug
toggles + HUD — "build this on day one of week 3")

Checklist:
- [ ] Minimal 2D raylib rendering: rectangles/circles for players, simple icons
      for flags and pickups (proposal §4.2).
- [ ] **F1 — prediction off:** renders local player at raw authoritative
      position instead of predicted (§6.6).
- [ ] **F2 — interpolation off:** remote players snap between raw snapshots
      instead of interpolating (§6.6).
- [ ] **F3 — server ghost:** translucent outline at the authoritative position of
      the local player, drawn beside the predicted position (§6.6 — "the single
      most useful thing you will build").
- [ ] HUD: RTT, unacked input count, misprediction rate, snapshot buffer depth,
      actual tick rate (§6.6).
- [ ] Not part of `client_core` in any restructuring — render is client-only,
      never linked into the headless bot.

Acceptance criteria:
- Toggling F1 during a live session with induced misprediction (e.g. via
  `netem` delay) visibly changes local player rendering from smooth-predicted to
  raw/laggy — verified visually per the README's own framing ("Shows what the
  network actually feels like"), not automatable, but the toggle's functional
  effect (which state feeds the renderer) is code-reviewable: confirm F1 off
  routes to `prediction`'s local state, F1 on routes to the raw authoritative
  snapshot value for the local player only.
- F3 ghost overlaps the predicted position exactly when there have been zero
  recent mispredictions, and visibly separates when a misprediction is induced —
  same visual/code-review split as above.
- HUD numeric fields are wired to real counters, not placeholders — grep/code
  review confirms RTT, unacked count, misprediction rate, buffer depth, and tick
  rate each read from an actual instrumented value (from `prediction.cpp`,
  `interpolation.cpp`, `net_client.cpp`) rather than a hardcoded or stubbed
  number.

---

### 3.5 `client/main.cpp`

**References:** README §4 (directory structure — entry point), §9 (usage
example)

Checklist:
- [ ] Parses CLI flags per §9's usage example: `--host`, `--port`, `--name`.
- [ ] Wires together `net_client.cpp` (§3.1), `prediction.cpp` (§3.2),
      `interpolation.cpp` (§3.3), and `render.cpp` (§3.4) into the client's main
      loop — connect, run handshake, then loop: sample input → predict → send →
      receive snapshot → reconcile/interpolate → render.
- [ ] Client tick runs at the same fixed 30 Hz as the server (§6.2 — "same rate
      as server"), independent of raylib's render/frame rate if those differ.
- [ ] Clean shutdown on window close / `Ctrl-C` — no dangling socket or thread.

Acceptance criteria:
- Running `./ctf_client --host 192.168.1.10 --port 7777 --name bibidh` (the
  exact example in §9) connects to a running server and reaches the point of
  rendering the local player after `GAME_START`.
- The client's input-sampling/prediction tick runs at a measured ~30 Hz
  regardless of the raylib window's actual FPS (test by capping/uncapping
  render FPS and confirming the input-send rate stays stable).
- Closing the client window or sending `Ctrl-C` results in the server's
  disconnect detection (§2.2) firing within its normal detection window (TCP
  `recv()==0` path, not the 3s UDP-silence path, since a clean TCP close should
  be immediate).

### 3.6 `bot/main.cpp`

**References:** README §4 directory structure (`bot/main.cpp` reuses
`net_client` + `prediction`), §10 ("Ten bots: concurrency model under full load,
tick rate stability")

Checklist:
- [ ] Headless: links `net_client.cpp` and `prediction.cpp` directly (flat, per
      the confirmed decision — no `client_core` lib required for this to work),
      does not link `render.cpp` or raylib.
- [ ] Runs the same handshake, input-sending, and reconciliation logic as the
      real client, minus rendering and minus interpolation of remote players
      (bots don't need to see anyone).
- [ ] Configurable count via `--count` flag per §9 (`./ctf_bot --host ... --count
      9`), and `--host`/`--port` per the same usage example.
- [ ] Bot input generation is simple/synthetic (e.g. scripted or random
      movement) — sufficient to generate realistic input load, not
      sophisticated AI (out of scope per the proposal's emphasis on networking
      over gameplay).

Acceptance criteria:
- **Ten bots load test (named in README §10 as an integration test):** run 9–10
  bots plus the harness against the server for 60 seconds; server tick rate
  stays at the 30 Hz target throughout (cross-reference sim.cpp's own tick-rate
  test, §2.6) and no server crash or client disconnect occurs that isn't
  intentional.
- Bot builds and links without pulling in raylib — verify via build system
  output (no raylib symbols/objects in the `ctf_bot` binary) or a build-time
  check that the bot target doesn't depend on the render target.
- Killing a subset of bots with `SIGKILL` mid-run is correctly detected by the
  server's disconnect handling (cross-reference §2.2's disconnect test) without
  affecting the remaining bots' connections.

---

### 3.7 `tools/netem.sh`

**References:** README §10 (adverse network conditions), §12 (deviation table —
netem used for testing)

Checklist:
- [ ] Script wraps the `tc qdisc add dev lo root netem delay 80ms 20ms loss 5%`
      / `tc qdisc del dev lo root` commands from §10, parameterized (delay,
      jitter, loss as arguments or presets) rather than hardcoded only for the
      one example values.
- [ ] Safe teardown: script (or a documented companion command) reliably removes
      the qdisc afterward so it doesn't leak into unrelated later testing.

Acceptance criteria:
- Running the script with default parameters, then checking `tc qdisc show dev
  lo`, confirms the netem qdisc is active with the specified delay/loss values.
- Running the teardown removes it — confirmed by `tc qdisc show dev lo`
  reverting to the default (no netem entry).
- Under active netem conditions (delay + loss per the §10 example), a live
  client/server session shows: UDP snapshots continuing to arrive (degraded,
  not stopped), TCP events still arriving reliably, and — the specific claim to
  verify — that induced single-packet loss does not visibly disrupt movement
  (input redundancy masking it, per §5.4) versus a version of the test with
  redundancy artificially disabled, if that's feasible to toggle for
  comparison.

---

### 3.8 Wireshark capture plan

**References:** README §10 (inspection tools — "Capture a session and include
annotated screenshots showing the TCP handshake, the UDP snapshot stream, and
packet sizes")

Checklist:
- [ ] Capture filter/session plan covering: the full TCP handshake sequence
      (`JOIN_LOBBY` → `GAME_START`, §5.6), a steady-state UDP snapshot stream
      segment, and at least one `PLAYER_INPUT` exchange.
- [ ] Annotate: message type per packet (cross-reference the §5.4 table),
      packet sizes (cross-reference §5.5's ~128-byte snapshot estimate for 10
      players — confirm actual capture matches or explain the delta).
- [ ] Capture one segment under netem conditions (§3.7) showing degrated-but-
      functioning UDP alongside reliable TCP.

Acceptance criteria:
- Three annotated screenshots/exports exist covering: (1) the TCP handshake
  sequence with each message type labeled, (2) a UDP snapshot stream segment
  with packet size visible and compared against the ~128-byte estimate from
  §5.5, (3) a capture taken during netem-induced loss/delay showing TCP events
  still arriving reliably.
- This is inherently non-unit-testable (README's own carve-out for "observable
  behavior" criteria) — the deliverable is the annotated capture output itself,
  checked for presence and correct labeling rather than a pass/fail assertion.

---

### 3.9 Report draft outline

**References:** proposal document (Abstract, §1–§8), README §10 (metrics to
collect), §12 (deviations table)

Checklist (skeleton only, not full prose):
- [ ] Abstract — adapt from proposal, updated for actual deviations (§12).
- [ ] Introduction / Problem Statement — reuse proposal §1–2 near-verbatim,
      since the problem framing didn't change.
- [ ] Objectives — reuse proposal §3.
- [ ] Design and Architecture — summarize README §3 (threading model), §5
      (protocol), §6 (prediction/reconciliation/interpolation); this section
      references the README rather than re-deriving it.
- [ ] Deviations from Proposal — pull directly from README §12 table, with
      justification already written there.
- [ ] Implementation — brief summary per owner's completed section (this
      document's §2/§3 serve as the source list).
- [ ] Testing and Evaluation — populate with actual results against README
      §10's named tests and §4.8's evaluation metrics (tick rate stability, RTT,
      misprediction rate vs. loss, poll-vs-epoll numbers).
- [ ] Wireshark captures section — insert the annotated captures from §3.8.
- [ ] Conclusion — expected outcomes (proposal §5) vs. actual outcomes.
- [ ] References — proposal's reference list (§13 of README already carries
      the updated version with the added Gaffer On Games citation).

Acceptance criteria:
- Outline exists as section headers with a one-line note on source material for
  each — this task's completion criterion is structural (all sections present,
  each pointing at its source), not content-complete, since actual metrics
  don't exist until week 4 testing is done.

---

### 3.10 Optional/later cleanup: `client_core` restructuring

**Not part of the build order above.** Deferred, pick up only if time remains
after the bot and client are both working end-to-end.

Checklist (for later):
- [ ] Package `net_client.cpp`, `prediction.cpp`, `interpolation.cpp` (and their
      headers) as a `client_core` static library CMake target.
- [ ] Link both `ctf_client` and `ctf_bot` against `client_core` instead of
      `ctf_bot` reaching into `client/` via an include-directory hack.
- [ ] Remove the `file(GLOB ...)` usage and the cross-directory include hack
      identified earlier as the actual structural problem (not directory
      depth).

Acceptance criteria (for later, when picked up):
- `ctf_bot`'s CMake target has no include path pointing into `client/` — it only
  links `client_core` and its own `bot/` sources.
- Removing or renaming a file in `client/` that isn't part of `client_core`
  (e.g. `render.cpp`) does not affect the bot's build.

---

## 4. Integration checkpoints

Reference list only — tied to README §11's four-week plan. Ownership (sections 1–3
above) remains the primary organization of this document; this section exists so
both of you know when you need to be in the same room/call.

- **End of Week 1** (README §11, Week 1) — Server echoes raw position back to a
  connected client; client renders it via raylib. No threads, no prediction, no
  interpolation yet. `shared/` (§1), the minimal single-threaded pieces of
  `net_server.cpp` (§2.2, pre-threading) and `net_client.cpp` (§3.1), plus a
  working `CMakeLists.txt` (§1.0) and both entry points (`server/main.cpp` §2.8,
  `client/main.cpp` §3.5) must all be in place and building together. Expected
  to feel laggy — that's the intended before-picture.

- **Two-client integration, before scaling up** (README §10 — "Two-client
  integration testing to validate the core game loop, prediction, and
  reconciliation logic before scaling up," and §11 Week 2 start) — with two
  real clients connected simultaneously, verify each sees the other's position
  update, and that the core game loop (§2.6), prediction (§3.2), and
  reconciliation (§3.2) behave correctly with more than one player in the
  world before moving on to bot load testing. This is a named test in §10
  distinct from both the single-client Week 1 milestone and the ten-bot Week 2
  milestone below — don't skip straight from one to the other.

- **End of Week 2** (README §11, Week 2) — Sim runs on its own pthread with the
  two-queue split (§2.5, §2.6) in place; full game rules (flags, combat, respawn,
  scoring, win condition) implemented; bot (§3.6) exists and 10 bots hold a
  stable 30 Hz tick rate under load (§2.6's tick-rate acceptance criterion, §3.6's
  ten-bots acceptance criterion — same test, checked from both sides).

- **End of Week 3** (README §11, Week 3) — Prediction, reconciliation, and
  interpolation (§3.2, §3.3) working end-to-end against the real server; debug
  toggles and HUD (§3.4) built and functional, since README explicitly calls
  these "the evaluation methodology" for later, not just a debugging aid.

- **End of Week 4** (README §11, Week 4) — netem testing (§3.7) run against the
  full system, epoll swap and poll-vs-epoll benchmark (§2.9) numbers recorded,
  Wireshark captures (§3.8) taken and annotated, report draft (§3.9) populated
  with real metrics.

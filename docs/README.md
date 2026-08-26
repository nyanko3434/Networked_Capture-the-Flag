# Networked Capture-the-Flag — Design and Implementation Reference

**Course:** Network and Systems Programming (ENCT 386) — Mini Project
**Team:** Bibidh Subedi (PUL080BCT021), Nayan Khusu (PUL080BCT048)
**Timeline:** 4 weeks
**Platform:** Linux, C++17, POSIX sockets, raw pthreads, raylib

This document is the single reference for implementation. Every design decision
made during planning is recorded here along with the reasoning, so that later work
sessions do not re-litigate settled questions.

---

## 1. Overview

A real-time, LAN-based, two-team Capture-the-Flag game for 2–10 players. One
authoritative server runs the entire simulation; clients capture input and render.

The game is the vehicle; the engineering focus is socket programming, concurrent
server design, custom protocol design, and latency compensation. Graphics are
deliberately minimal.

**Dual-transport model:**

- **TCP** — reliable, ordered delivery of one-time critical events (lobby, team
  assignment, kills, flag events, match end).
- **UDP** — high-frequency, loss-tolerant delivery of player input and world
  snapshots.

---

## 2. Core design decisions

Each of these was chosen deliberately. The rationale matters as much as the choice
when defending the project.

| Decision | Choice | Reason |
|---|---|---|
| State authority | UDP snapshot is the *only* source of truth | TCP and UDP are independent streams with no ordering guarantee between them |
| TCP events | Notifications only, never state changes | A late TCP event must not contradict the snapshot |
| Server threads | Exactly two: network + simulation | Thread-per-client multiplies contention for no benefit at n=10 |
| Thread communication | Two queues, no shared game state | Correctness argument becomes one sentence |
| Position representation | Fixed-point integers | Float divergence between client and server breaks reconciliation |
| Movement model | Instant velocity, no acceleration | Halves the state that must be predicted, reconciled, and replayed |
| Server input handling | Exactly one input applied per tick | Client and server must apply identical input sequences |
| Input transmission | Last 3 inputs in every packet | Single-packet loss becomes invisible for ~6 extra bytes |
| Combat | Hitscan, server-side | Projectiles would add entities, lifecycle events, and interpolation |
| Prediction scope | Own movement only | Predicting pickups or kills produces visible rollback artifacts |
| I/O multiplexing | `poll()` first, `epoll` in week 4 | 11 fds do not need epoll; swapping late gives a measurable comparison |

### Explicitly out of scope (documented as future work)

- Lag compensation (server rewinding to validate shots against past positions)
- Projectile weapons, item pickups, powerups
- Mid-match joining
- Delta-compressed snapshots
- Persistence, cloud hosting, internet play, NAT traversal

---

## 3. Architecture

### 3.1 Overall

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

Clients send input and render. They compute nothing that affects the outcome.

### 3.2 Server threading model

```
  ┌────────────────────┐   inbound queue    ┌────────────────────┐
  │  Network thread    │ ─────────────────▶ │  Simulation thread │
  │  Owns all sockets  │                    │  Owns all state    │
  │  poll/epoll loop   │ ◀───────────────── │  30 Hz tick loop   │
  └────────────────────┘   outbound queue   └────────────────────┘
```

**Ownership is exclusive.** The network thread owns every file descriptor, the
client registry (fd, UDP address, session token, name), and all socket buffers. It
never reads a player position. The simulation thread owns all game state —
positions, health, flags, scores, active roster. It never touches a socket.

**Two mutexes total**, one per queue. That is the entire synchronization surface.

**Player join and leave are commands**, not direct mutations. The network thread
completes the handshake, then pushes `CMD_PLAYER_JOINED`. The sim thread applies it
at a tick boundary. The roster therefore needs no lock — only one thread touches it.

#### Lock discipline (three rules, no exceptions)

1. **Never hold a lock across a syscall.** Copy into a local buffer, unlock, then
   `send`/`recv`.
2. **Never hold both locks at once.** With no nesting, deadlock is impossible by
   construction rather than by convention.
3. **Lock only inside the queue implementation.** `pthread_mutex_lock` must not
   appear in `sim.cpp`, `net_server.cpp`, or anywhere else.

Wrap `pthread_mutex_t` in a small RAII guard (~8 lines) so an early `return` cannot
leak a lock. This still satisfies the raw-pthreads requirement.

#### Tick loop

```cpp
struct timespec next;
clock_gettime(CLOCK_MONOTONIC, &next);
while (running) {
    next.tv_nsec += TICK_NS;              // normalize past 1e9
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    tick();
}
```

`TIMER_ABSTIME` against an absolute deadline is drift-free. Sleeping a fixed 33 ms
accumulates the tick's own duration as error. If the loop wakes more than 3 ticks
behind, resync `next` to now rather than trying to catch up.

Fixed order inside `tick()`:

```
drain inbound → apply commands → pop one input per player → movement
             → combat → flags → win check → publish snapshot and events
```

#### Waking the network thread

The network thread blocks in `poll()`. After publishing a snapshot, the sim thread
writes 8 bytes to an **eventfd** registered in the poll set. The network thread
wakes immediately. Do not use a short poll timeout and busy-check — that is a spin
loop pretending to be a design.

#### Outbound sends

- **UDP snapshots** — sim publishes the body plus a per-player `last_input_seq`
  table. The network thread patches 4 bytes per recipient and issues one `sendto`
  each. It has the addresses; the sim does not need them.
- **TCP events** — all sockets non-blocking, so `send` may return `EAGAIN`
  mid-message. Each client keeps a pending output buffer. Register `POLLOUT` for
  that client only while its buffer is non-empty; deregister when it drains.
  Skipping this works with two clients and corrupts messages with ten.
- If a client's pending buffer exceeds 64 KB, disconnect it. Unbounded buffering
  turns one slow client into a server-wide memory problem.

---

## 4. Directory structure

```
ctf/
├── CMakeLists.txt
├── shared/                  # compiled into server, client, AND bot
│   ├── game_config.h        # every tuning constant — no duplicates anywhere
│   ├── game_types.h         # PlayerState, FlagState, WorldSnapshot
│   ├── bytebuffer.h/.cpp    # cursor-based read/write, endian-safe
│   ├── protocol.h/.cpp      # message enums + encode()/decode() per message
│   ├── movement.h/.cpp      # THE physics step — must never fork
│   ├── map.h/.cpp           # tile grid + collision queries
│   └── net_util.h/.cpp      # set_nonblocking, send_all, recv_framed
├── server/
│   ├── main.cpp
│   ├── poller.h             # IPoller interface
│   ├── poller_poll.cpp
│   ├── poller_epoll.cpp     # week 4
│   ├── net_server.h/.cpp    # accept, TCP framing, UDP recv/send
│   ├── client_registry.h/.cpp
│   ├── lobby.h/.cpp
│   ├── queues.h/.cpp        # the ONLY place mutexes appear
│   ├── sim.h/.cpp           # tick thread
│   └── broadcast.h/.cpp
├── client/
│   ├── main.cpp
│   ├── net_client.h/.cpp
│   ├── prediction.h/.cpp
│   ├── interpolation.h/.cpp
│   └── render.h/.cpp
├── bot/main.cpp             # headless client, reuses net_client + prediction
└── tools/netem.sh
```

**Two structural rules:**

1. Nothing outside `queues.cpp` touches a mutex.
2. Every constant lives in `game_config.h`. A movement constant duplicated in a
   client file causes silent prediction desync that takes days to find.

---

## 5. Protocol

### 5.1 Fixed-point positions

Positions are integers, never floats.

- **Internal:** `int32`, 1/256 px units
- **On the wire:** `int16`, 1/16 px units (range ±2048 px; map is 1280×800)

Client and server run the same movement code and must produce *identical* results.
Any divergence — including from different compiler optimization flags — makes
reconciliation fight itself and produces permanent micro-jitter. Integer math
removes the entire failure class, halves position bytes, and sidesteps float
endianness. It also makes misprediction detection exact integer equality with no
epsilon to tune.

Write `fp_mul`/`fp_div` helpers in `shared/movement.cpp`.

### 5.2 ByteBuffer

```cpp
class ByteWriter {
  uint8_t* buf; size_t cap, pos; bool overflow;
public:
  void u8(uint8_t v); void u16(uint16_t v);   // htons
  void u32(uint32_t v); void i16(int16_t v);
  bool ok() const { return !overflow; }
  size_t size() const { return pos; }
};

class ByteReader {
  const uint8_t* buf; size_t len, pos; bool underflow;
public:
  uint8_t u8(); uint16_t u16(); uint32_t u32(); int16_t i16();
  bool ok() const { return !underflow; }
};
```

**Never `memcpy` a struct onto the wire.** Padding and endianness differ between
machines; it silently works on one and breaks on another. Every message gets an
explicit `encode`/`decode` pair.

**A malformed packet must never crash or hang the server.** The reader returns zeros
past the end and sets `underflow`; every decode path checks `ok()` and drops the
packet. A one-byte UDP datagram from `nc` must be a no-op. Test this explicitly —
the UDP socket accepts datagrams from anyone on the LAN.

### 5.3 Framing

**TCP is a byte stream, not a message stream.** `recv()` returns partial and merged
messages. Every client needs a persistent accumulation buffer server-side.

```
TCP frame:  [u16 payload_len][u8 type][payload...]
```

Loop: while buffered ≥ 3, peek `len`; if buffered ≥ `len + 3`, dispatch one message
and `memmove` the remainder. Cap at 4 KB; disconnect anyone exceeding it.

**UDP is datagram-bounded** — no length prefix, but a header is required because
packets arrive out of order, duplicated, and from unknown senders.

```
UDP header (8 bytes):
  u16 magic    0x4346    reject anything else immediately
  u8  version            reject mismatches (catches stale binaries)
  u8  type
  u32 tick               server tick this packet describes
```

### 5.4 Message set

| Message | Transport | Direction | Payload |
|---|---|---|---|
| `JOIN_LOBBY` | TCP | C→S | name (16 bytes, fixed) |
| `JOIN_ACCEPT` | TCP | S→C | player_id, session_token, udp_port |
| `JOIN_REJECT` | TCP | S→C | reason (full / in-progress / bad version) |
| `LOBBY_STATE` | TCP | S→all | player count, ids, names, host id |
| `START_REQUEST` | TCP | C→S | host only |
| `GAME_START` | TCP | S→all | team per player, spawn points, tick 0 |
| `UDP_HELLO` | UDP | C→S | player_id, session_token |
| `PLAYER_INPUT` | UDP | C→S | player_id, token, base_seq, count, 3× input |
| `WORLD_SNAPSHOT` | UDP | S→all | see 5.5 |
| `SHOT_FIRED` | UDP | S→all | shooter, origin, angle, hit point (cosmetic) |
| `PLAYER_KILLED` | TCP | S→all | victim, killer, tick |
| `PLAYER_RESPAWNED` | TCP | S→all | player, position, tick |
| `FLAG_PICKED_UP` | TCP | S→all | flag team, player, tick |
| `FLAG_DROPPED` | TCP | S→all | flag team, position, tick |
| `FLAG_RETURNED` | TCP | S→all | flag team, player, tick |
| `FLAG_CAPTURED` | TCP | S→all | flag team, player, tick |
| `MATCH_END` | TCP | S→all | winning team, final scores |
| `HEARTBEAT` | TCP | C→S | every 1 s |

**The session token is not optional.** UDP has no connection state — without it,
anyone on the LAN can spoof a `player_id` and puppet another player. Re-validate the
source address against the token on *every* input packet, not just once, since a
client's socket can rebind mid-match.

**Input redundancy:** every `PLAYER_INPUT` carries the last three inputs (base_seq
and 3× `{buttons u8, aim_angle u16}`), about 27 bytes total. A lost datagram then
costs nothing — the next packet already contains it. The server ignores any seq it
has already queued. This is the highest value-per-line change in the project; most
apparent "network jitter" in a naive implementation is single-packet input loss.

### 5.5 Snapshot layout

```
header (8)  +  u32 last_input_seq            ← per-recipient
               u8  player_count
               u8  flag_carrier_red, flag_carrier_blue   (0xFF = none)
               u8  flag_state_red, flag_state_blue       (at_base/carried/dropped)
               u8  score_red, score_blue
               u16 seconds_remaining
per player (9 bytes):
               u8  id
               i16 x, i16 y
               u16 aim_angle                 (0..65535 → 0..2π)
               u8  health
               u8  flags                     (alive | carrying | team | firing)
```

Ten players ≈ 128 bytes — one datagram, no fragmentation, no delta compression
needed at this scale.

**`last_input_seq` is per-recipient.** Reconciliation requires each client to be
told which of *its own* inputs was last applied, so the snapshot is not one
identical buffer. Serialize the body once per tick, then patch those 4 bytes per
recipient before each `sendto`.

#### Delta snapshots (post-course extension)

Bandwidth optimization: most ticks change only a few players. With
`--snapshots delta` (the default), the server publishes a full
`WORLD_SNAPSHOT` **keyframe** every 10th snapshot and a `DELTA_SNAPSHOT`
(type 19) in between. The delta carries field-level diffs against the
*previous publish*:

```
u32 last_input_seq      <- offset 0, patched per recipient (same as full)
u8  baseline_ticks_ago  <- baseline = header tick - this (usually 1)
u8  header_mask         <- scores / seconds / flag fields present below
[changed header fields]
u16 player_change_mask  <- bit i = player slot i changed
per changed player: u8 id, u8 field_mask (pos/aim/health/flags), [set fields]
```

- The client caches its last applied snapshot; a delta is decoded onto that
  cache. Positions compare at wire granularity (i16, 1/16 px), so an encoder
  never emits a "change" the decoder cannot represent.
- **Loss handling without ACKs:** if a datagram is lost, later deltas
  reference a baseline the client does not hold. The decoder detects the tick
  mismatch, rejects them, and the stream stalls until the next keyframe — at
  most `kSnapshotKeyframeInterval` snapshot intervals (~333 ms at 30 Hz).
  No NACK channel; UDP stays fire-and-forget.
- The protocol version bumped to 2 so stale v1 binaries get a clean
  `JOIN_REJECT BadVersion` instead of mis-decoding type 19.
- Measured numbers live in `docs/benchmark_snapshots.md`
  (`tools/bench_bandwidth.sh`); the optimization write-up itself is
  `docs/optimization.md`.

### 5.6 Connection handshake

```
1. JOIN_LOBBY    TCP  C→S   client connects and asks
2. JOIN_ACCEPT   TCP  S→C   returns player_id and session token
3. UDP_HELLO     UDP  C→S   registers the client's UDP source port
4. LOBBY_STATE   TCP  S→all broadcast on every change
5. GAME_START    TCP  S→all teams and spawn points
```

**Step 3 is the one people forget.** The client's UDP source port is assigned by the
kernel and is unrelated to its TCP port — the server cannot know where to send
snapshots until the client speaks over UDP. Until `UDP_HELLO` lands, the player
exists in the lobby but receives nothing.

The hello can be lost. Resend it every 200 ms until the first snapshot arrives.

### 5.7 Disconnect handling

Two independent detectors, because the transports fail differently:

- **TCP:** `recv` returning 0 or an error is definitive. Close the fd, drop from the
  poller, free the slot.
- **UDP silence:** a client can go quiet on UDP while TCP stays technically open
  (suspended process, stalled wifi). Track `last_input_tick` per player; 3 seconds
  of silence is a timeout.

On either path: remove from the sim, drop their carried flag where they stood,
broadcast the departure over TCP. If the host leaves during the lobby, promote the
lowest remaining player id.

Get this right early — you will be `Ctrl-C`-ing clients constantly, and a server
that leaks slots or wedges on a half-closed socket wastes hours.

---

## 6. Prediction, reconciliation, interpolation

Three distinct mechanisms. Keep them in separate files.

| | Applies to | Purpose |
|---|---|---|
| Prediction | your own player | act on input now, don't wait for RTT |
| Reconciliation | your own player | fix prediction when the server disagrees |
| Interpolation | every other player | smooth rendering between 30 Hz snapshots |

Your own player is predicted, never interpolated. Remote players are interpolated,
never predicted. No overlap.

### 6.1 Server input queue

**A per-player ring buffer of 8 entries. The simulation pops exactly one input per
tick — no more, no less.** This is the invariant everything else depends on.

Reconciliation only works if the server applies exactly the same inputs, in the same
order, as the client predicted with. A single-slot "latest wins" design discards
inputs the client already predicted with, and the two diverge every time two packets
land in one tick.

- **Buffer empty** (packet lost or late): apply a zero-movement input, leave the ack
  unchanged. Player stalls one tick; reconciliation corrects it next snapshot.
  Zero rather than repeat-last means packet loss cannot be exploited into free
  movement.
- **Buffer full:** drop the oldest. A client sending faster than 30 Hz gains nothing.

### 6.2 Client tick

```
on_client_tick():                       // fixed 30 Hz, same rate as server
    seq += 1
    input = sample_input()
    history.push({seq, input, state_after: null})

    send_input_packet(seq, last_three_from(history))

    local_state = movement_step(local_state, input)   // shared/movement.cpp
    history.back().state_after = local_state          // debug HUD only
```

### 6.3 Reconciliation

```
on_snapshot(snap):
    if snap.tick <= last_applied_tick: return       // UDP reorders — drop stale
    last_applied_tick = snap.tick

    auth = snap.players[my_id]
    ack  = snap.last_input_seq

    history.drop_through(ack)

    if history.state_at(ack) != auth: mispredictions += 1   // instrumentation

    local_state = auth                               // snap to authority
    for entry in history (ascending seq):            // replay everything unacked
        local_state = movement_step(local_state, entry.input)
```

**Always replay — do not branch on whether a misprediction occurred.** If prediction
was correct, replay is deterministic and reproduces the same state, so nothing moves
on screen. Skipping it saves ~5 movement steps per snapshot (nothing) and adds a
branch for bugs to hide in. Keep the comparison purely as a HUD counter and report
metric.

`history` never exceeds `RTT × 30` entries — 2 to 4 on a LAN. A 64-entry ring is
ample. The unacked count *is* the RTT made visible; put it on the HUD.

### 6.4 Interpolation

Keep a ring of the last ~30 snapshots. Maintain a fractional `render_tick` advancing
at 30 ticks/sec of wall time, targeting `newest_received_tick - 2`. Each frame, find
the two snapshots bracketing `render_tick`:

```
t   = (render_tick - snap_a.tick) / (snap_b.tick - snap_a.tick)
pos = lerp(a.pos, b.pos, t)
```

**Angle wraparound:** `aim_angle` is a `u16` covering 0..2π. Interpolating 65000 →
500 naively spins the player almost all the way around. Compute the difference as a
signed 16-bit delta and add — shortest arc.

**Clock drift:** local and server clocks will not match. Nudge `render_tick` — if the
buffer is deeper than target advance at 1.02×, if shallower 0.98×. Never jump it; a
hard correction is a visible pop.

**On stall, hold the last known position; do not extrapolate.** Extrapolation
overshoots and snaps back, which looks worse than a brief freeze.

The 2-tick delay costs ~66 ms of "seeing others in the past." That trade —
smoothness for latency — is the thing to be able to defend.

### 6.5 What is never predicted

Movement only. Not shooting, hits, flag pickup, or death. Predicting a flag pickup
means occasionally showing the grab and yanking it back, which looks far worse than
a 30 ms delay. This is also the answer to "how do you prevent cheating": the client
predicts nothing that affects the outcome.

### 6.6 Debug tooling — build this on day one of week 3

- **F1 — prediction off.** Renders your player at the raw authoritative position.
  Shows what the network actually feels like.
- **F2 — interpolation off.** Remote players snap between raw snapshots.
- **F3 — server ghost.** Draws the authoritative position of your own player as a
  translucent outline beside your predicted position.

**The ghost is the single most useful thing you will build.** When prediction is
correct the outline sits exactly on you and is invisible. When it drifts you *see*
the divergence, its direction, and its trigger. Debugging reconciliation without it
is guesswork.

These toggles double as the evaluation methodology — flip them during the demo,
screenshot them for the report.

**HUD:** RTT, unacked input count, misprediction rate, snapshot buffer depth, actual
tick rate. This turns the evaluation section from assertions into measurements.

### 6.7 Desync checklist (in order)

1. A movement constant duplicated in a client file instead of read from `game_config.h`
2. Floats in the movement path
3. Server applying more or fewer than one input per tick
4. Stale snapshots not dropped (`snap.tick <= last_applied_tick`)
5. Input history not trimmed on ack, so replay re-applies old inputs
6. Collision axis resolution order differing between client and server

Number 6 is the sneaky one — resolving X-then-Y vs Y-then-X gives different results
when sliding along a corner. Shouldn't happen since both call the same function,
unless someone "optimizes" one copy.

---

## 7. Game rules

### 7.1 Map

40 × 25 tiles at 32 px = 1280 × 800. Two tile types: empty and wall. Hardcoded as a
`const char*` array in `shared/map.cpp`, mirrored left-right. Red base left, blue
base right. Bases and spawn points are coordinate constants, not tile types.

### 7.2 Movement

**No acceleration or momentum.** Input maps directly to velocity; releasing a key
stops instantly. Momentum would make velocity part of the predicted/reconciled/
replayed state, roughly doubling week 3's debugging surface. Instant velocity means
a misprediction is a position error only.

Diagonal movement must be normalized or it is 41% faster: multiply by `181/256`
(≈0.707) when both axes are non-zero.

**Collision:** 24×24 AABB against the tile grid. Resolve X fully, then Y. The axis
order is fixed and must be identical on both sides. AABB rather than a circle
collider because it needs no square roots — everything stays integer.

**No carrier slowdown.** It would require `movement_step` to know about flag state,
breaking the pure signature. Defense is handled by the flag-at-home rule instead.

```cpp
// shared/movement.cpp — the one function that must never fork
PlayerMotion movement_step(const PlayerMotion& in, const InputCmd& cmd, const Map& map);
```

Position, velocity, and input in; position and velocity out. No weapon state, no
health, no team, no timers. Unit-testable: run 10,000 random inputs through it twice
and assert byte-identical results.

### 7.3 Combat

Hitscan, server-side, instant. On a tick where the fire bit is set and cooldown is
zero:

1. DDA-march the ray through the tile grid to find the nearest wall hit
2. Test the segment against every living enemy AABB, take the nearest
3. If a player is nearer than the wall, apply damage

**No friendly fire** — removes accidental griefing during the demo and saves a team
check in three places.

`SHOT_FIRED` goes over UDP with origin, angle, and hit point; the client draws a
tracer for ~80 ms. Purely cosmetic, so a lost one is invisible.

### 7.4 Death, respawn, flags

On death: drop the carried flag at the death position, mark dead, start a 90-tick
timer. Dead players receive no input, have no collision, are not rendered. Respawn
at own base with full health.

Flag states: `at_base`, `carried`, `dropped`.

- Touching an **enemy** flag picks it up.
- Touching **your own** dropped flag returns it to base instantly.
- A dropped flag auto-returns after 15 s untouched, preventing a flag being parked
  in a corner forever.

**Capture requires your own flag to be at base.** If both flags are out, the carrier
waits for a teammate to return theirs. This single `if` is what makes defense
matter — without it both teams just race and the game has no tactical shape.

### 7.5 Teams and match flow

Balanced random assignment at `GAME_START` (shuffle, alternate). Host is whoever
joined first; if they leave during the lobby, promote the lowest remaining id.

**No mid-match joining** — it requires seeding a client with full state, which is
out of scope.

`MATCH_END` on 3 captures or 10 minutes, then everyone returns to the lobby with the
roster intact so another match can start immediately. This detail saves real testing
time.

---

## 8. Configuration

| Constant | Value | Lives in |
|---|---|---|
| Tick rate | 30 Hz | shared |
| Tile size | 32 px | shared |
| Player size | 24 × 24 px | shared |
| Move speed | 160 px/s (1365 fp-units/tick) | shared |
| Diagonal factor | 181/256 | shared |
| Max health | 100 | shared |
| Input redundancy | 3 per packet | shared |
| Damage per shot | 34 (3 shots to kill) | server |
| Fire cooldown | 10 ticks | server |
| Respawn delay | 90 ticks (3 s) | server |
| Flag auto-return | 450 ticks (15 s) | server |
| Score to win | 3 captures | server |
| Match time limit | 10 minutes | server |
| Interpolation delay | 2 ticks | client |

**The shared/server split matters.** Only shared constants can cause desync, so
keeping that list to seven entries bounds what must be treated as dangerous. Combat
constants can differ between client and server with no consequence — the client
never computes damage.

---

## 9. Build and run

```bash
mkdir build && cd build
cmake .. && make -j
```

Produces `ctf_server`, `ctf_client`, `ctf_bot`.

```bash
./ctf_server --port 7777 --tick 30
./ctf_client --host 192.168.1.10 --port 7777 --name bibidh
./ctf_bot    --host 192.168.1.10 --port 7777 --count 9
```

Runtime flags worth having: `--snapshot-rate` (drop to 10 Hz during the demo and
interpolation's value becomes visually obvious), `--poller poll|epoll`.

---

## 10. Testing and validation

**Unit tests**

- `movement_step` determinism: 10,000 random inputs run twice, byte-identical
- Serialization round-trip for every message type
- Malformed-packet fuzzing: truncated, oversized, wrong magic, wrong version
- TCP frame reassembly with artificially split reads

**Integration**

- Two clients: core loop, prediction, reconciliation
- Ten bots: concurrency model under full load, tick rate stability
- Kill clients with `SIGKILL` mid-match to verify both disconnect detectors

**Adverse network conditions** (`tools/netem.sh`)

```bash
tc qdisc add dev lo root netem delay 80ms 20ms loss 5%
tc qdisc del dev lo root
```

Verify: UDP state updates degrade gracefully, TCP events still arrive, input
redundancy masks single-packet loss, reconciliation converges.

**Inspection tools:** `netstat`, `ss`, `tcpdump`, Wireshark. Capture a session and
include annotated screenshots showing the TCP handshake, the UDP snapshot stream,
and packet sizes.

**Metrics to collect**

- Max concurrent clients at stable tick rate
- Tick rate stability (target 30 Hz) under full load
- RTT for critical TCP events
- Misprediction rate vs. induced packet loss
- `poll` vs `epoll` throughput at n=10 (measured, not asserted)
- Absence of race conditions and deadlocks under load

---

## 11. Four-week plan

**Week 1 — protocol and "it moves"**
Both members write `shared/` together. Then: TCP connect, `JOIN_LOBBY`, team assign,
`GAME_START`, UDP hello handshake, single-threaded server loop, client sends input,
server echoes positions, raylib draws squares. **No threads, no prediction, no
interpolation.** It will feel laggy — that is deliberate, it is the before-picture
for the demo.

**Week 2 — concurrency and real game**
Split the sim into its own pthread, add the two queues and mutexes, then the actual
rules: flags, hitscan, respawn, scoring, win condition. Write the bot. Run 10 bots
and confirm tick rate holds.

**Week 3 — prediction, reconciliation, interpolation**
The hard week; budget all of it. Build the debug toggles and HUD *first*. This is
where the project earns its marks and where every subtle serialization or
constant-duplication bug surfaces.

**Week 4 — adverse conditions, measurement, report**
netem testing, epoll swap plus benchmark, collect metrics, Wireshark captures,
polish.

**Critical:** something playable must exist by the end of week 1. If week 1 slips,
everything compresses into the week you can least afford to lose.

### Work split

The protocol is the seam. After week 1 it barely changes, so both members can work
in parallel without blocking.

**Bibidh — server:** `poller`, `net_server`, `client_registry`, `lobby`, `queues`,
`sim`, `broadcast`; plus the epoll-vs-poll benchmark.

**Nayan — client:** `net_client`, `prediction`, `interpolation`, `render`, and the
bot (the same networking code minus raylib); plus netem scripts, Wireshark captures,
and the first draft of the report.

The server side is heavier in raw code, which is why tooling and the report draft
sit on the client side.

---

## 12. Deviations from the original proposal

Recorded so the report can explain them rather than quietly dropping them.

| Proposal | Actual | Reason |
|---|---|---|
| Projectile weapons | Hitscan | Projectiles need entity lifecycle, extra snapshot fields, own interpolation |
| `ITEM_SPAWNED` / pickups | Removed | Adds a whole entity lifecycle, contributes nothing to the networking story |
| C++ generally | C++17, C-style POSIX APIs | Course focus is the raw syscall layer |
| "select/poll/epoll" | `poll` then `epoll`, both measured | Turns an assertion into a measured comparison |

Items deferred to future work: lag compensation, mid-match join, projectiles
and pickups, internet play. Delta-compressed snapshots — also on the deferred
list — have since been implemented (see §5.5 "Delta snapshots"), along with a
bandwidth benchmark harness (`tools/bench_bandwidth.sh`).

---

## 13. References

1. W. R. Stevens, B. Fenner, A. M. Rudoff, *UNIX Network Programming, Vol. 1: The
   Sockets Networking API*, 3rd ed., Addison-Wesley, 2003.
2. B. Hall, *Beej's Guide to Network Programming*. https://beej.us/guide/bgnet/
3. Valve Corporation, "Source Multiplayer Networking," Valve Developer Community.
4. G. Fiedler, "Gaffer On Games" — fixed timestep, snapshot interpolation,
   networked physics.
5. raylib. https://www.raylib.com/
<!-- small edit by bibidh -->

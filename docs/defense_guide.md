# defense_guide.md — How the System Works, in Simple Terms

Preparation document for the project defense (ENCT 386 mini project).
Team: Bibidh Subedi (server), Nayan Khusu (client/bot). Main focus: the
**network layer** — transports, protocol, concurrency, and latency
compensation — with everything else explained as needed.

Every claim here points at real code (`file:line`) so you can open it live
during questions. Design decisions trace back to `docs/README.md` sections.

---

## Table of contents

0. [The 60-second version](#0-the-60-second-version)
1. [Big picture](#1-big-picture)
2. [Why two transports (TCP + UDP)](#2-why-two-transports-tcp--udp)
3. [The wire format](#3-the-wire-format)
4. [Security on a hostile LAN: session tokens](#4-security-on-a-hostile-lan-session-tokens)
5. [The connection handshake](#5-the-connection-handshake)
6. [Server internals: two threads, two queues](#6-server-internals-two-threads-two-queues)
7. [The tick loop](#7-the-tick-loop)
8. [I/O multiplexing: poll vs epoll](#8-io-multiplexing-poll-vs-epoll)
9. [The outbound path: snapshots and events](#9-the-outbound-path-snapshots-and-events)
10. [Disconnect handling: two independent detectors](#10-disconnect-handling-two-independent-detectors)
11. [Latency compensation: prediction, reconciliation, interpolation](#11-latency-compensation)
12. [Optimization: delta-compressed snapshots](#12-optimization-delta-compressed-snapshots)
13. [Determinism: integers everywhere](#13-determinism-integers-everywhere)
14. [Input redundancy: surviving packet loss for free](#14-input-redundancy-surviving-packet-loss-for-free)
15. [How we tested all of this](#15-how-we-tested-all-of-this)
16. [Decision table: "why did you choose X?"](#16-decision-table-why-did-you-choose-x)
17. [Likely defense questions with model answers](#17-likely-defense-questions-with-model-answers)
18. [The POSIX socket layer: every network call, in real code](#18-the-posix-socket-layer-every-network-call-in-real-code)

---

## 0. The 60-second version

> We built a 2–10 player LAN Capture-the-Flag game in C++17 on raw POSIX
> sockets. One **authoritative server** runs the whole game simulation at
> **30 ticks per second**; clients only capture keyboard input and draw.
>
> Every client holds **two connections** to the server:
> - a **TCP** connection carrying rare, must-not-be-lost events
>   (joins, kills, flag captures, match end), and
> - a **UDP** connection carrying high-frequency data that tolerates loss
>   (player inputs at 30 Hz, world state snapshots at up to 30 Hz).
>
> The server has exactly **two threads**: one owns every socket and does
> network I/O; the other owns all game state and runs the simulation. They
> talk through **two mutex-guarded queues** — that is the *entire*
> synchronization surface of the server.
>
> To hide network latency the client **predicts** its own movement locally,
> **reconciles** against server snapshots by replaying unacknowledged
> inputs, and **interpolates** other players between snapshots. Bandwidth
> was halved with field-level **delta snapshots** (measured 3.42 → 1.85
> KiB/s per client).

If you can say that smoothly, you've passed the opening question.

---

## 1. Big picture

```
                     LAN
  [client 0] ──TCP──┐             ┌──TCP── [client n]
  [client 0] ──UDP──┤             ├──UDP── [client n]
                    ▼             ▼
              ┌──────────────────────────┐
              │   Authoritative server    │
              │   fixed tick = 30 Hz      │
              │                           │
              │  ┌────────┐  queue  ┌────┐│
              │  │ network│ ═══════▶│sim ││
              │  │ thread │ ◀═══════│    ││
              │  └────────┘  queue  └────┘│
              └──────────────────────────┘
```

Key idea: **the game is the vehicle; the networking is the project.**
Graphics are deliberately squares-on-a-grid so effort went into sockets,
concurrency, a custom binary protocol, and latency compensation.

Clients compute nothing that affects the outcome. They send *intent*
("I hold RIGHT, aim angle X") and receive *truth* ("at tick 4821 you were
here"). This is what makes cheating structurally hard (see §17 Q6).

### The one-sentence architecture

The network thread translates bytes ↔ messages, pushes decoded **commands**
inbound and pulls pre-serialized **events/snapshots** outbound; the sim
thread consumes commands and produces events at each tick boundary. Neither
thread ever touches the other's data directly (`server/queues.h:1-14`).

### Directory map (who talks to the network)

| Layer | Files | Role |
|---|---|---|
| shared | `shared/protocol.{h,cpp}`, `bytebuffer.*`, `net_util.*`, `movement.*`, `game_config.h` | wire codec, framing helpers, THE physics step, all constants |
| server | `server/net_server.cpp`, `poller_*.cpp`, `queues.*`, `broadcast.cpp`, `sim.cpp`, `lobby.cpp`, `client_registry.*` | sockets, event loop, queues, fan-out, game rules |
| client | `client/net_client.cpp`, `prediction.cpp`, `interpolation.cpp`, `render.cpp` | mirror of the protocol + latency compensation |
| bot | `bot/` | headless client reusing the same net code — our load generator |

Two structural rules enforced across the codebase:

1. **Mutexes exist only inside `queues.{h,cpp}`** — grep-enforced
   (`server/queues.h:3`). Nowhere else may lock.
2. **Every tuning constant lives once in `shared/game_config.h`** — a
   duplicated movement constant causes silent client/server divergence,
   which takes days to find (`shared/game_config.h:4-6`).

---

## 2. Why two transports (TCP + UDP)

This is the first question every examiner asks. The answer is about
matching transport properties to message properties.

| Property | TCP | UDP |
|---|---|---|
| Delivery | guaranteed, ordered | best-effort, unordered |
| Latency under loss | bad — a lost segment stalls *everything behind it* (head-of-line blocking) | lost packet is just gone |
| Overhead | ~20+ bytes/packet + ACK machinery | 8-byte header total |
| Message model | byte stream — you must add framing | datagram — natural message boundaries |

A real-time game needs both behaviors simultaneously:

- **TCP for events that must never be lost or arrive out of order:**
  join/lobby/team assignment, kills, flag pickups/captures, match end.
  If FLAG_CAPTURED arrives before the score snapshot explaining it, the
  UI glitches; if MATCH_END is lost, the client hangs forever. There are
  few of these events (~tens per match), so reliability costs nothing.
- **UDP for data that expires in milliseconds:** player input and world
  snapshots. If a snapshot is lost, the next one (33 ms later) supersedes
  it — retransmitting a stale position is actively harmful, because it
  would arrive *after* fresher data and drag players backward.

**The critical design rule that follows** (`docs/README.md` §2): because
TCP and UDP are independent streams with no ordering guarantee *between*
them, the UDP snapshot is declared the **only source of truth**, and TCP
events are notifications only — never state changes. A late TCP event can
never contradict state, because no state lives in TCP.

Evidence in code: the sim emits kills/flag events as `TcpBroadcast`
events (`server/sim.cpp:359-364`), while the world state flows as
`UdpSnapshot`/`UdpDeltaSnapshot` (`server/sim.cpp:561-563`).
`SHOT_FIRED` is deliberately UDP — it's cosmetic, losing one tracer is
invisible (`server/sim.cpp:334-348`).

---

## 3. The wire format

### 3.1 Framing: making TCP behave like a message stream

TCP gives you a featureless byte stream: one `recv()` may return half a
message, three messages, or a message plus a half. So we length-prefix
every message:

```
TCP frame:  [u16 payload_len][u8 type][payload...]     (len excludes header)
```

Both sides run the same reassembly helper, `recv_framed()`
(`shared/net_util.h:31-32`): buffer bytes, peek the u16 length, dispatch
only when a complete frame is buffered, keep the remainder. Frames larger
than 4 KB are a disconnect signal, never an accumulation
(`shared/game_config.h:77`, checked in `server/net_server.cpp:237` and
`256-259`).

UDP needs no length prefix (a datagram *is* a message) but does need an
authenticity/version header, because datagrams arrive out of order,
duplicated, and from anyone on the LAN:

```
UDP header — exactly 8 bytes on EVERY datagram:
  u16 magic 0x4346   ("CF")   reject anything else immediately
  u8  version (=2)            reject stale binaries cleanly
  u8  type                    which message this is
  u32 tick                    which tick this describes
```

Defined at `shared/protocol.h:22-30`; validated before any routing in
`server/net_server.cpp:452-462`. A 1-byte datagram from `nc` is a silent
no-op — malformed packets must never crash or hang the server.

### 3.2 Serialization: never memcpy a struct

Struct padding and endianness differ between machines; dumping a C struct
to a socket silently works on your machine and breaks on another. So every
message gets an explicit field-by-field `encode`/`decode` pair written
through `ByteWriter`/`ByteReader` (`shared/protocol.h:171-241` lists all
19 pairs), all big-endian (network byte order).

`ByteReader` never reads past the end: it returns zeros past the boundary
and sets an `underflow` flag; every decode site checks `ok()` and drops
the packet (`server/net_server.cpp:263-282` pattern). Fuzz tests truncate
every message at every byte offset to prove this.

### 3.3 Positions: fixed-point integers, two scales

Positions are integers, never floats, in two domains:

| Domain | Type | Units |
|---|---|---|
| internal math | int32 | 1/256 px (`kFixedShift=8`, `shared/game_config.h:47-48`) |
| wire | int16 | 1/16 px (`kWireFixedShift=4`, `shared/game_config.h:71-72`) |

Wire conversion is a shift (`wire = internal >> 4`); int16 halves the
bytes and covers ±2048 px — plenty for a 1280×800 map. Why integers?
Client and server run byte-identical movement code, and any float
divergence (different compiler flags among them) makes reconciliation
fight itself forever. Integers remove the entire failure class and make
misprediction detection exact equality with no epsilon
(`docs/README.md` §5.1).

### 3.4 Message set (all 19 types)

| id | Message | Transport | Direction | Purpose |
|----|---------|-----------|-----------|---------|
| 1 | JOIN_LOBBY | TCP | C→S | connect with a name |
| 2 | JOIN_ACCEPT | TCP | S→C | player_id + session_token + server udp_port |
| 3 | JOIN_REJECT | TCP | S→C | Full / InProgress / BadVersion |
| 4 | LOBBY_STATE | TCP | S→all | roster broadcast on every change |
| 5 | START_REQUEST | TCP | C→S | host-only match start |
| 6 | GAME_START | TCP | S→all | teams + spawn points |
| 7 | UDP_HELLO | UDP | C→S | register the client's UDP endpoint |
| 8 | PLAYER_INPUT | UDP | C→S | token + last 3 inputs |
| 9 | WORLD_SNAPSHOT | UDP | S→all | full world state (also the keyframe) |
| 10 | SHOT_FIRED | UDP | S→all | cosmetic tracer |
| 11–17 | PLAYER_KILLED … MATCH_END | TCP | S→all | reliable game events |
| 18 | HEARTBEAT | TCP | C→S | courtesy keepalive (consumed as no-op) |
| 19 | DELTA_SNAPSHOT | UDP | S→all | field-level diff vs previous publish |

Enum: `shared/protocol.h:36-56`.

### 3.5 Snapshot layout (the most important packet)

Serialized body (before the per-recipient patch):

```
u32 last_input_seq        <- PATCHED PER RECIPIENT, payload offset 0
u32 tick                  (repeated from the UDP header)
u8  player_count
u8  flag_carrier_red/blue (0xFF = none)
u8  flag_state_red/blue   (base / carried / dropped)
u8  score_red, score_blue
u16 seconds_remaining
per player (exactly 9 bytes):
  u8 id, i16 x, i16 y, u16 aim_angle, u8 health, u8 flags-bitmask
```

Ten players ≈ 128 bytes — fits in one datagram, no IP fragmentation.
Codec: `shared/protocol.cpp:241` (encode), `:268` (decode).

**Why is `last_input_seq` patched per recipient?** Reconciliation (§11)
needs each client to know which of *its own* inputs the server last
applied. That value differs per client, so the body is serialized **once**
per tick and those 4 bytes are overwritten per recipient just before
`sendto` — see §9.

---

## 4. Security on a hostile LAN: session tokens

UDP has no connection state — any process on the LAN can send a datagram
claiming "I am player 3." Without authentication, anyone could puppet
another player's character.

Defense: during the handshake the server hands each client a random 32-bit
**session token** (`server/net_server.cpp:82-89`). Every `PLAYER_INPUT`
carries it, and the server validates **both** the token *and* the source
IP:port against the registered address on **every** packet, not just once
— a client's socket could rebind mid-match:

```cpp
// server/net_server.cpp:482-495
ClientEntry* NetServer::auth_udp_sender(...) {
    if (entry->session_token != token) return nullptr; // spoofed id
    if (entry->has_udp_addr && (port or addr differs)) return nullptr; // rebound
}
```

Mismatched packets are dropped silently (`server/net_server.cpp:510-511`).
This also means the server can't be tricked by a replayed packet from a
different sender. It's not cryptographic — the threat model is "accidental
or lazy interference on a course LAN," not a determined attacker, and the
comment at `server/net_server.cpp:83-84` says so honestly.

---

## 5. The connection handshake

Five steps, one of which everyone forgets:

```
1. TCP connect                      client → server :7777
2. JOIN_LOBBY   {name}       TCP    C→S
3. JOIN_ACCEPT  {id, token, udp_port} TCP  S→C     ← note: server's UDP port!
4. UDP_HELLO    {id, token}  UDP    C→S  to udp_port
5. LOBBY_STATE broadcasts / GAME_START after host's START_REQUEST
```

Walkthrough in code: `client/net_client.cpp:66-170` (client side),
`server/net_server.cpp:322-354` (JOIN_LOBBY handling), `:391-431`
(GAME_START + team assignment pushed to the sim as commands).

**Step 4 is the one people forget.** The client binds its UDP socket to an
ephemeral kernel-assigned port unrelated to its TCP port
(`client/net_client.cpp:111`). The server literally cannot know where to
send snapshots until the client speaks first over UDP. Until UDP_HELLO
lands, the player exists in the lobby but receives nothing.

And since UDP_HELLO itself can be lost, the client **resends it every
200 ms until the first snapshot arrives** (`client/net_client.cpp:294-302`,
interval at `shared/game_config.h:143`). First received snapshot clears
the resend flag (`client/net_client.cpp:479`).

Also notice: the server's UDP port is **ephemeral** too — it prints
`listening tcp=7777 udp=NNNNN` at startup and delivers its own UDP port
inside JOIN_ACCEPT (`server/net_server.cpp:109-117`, `348-352`). Nothing
is hardcoded; a stale assumption here breaks every deploy.

Why a handshake at all instead of just trusting TCP connect? Because the
two transports are independent: TCP being connected proves nothing about
the client's UDP reachability. Step 4 binds the *pair* together, tied to
the token.

Host semantics: first joiner becomes host (`server/lobby.cpp:27`);
only the host's START_REQUEST is accepted (`server/lobby.cpp:63-65`);
if the host leaves, the lowest remaining id is promoted
(`server/lobby.cpp:37-59`).

---

## 6. Server internals: two threads, two queues

### 6.1 The threading model

Exactly two threads (`server/main.cpp:76`, spawned at `:106-107`):

- **Network thread** — owns every file descriptor, the client registry
  (fd ↔ id ↔ UDP address ↔ token), socket buffers, poller. Never reads a
  position.
- **Simulation thread** — owns positions, health, flags, scores, roster.
  Never touches a socket.

They communicate exclusively via two mutex-protected deques
(`server/queues.h:93-111`):
- `InboundQueue`: commands like `PlayerJoined`, `PlayerLeft`, `PlayerInput`
- `OutboundQueue`: events like `TcpBroadcast`, `UdpSnapshot`,
  `UdpDeltaSnapshot`, `UdpEvent` (`server/queues.h:61-66`)

Joining and leaving are **commands, not direct mutations**: the network
thread completes the handshake and pushes `CMD_PLAYER_JOINED`; the sim
applies it at the next tick boundary (`server/sim.cpp:145-179`). That's
why the roster needs no lock — one thread only ever touches it.

### 6.2 Why not thread-per-client?

At n=10, ten threads contending over shared game state buys nothing but
race conditions. Two threads with exclusive ownership means the
correctness argument is one sentence: *no data is ever shared except
through the queues.*

### 6.3 Lock discipline — three rules, no exceptions

(`server/queues.h:7-11`)

1. **Never hold a lock across a syscall.** Copy into/out of the deque,
   unlock, then do I/O. A blocked `send()` holding a lock stalls the other
   thread's entire game.
2. **Never hold both locks at once.** With zero nesting, deadlock is
   impossible *by construction*, not by careful review.
3. **Lock only inside `queues.cpp`.** Enforced by grep; `pthread_mutex_t`
   appears nowhere else.

Locks are wrapped in an 8-line RAII guard so an early `return` can't leak
one (`server/queues.cpp:9-15`) — still raw pthreads, satisfying the
course requirement, without manual lock/unload discipline.

The queues themselves are trivially simple copy-in/copy-out deques
(`server/queues.cpp:17-45`). Outbound events use a fixed 512-byte inline
payload array rather than heap allocation per event
(`server/queues.h:79-91`).

### 6.4 Waking the network thread instantly

After publishing a snapshot the sim writes 8 bytes to an **eventfd**
registered in the poll set (`server/sim.cpp:583-587`; fd created at
`server/net_server.cpp:119-120`, drained at `:531-534`). The network
thread wakes immediately regardless of its poll timeout.

Why this matters: the naive alternative is a short poll timeout plus
"check the queue each wakeup" — that's a busy-wait pretending to be a
design. Eventfd gives sub-millisecond wake latency with zero spinning,
independent of the poll timeout value.

Wiring order matters: the eventfd is created and handed to the sim
**before either thread starts** (`server/main.cpp:92`), so there is no
startup race.

---

## 7. The tick loop

The sim advances the world in fixed 33.3 ms steps, in a **fixed order**:

```
drain inbound → pop one input/player → movement → combat
→ flags → win check → publish snapshot/events          (server/sim.cpp:594-603)
```

Fixed order matters because results depend on it: combat before or after
movement changes who gets shot. Client and server don't need to agree on
this order (clients don't simulate combat) but tests rely on its
determinism.

Pacing uses an **absolute deadline** (`server/sim.cpp:44-69`):

```cpp
next += TICK_NS;                       // absolute target
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next);
```

Sleeping a *fixed* 33 ms accumulates the tick's own execution time as
drift (tick takes 3 ms → actual rate 27.5 Hz). Sleeping to an absolute
deadline is drift-free. And if the process stalls more than 3 ticks
behind (debugger, page cache flush), the deadline resyncs to now instead
of trying to catch up by sprinting (`server/sim.cpp:56-64`) — catch-up
would produce a burst of ticks and a visible teleport.

Per-player inputs live in a **ring buffer of 8**, and the sim pops
**exactly one input per player per tick** (`server/sim.cpp:185-200`).
This invariant is what makes reconciliation possible (§11):

- ring empty (loss): apply a zero-movement input, ack unchanged — the
  player stalls one tick; repeating the last input would let loss be
  exploited into free movement
- ring full (faster than 30 Hz): drop oldest (`server/sim.cpp:163-168`)
- duplicate/reordered seqs ignored (`server/sim.cpp:161-162`)

---

## 8. I/O multiplexing: poll vs epoll

One thread must watch up to 13 fds (listen socket, UDP socket, eventfd,
up to 10 client TCP sockets) without blocking on any single one. All
sockets are non-blocking (`shared/net_util.h:17`) and multiplexed through
an `IPoller` interface (`server/poller.h:17-28`) with two swappable
backends chosen by `--poller poll|epoll` (`server/main.cpp:80-82`):

- `poll(2)` — classic level-triggered readiness list; O(fds) per call
- `epoll(2)` — kernel keeps the interest set; O(ready) per call

**Measured result** (`docs/benchmark.md`, n=10 bots, loopback): both
backends hold ~31.4 Hz observed snapshot rate, worst interval stddev
~6.9 ms, server CPU 0.09 s over 10 s — statistically identical.

The defensible conclusion: **epoll's advantage appears at hundreds/thousands
of fds; at n=10 the difference is noise, and we measured it rather than
asserting it.** Swappable backends cost one interface and let us turn an
assumption into data. POLLOUT registration toggles dynamically: a client's
socket is watched for writability only while its pending-output buffer is
non-empty (`server/net_server.cpp:155-161`) — otherwise the poll set spins
on always-writable sockets.

---

## 9. The outbound path: snapshots and events

Full journey of one snapshot, sim → air:

1. **Sim publishes** (`server/sim.cpp:510-588`): builds the `WorldSnapshot`,
   serializes it ONCE into the outbound event, attaches the
   per-player `last_input_seq` table (`ev.acks[]`), pushes to the outbound
   queue, then writes the eventfd.
2. **Network thread wakes**, drains the queue (`handle_wake`,
   `server/net_server.cpp:531-597`).
3. **Per-recipient patch + sendto** (`server/broadcast.cpp:27-68`): the
   8-byte UDP header is stamped (magic/version/type/tick), then for each
   registered client the 4-byte `last_input_seq` at payload offset 0 is
   overwritten with *that client's* ack and one `sendto` fires. Delta
   snapshots reuse the exact same patch path because they keep the same
   field at offset 0 (`server/net_server.cpp:544-553`).
4. **TCP events** are framed once (`[u16 len][u8 type][payload]`,
   `server/net_server.cpp:33-42`) and appended to every client's
   `pending_out` buffer (`broadcast.cpp:14-25`).

### Backpressure: slow clients must not hurt everyone

Non-blocking `send()` can accept only part of a write (EAGAIN mid-message),
so each client carries a persistent pending buffer flushed on POLLOUT
(`flush_pending`, `server/net_server.cpp:297-316`). Unbounded buffering
would turn one stalled client into a server-wide memory problem, so a
client whose buffer exceeds **64 KB is disconnected**
(`enforce_pending_cap`, `server/net_server.cpp:599-608`;
constant at `shared/game_config.h:116`). This cap is tested with a
deliberately-stalled socketpair client.

Note the symmetry with rule 1 in §6.3: buffers accumulate under no lock —
the registry belongs wholly to the network thread.

---

## 10. Disconnect handling: two independent detectors

Transports fail differently, so detection is split:

- **TCP detector:** `recv()==0` (clean close) or a hard error is
  definitive — close fd, drop from poller, free the slot
  (`server/net_server.cpp:222-234`, teardown at `disconnect()`, `:622-641`).
- **UDP silence detector:** a client can stop sending input while TCP stays
  technically open (suspended laptop, stalled Wi-Fi). Track
  `last_input_tick` per player; **3 seconds** of silence disconnects them
  even though TCP looks fine (`check_silence_timeouts`,
  `server/net_server.cpp:610-620`; constant at `shared/game_config.h:112`).
  Checked at most every 100 ms to avoid rescanning the registry per event.

On either path the same cleanup runs: drop their carried flag where they
stood (so it isn't deleted from the game — `server/sim.cpp:131-143`),
notify the sim via a `PlayerLeft` command, rebroadcast LOBBY_STATE, and
promote a new host if needed. Even rejected clients (roster full) get a
clean rollback so no registry slot leaks (`server/net_server.cpp:330-344`,
`close_unregistered`, `:643-651`).

SIGINT shuts the server down cleanly: signal handler sets an atomic flag,
main stops both threads and joins them (`server/main.cpp:73`, `109-118`).
`SIGPIPE` is ignored so writing to a dead peer doesn't kill the process
(`server/main.cpp:74`).

---

## 11. Latency compensation

Three distinct mechanisms, kept in three separate files. Getting their
*scope* right is the heart of the design:

| Mechanism | Applies to | File | Plain words |
|---|---|---|---|
| Prediction | your own player | `client/prediction.cpp` | act on input now; don't wait RTT |
| Reconciliation | your own player | `client/prediction.cpp` | when the server disagrees, snap + replay |
| Interpolation | everyone else | `client/interpolation.cpp` | render others slightly in the past, smoothly |

Your own player is predicted, **never** interpolated; remote players are
interpolated, **never** predicted. No overlap.

### 11.1 Prediction — play the game locally

Each client tick (same 30 Hz as the server): increment seq, sample input,
apply `movement_step` **locally immediately** (`on_local_tick`,
`client/prediction.cpp:24-35`), and push `{seq, input}` onto a history
ring. Movement feels instant even though the round trip is tens of ms.

Because prediction calls the *same* `shared/movement_step` the server runs
(`shared/movement.cpp:23`), prediction matches authority by construction —
there aren't two physics implementations to drift apart.

### 11.2 Reconciliation — apologize when wrong

When a snapshot arrives (`client/prediction.cpp:37-93`):

1. Drop stale snapshots (`snap.tick <= last_applied_tick`) — UDP reorders
   (`:42-43`).
2. Read the server's authoritative position for me and my `last_input_seq`
   ack.
3. Discard history entries with seq ≤ ack (`:78-80`).
4. **Snap** local state to the authoritative position, then **always
   replay** all remaining unacked inputs through `movement_step`
   (`:86-92`).

Replay-always, no branch on "was there a misprediction?": if prediction
was correct, deterministic replay reproduces the same state and nothing
visibly moves; branching saves microseconds and adds a place for bugs to
hide. Mispredictions are counted purely as HUD instrumentation
(`:66-74`) — on a healthy link it stays near zero, which is itself the
demo metric.

History depth ≈ RTT × 30 — 2–4 entries on a LAN; the ring caps at 64
(`shared/game_config.h:140`).

### 11.3 Interpolation — watching others 66 ms in the past

Remote entities are rendered from a ring of the last ~30 snapshots,
targeting `newest_tick − 2` (a deliberate 2-tick ≈ 66 ms delay;
`shared/game_config.h:133`). Each frame finds the two snapshots
bracketing the fractional `render_tick` and lerps positions
(`Interpolation::sample`, `client/interpolation.cpp:88-130`).

Three subtle details worth citing:

- **Angle wraparound:** aim_angle is a u16 covering 0..2π. Naive lerp from
  65000→500 spins almost a full revolution; we interpolate the signed
  16-bit delta — shortest arc (`lerp_angle`, `:30-37`).
- **Clock drift:** local clock ≠ server clock, so render_tick speed nudges
  1.02×/0.98× depending on buffer depth — rate-corrected, never jumped; a
  jump is a visible pop (`advance`, `:64-86`).
- **Stall behavior:** on packet loss the clamp at `:83` makes the renderer
  hold the last known position instead of extrapolating. Extrapolation
  overshoots then snaps back — worse than a brief freeze.

The trade to articulate: we give up 66 ms of freshness for smoothness.
Flip `--snapshot-rate 10` in the demo and interpolation's value becomes
visible to the naked eye.

Debug tooling doubles as evaluation methodology: F1 prediction off, F2
interpolation off, F3 "server ghost" showing the raw authoritative
position beside the predicted one (`docs/README.md` §6.6). When the ghost
sits exactly on you, reconciliation is provably converging.

### 11.4 What is never predicted — and why that's the anti-cheat answer

Movement only. Not shooting, hits, flag pickup, death. Predicting a pickup
means occasionally showing a grab and yanking it back — worse than a 30 ms
delay. Structurally, the client predicts nothing that affects the outcome:
hitscan is computed entirely server-side (`server/sim.cpp:316-367`), so
the only thing a hacked client achieves is a prettier lie about its own
movement, which the next snapshot corrects.

---

## 12. Optimization: delta-compressed snapshots

(The full write-up is `docs/optimization.md`; numbers in
`docs/benchmark_snapshots.md`.)

### The problem

Full snapshots cost ~128 bytes × 30 Hz ≈ 3.9 KB/s per client regardless
of what happened — yet in real play most fields are unchanged most ticks.

### The mechanism

With `--snapshots delta` (default), the server publishes:

- a **full WORLD_SNAPSHOT keyframe every 10th publish**
  (`kSnapshotKeyframeInterval = 10`, `shared/game_config.h:125`;
  cadence logic `server/sim.cpp:553-559`), and
- a **DELTA_SNAPSHOT (type 19)** in between, carrying only changed fields
  against the previous publish: a header bitmask, a `u16 player_change_mask`,
  and per-changed-player field masks (`encode_delta_snapshot`,
  `shared/protocol.cpp:345`; layout in `docs/HANDOFF.md` §2).

An idle player contributes zero bytes; an idle *world* produces an ~8-byte
payload. Positions are compared at **wire granularity** (i16) so the
encoder never emits a change the decoder can't represent.

### Loss handling without ACKs (the interesting part)

Deltas are useless without their baseline, and UDP loses packets. Instead
of building a NACK channel (more protocol, more states):

1. Each delta names its baseline: `baseline_ticks_ago`.
2. The client caches its last applied snapshot
   (`client/net_client.cpp:477`). If `cache.tick != hdr.tick − baseline`,
   something upstream was lost → the delta is dropped and the cache is
   invalidated (`:483-496`).
3. Updates stall until the next keyframe — bounded at 10 snapshot
   intervals ≈ **333 ms** worst case. Then everything resumes.

No client action needed; UDP stays fire-and-forget. Decoder applies onto
a copy and commits only on success, so malformed deltas can't corrupt the
cache (fuzz-tested truncation at every offset).

### Version bump 1 → 2

A stale v1 binary receiving type 19 would mis-decode silently. Bumping
`kProtocolVersion` (`shared/protocol.h:23`) turns that into a clean
`JOIN_REJECT BadVersion` at the handshake.

### Measured result

n=10 bots, loopback (`tools/bench_bandwidth.sh`):

| mode | UDP KiB/s/client |
|---|---|
| full | 3.42 |
| delta | **1.85 (−46%)** |

Win scales with idle time (constant motion is the pathological worst case,
still −15%). Crucially, **only the transport changed**: prediction,
reconciliation, interpolation consume the same decoded full
`WorldSnapshot` as before — deltas decode onto the cache and surface as
ordinary snapshots (`client/net_client.cpp:483-496`).

Interest management came free: the per-player change mask *is* relevance
filtering. Nobody relevant-misses anything on a 1280×800 map, so no
distance culling was added (`docs/optimization.md` §2).

---

## 13. Determinism: integers everywhere

One function is singled out as "must never fork":

```cpp
PlayerMotion movement_step(const PlayerMotion& in,
                           const InputCmd& cmd, const Map& map);
```

(`shared/movement.cpp:23-65`) Position+input in, position+velocity out.
Integer-only, no randomness, no static mutable state, no I/O. Both the
server (authority) and every client (prediction) call this exact compiled
code, so identical input sequences produce identical states — that's the
precondition for replay-based reconciliation (§11.2).

Details that preserve determinism:

- instant velocity, no acceleration/momentum — momentum would double the
  state that must be predicted and replayed
- diagonal normalized by 181/256 ≈ √½ in integer math, else diagonals are
  41% faster (`:44-47`)
- collision resolves **X fully, then Y** — the axis order is part of the
  contract; swapping it changes corner-sliding results (`:51-59`)
- AABB colliders (no circles) because circles need square roots
  (`map.aabb_collides`)

Proof, not hope: the test suite runs 10,000 random inputs through the step
twice and asserts byte-identical output.

---

## 14. Input redundancy: surviving packet loss for free

Every PLAYER_INPUT datagram carries the **last 3 inputs** (`base_seq` +
3 × {buttons u8, aim_angle u16}, ~27 bytes total;
`shared/game_config.h:43`). If one datagram is lost, the next one already
contains its inputs — single-packet loss becomes invisible for ~6 extra
bytes.

Server side: the network thread expands one packet into up to 3 queued
commands (`server/net_server.cpp:514-524`), and the sim ignores any seq
it has already queued (`server/sim.cpp:161-162`). Redundant re-sends are
free; strictly increasing sequences keep ordering.

Most apparent "network jitter" in naive implementations is actually
single-packet input loss — this was the highest value-per-line change in
the project (`docs/README.md` §5.4).

---

## 15. How we tested all of this

TDD throughout: failing tests first, then implementation. Current suite:
**141 cases / 18,444 assertions**, green on normal and ASan builds.

| Risk | Test |
|---|---|
| movement nondeterminism | 10k random inputs ×2, byte-identical |
| codec bugs | round-trip every one of the 19 message types |
| malicious/garbage packets | fuzz: 1-byte datagrams, bad magic/version, truncation at every offset |
| TCP stream splitting | artificial split-read reassembly test |
| races/deadlocks | TSan on N-producer queue stress; mutexes grep-confined to `queues.*` |
| memory bugs | ASan build clean (even caught a test-side overread) |
| snapshot patch correctness | two recipients' buffers differ ONLY in the 4 patched bytes (byte-diff) |
| delta robustness | stale-baseline rejection, keyframe cadence, end-to-end loss-and-recovery over real sockets |
| slow clients | 64 KB pending-cap disconnect over socketpairs |
| silence timeouts | 3 s UDP-silence detector, independent of TCP state |
| tick stability | stall-resync test; observed ~31 Hz under 10-bot load (both pollers) |
| bandwidth claims | measured tables in `docs/benchmark*.md`, produced by committed scripts |
| adverse networks | `tools/netem.sh` (delay/jitter/loss on loopback) |
| load generation | `tools/loadgen.py` — an independent Python reference client that also validates the spec |

That last point is worth saying aloud in defense: the Python client proves
the protocol is implementable from the documentation alone, not just from
our C++ headers.

---

## 16. Decision table: "why did you choose X?"

| Question | Answer | Where |
|---|---|---|
| Why TCP **and** UDP? | Reliability where needed, timeliness where needed; snapshot = truth, TCP = notifications | §2, `docs/README.md` §2 |
| Why not thread-per-client? | Contention for nothing at n=10; 2 threads ⇒ 1-sentence correctness argument | §6.1 |
| Why only 2 mutexes? | All cross-thread contact is the queues; nesting banned ⇒ deadlock impossible by construction | §6.3 |
| Why eventfd? | Instant wake independent of poll timeout; timeout-polling the queue is a disguised spin loop | §6.4 |
| Why `TIMER_ABSTIME`? | Fixed sleeps accumulate execution-time drift; absolute deadlines don't | §7 |
| Why poll *then* epoll? | Interface swap turned an assertion into a measurement; at n=10 they tie | §8 |
| Why fixed-point ints? | Deterministic replay across compilers; floats desync invisibly | §13 |
| Why one input per tick? | Server must apply exactly what the client predicted with, in order | §7, §11 |
| Why 3 redundant inputs? | Single-packet loss invisible for ~6 bytes | §14 |
| Why session tokens? | UDP is spoofable; token + source-address check on *every* packet | §4 |
| Why length-prefix TCP? | recv() merges/splits messages; framing is mandatory, 4 KB cap bounds abuse | §3.1 |
| Why per-recipient patching? | Each client needs its own ack; serialize once, patch 4 bytes per recipient | §9 |
| Why 64 KB pending cap? | One slow client must not become a server-wide memory problem | §9 |
| Why hitscan, no projectiles? | Projectiles add entities/lifecycle/interpolation; hitscan is one ray per shot, server-side | `server/sim.cpp:316` |
| Why capture gated on own-flag-at-base? | The single rule that makes defense matter | `server/sim.cpp:446-465` |
| Why no mid-match join? | Requires full-state seeding of a late client; deferred honestly | `docs/README.md` §12 |
| Why delta snapshots + keyframes, no NACKs? | Bounded staleness (≤333 ms) beats new protocol states; −46% bandwidth | §12 |
| Why interpolate others at −2 ticks? | Smoothness for 66 ms staleness — a trade we can demo with `--snapshot-rate 10` | §11.3 |
| Why replay-always in reconciliation? | Correct replay is a visual no-op; branching hides bugs | §11.2 |
| Why a headless bot? | Same wire code minus graphics = load generator + integration tester | §15 |

---

## 17. Likely defense questions with model answers

**Q1. Walk me through what happens when a player presses a key, end to end.**
Client samples input at its 30 Hz tick → increments seq → applies
`movement_step` locally (prediction) → packs last 3 inputs into
PLAYER_INPUT over UDP → server validates magic/version/token/source →
expands redundant inputs into queue commands → sim drains the queue at
tick start → dedups seqs into an 8-slot ring → pops exactly one input →
moves the player → publishes snapshot with that player's ack patched in →
eventfd wakes the network thread → per-recipient `sendto` → client snaps
to the authoritative position and replays any unacked inputs
(reconciliation) → meanwhile every other client interpolates this player
between snapshots.

**Q2. What happens when a UDP packet is lost?**
Depends which one. Lost *input*: the next datagram carries it (redundancy);
if all 3 copies are lost the ring is empty that tick and the server applies
zero-movement — one stalled tick, corrected next snapshot. Lost
*snapshot*: the next one supersedes it (that's why we use UDP); in delta
mode the client detects the broken baseline and stalls ≤333 ms to the next
keyframe. Nothing retransmits, because stale data is worthless.

**Q3. What happens when a TCP packet is lost?**
Nothing user-visible — the kernel retransmits; the stream guarantees
order. Cost is potential head-of-line latency, acceptable because TCP
carries rare events, not per-tick data.

**Q4. Why does the client trust the snapshot's `last_input_seq`? How does
it know which inputs to replay?**
Inputs are numbered with a monotonically increasing seq. The snapshot's
patched field names the newest applied seq; the client discards history
≤ ack and replays the rest, ascending, from the authoritative position
(`client/prediction.cpp:78-92`).

**Q5. How do you prevent desync between client and server physics?**
By construction: both link one `shared/movement.cpp`; integer-only math;
all constants single-sourced in `game_config.h`; X-then-Y collision order
baked into the shared function; verified by a 10k-input determinism test.
Plus the F3 ghost debug overlay makes any residual divergence visible.

**Q6. How do you prevent cheating?**
Authoritative server: clients send intent, never outcomes. Hitscan,
damage, flags, scoring are all computed server-side. Session tokens +
source-address checks stop identity spoofing on UDP. A modified client can
at most mis-render its own movement until the next snapshot corrects it.
Lag compensation (rewinding hitboxes) is documented future work — today a
high-latency shooter must lead their target.

**Q7. Your server has two mutexes. Walk me through how deadlock is impossible.**
Only `queues.cpp` locks; each method acquires exactly one mutex, copies one
item, releases. No code path ever holds both queue locks, and no lock is
held across a syscall. Deadlock requires circular wait; with single-lock
methods there is nothing to circle.

**Q8. Why is the snapshot body serialized once per tick?**
It's identical for everyone except 4 bytes. Serializing per recipient
wastes CPU at 10 clients; we serialize once and patch each recipient's ack
at a known offset before `sendto` (`server/broadcast.cpp:55-63`).

**Q9. What if two clients have the same player_id, or someone sends inputs
for another player?**
Duplicate joins are rejected; every UDP packet must carry the matching
session token AND originate from the registered address
(`server/net_server.cpp:482-495`) or it is dropped before touching the sim.

**Q10. Why 30 Hz? What changes at 60 Hz?**
30 Hz halves bandwidth and leaves ample CPU; interpolation masks the
choppiness visually. The rate is a constant (`kTickRateHz`), and
`--tick` overrides it at runtime — the design doesn't care. Input above
30 Hz gains nothing (full ring drops oldest).

**Q11. Explain the delta snapshot baseline problem again — why no NACKs?**
A delta says "apply these diffs to the snapshot from N ticks ago." If that
baseline never arrived, the diff is unapplicable. Options: NACK channel
(new message, server-side retry state, ordering problems) or wait for the
periodic keyframe (already required for late/lost-baseline clients). We
chose waiting: zero added protocol states, bounded ≤333 ms staleness, and
bandwidth still fell 46%.

**Q12. Poll returned identical performance to epoll in your benchmark.
Does that mean epoll is pointless?**
At this scale, yes — 13 fds is far below where epoll's O(ready) vs O(fds)
difference matters. The point was methodology: we built the `IPoller` seam
and measured instead of assuming. At thousands of clients epoll wins; our
data shows where that crossover begins to matter.

**Q13. What happens if the host disconnects mid-lobby? Mid-match?**
Mid-lobby: lowest remaining id is promoted and LOBBY_STATE tells everyone
(`server/lobby.cpp:37-59`). Mid-match: their flag drops where they stood,
the sim is notified via command, roster updates — and the roster survives
MATCH_END so a rematch starts without reconnects.

**Q14. How do you know the tick rate is stable?**
Absolute-deadline pacing with resync-instead-of-catch-up, plus measurement:
loadgen reports per-client observed snapshot intervals (~31.4 Hz min across
10 clients, stddev <7 ms, both pollers — `docs/benchmark.md`). Actual rate
is also on the client HUD.

**Q15. A malformed packet arrives — 1-byte UDP datagram, or a TCP frame
claiming 10 KB. What happens?**
All dropped silently. UDP: header decode fails before any routing
(`server/net_server.cpp:456-458`). TCP: frames over the 4 KB cap
disconnect the sender rather than accumulate
(`server/net_server.cpp:237, 256-259`); unknown types are ignored. Fuzz
tests cover truncation at every byte of every message.

**Q16. Why is SHOT_FIRED on UDP but PLAYER_KILLED on TCP?**
Tracer is cosmetic — losing one is invisible, and head-of-line-blocking
TCP could delay it meaninglessly behind older traffic. Death changes game
state everyone agrees on, must never be lost or reordered.

**Q17. What's the hardest bug you hit?**
Good candidates with real stories: (a) shots quantized through a coarse
trig table + origin at box corner instead of center — fixed by full 16-bit
direction and center-origin casting (`server/sim.cpp:219-267`); (b) bots
wedged on wall corners from memoryless greedy steering — replaced with BFS
flow fields (`docs/optimization.md` §3); (c) loadgen discarding bytes after
JOIN_ACCEPT causing permanent TCP frame misalignment — a textbook framing
bug found by the benchmark harness.

**Q18. What would you do differently / next?**
Documented future work: lag compensation for fair high-latency shooting,
mid-match joining (state seeding), internet-scale concerns (NAT traversal,
interest management by distance). Also honest limits: rand()-based tokens
are fine for a LAN threat model only; heartbeat exists for spec compliance
but liveness really rides on the two detectors.

**Q19. Why is the client allowed to use floats in interpolation but not in
movement?**
Interpolation output is display-only — it never feeds back into
simulation or reconciliation, so rounding differences can't accumulate
anywhere (`client/interpolation.cpp:8-14`). The "no floats" rule scopes to
anything that participates in the deterministic movement path.

**Q20. How does the server know a client's UDP address, and what if it
changes?**
From UDP_HELLO and refreshed implicitly by validated input packets; if the
source address differs from the registered one the packet is dropped, and
the client recovers by re-helloing (its 200 ms resend loop handles the
initial case; NAT rebinds would need the same).

**Q21. Ten clients each get a personalized snapshot — doesn't that mean 10
serializations?**
No: one serialization + ten 4-byte patches + ten `sendto`s. Total UDP
traffic at n=10 measured at 1.85 KiB/s/client in delta mode.

**Q22. Why write your own protocol instead of protobuf/etc.?**
Course focus is the raw socket layer; hand-rolled field-by-field codecs
with ByteWriter/ByteReader teach endianness, framing, and bounds-checking
that a library hides. Dependency-free also means the bot's Python reference
client can validate the spec independently.

**Q23. What does `--snapshot-rate 10` actually change?**
Only UDP send cadence — the sim still ticks 30 Hz and gameplay fidelity is
untouched; publish decimation happens at the emit step
(`server/sim.cpp:553-554`). Built as a demo knob: at 10 Hz interpolation's
smoothing is obvious to the eye.

**Q24. Is there any scenario where the two threads touch the same data?**
Only momentary, inside the queue push/pop — one mutex each, copying values
by value (`OutboundEvent` carries a fixed inline array precisely so the
handoff never shares heap pointers, `server/queues.h:79-91`). TSan stress
with multiple producers found nothing.

**Q25. How would this scale to 100 players?**
Honest answer: not as-is. Snapshots grow linearly (9 B/player), the single
sim thread saturates, and per-recipient sends multiply. Fixes in order:
delta snapshots already help; next comes spatial partitioning/interest
management, sharded simulation, then the real fixes — region-based
authority or moving to an established engine model. Our architecture
(queue seam, IPoller seam, pure tick function) is structured so each of
those is an isolated substitution, which is the actual lesson of the
project.

---

## 18. The POSIX socket layer: every network call, in real code

This is a network programming course, so expect *"show me the actual
syscall."* This section walks every socket API the project uses, with real
code and what each argument means. Everything here is raw POSIX — no
wrapper library, no boost, no asio.

### 18.1 Creating the two server sockets — `socket()`, `setsockopt()`, `bind()`, `listen()`, `getsockname()`

`server/net_server.cpp:91-130` (`NetServer::start()`):

```cpp
// ---- TCP listening socket --------------------------------------------
tcp_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);        // :92
//            ^IPv4     ^byte-stream = TCP

const int one = 1;
setsockopt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR,
           &one, sizeof(one));                            // :95-96

fill_sockaddr(addr, port_);
bind(tcp_listen_fd_, (sockaddr*)&addr, sizeof(addr));    // :100
listen(tcp_listen_fd_, 8);                               // :104
//                   ^backlog: kernel-queued pending connections
getsockname(tcp_listen_fd_, (sockaddr*)&addr, &len);     // :106
local_port_ = ntohs(addr.sin_port);   // learn our own port after bind

// ---- UDP socket -------------------------------------------------------
udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);                // :109
//            ^datagram = UDP  — same bind pattern, port 0
fill_sockaddr(addr, 0);              // port 0 -> kernel picks ephemeral
bind(udp_fd_, ...); getsockname(...);                    // :111-117
udp_port_ = ntohs(addr.sin_port);
```

The address structure, built once by `fill_sockaddr`
(`server/net_server.cpp:44-51`):

```cpp
addr.sin_family      = AF_INET;
addr.sin_addr.s_addr = htonl(INADDR_ANY);  // accept from ANY interface
addr.sin_port        = htons(port);        // network byte order!
```

Three things to be able to explain:

- **`SO_REUSEADDR`** — lets the server rebind immediately after restart,
  instead of waiting out the TCP `TIME_WAIT` state left behind by old
  connections.
- **`INADDR_ANY`** (`0.0.0.0`) vs `127.0.0.1`: binding loopback would make
  the game single-machine only; this is a LAN game, so we accept on all
  interfaces.
- **`htons`/`ntohs`/`htonl`** — byte order. x86 is little-endian; the
  network is big-endian ("network byte order"). Every port/IP field going
  into a `sockaddr_in` must be converted, and so must every integer on our
  wire format (that's why `ByteWriter::u16` writes high byte first).

The same pattern appears client-side in `client/net_client.cpp:88-117`:
`socket()` + `connect()` for TCP (:88-99), then a second `socket()` for
UDP **bound to port 0** (:102-116) — the kernel assigns an ephemeral local
port, which is precisely why the server can't know where to send snapshots
until `UDP_HELLO` arrives (§5). Hostname resolution happens first via
`getaddrinfo()` (:73-86), which converts `"192.168.1.10"` + `"7777"` into
a filled-in `sockaddr_in`.

### 18.2 Making everything non-blocking — `fcntl()`

`shared/net_util.cpp:16-22`:

```cpp
bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);       // read current flags
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
```

By default `recv()`/`send()`/`accept()` **sleep** until they can proceed.
A server thread that blocks inside one client's `recv()` stops servicing
everyone else. Setting `O_NONBLOCK` makes those calls return `-1` with
`errno == EAGAIN/EWOULDBLOCK` when there's nothing to do — which only
makes sense combined with readiness multiplexing (§18.6): we call them
only *after* poll says the fd is ready, so EAGAIN becomes rare instead of
constant spinning.

Applied at `server/net_server.cpp:122-123` to both server sockets, at
`:183` to every accepted client fd, and to the client's UDP socket
(`client/net_client.cpp:117`).

### 18.3 Accepting connections — `accept()`

`server/net_server.cpp:176-186`:

```cpp
void NetServer::accept_new_clients() {
    for (;;) {
        sockaddr_in peer{}; socklen_t len = sizeof(peer);
        const int fd = accept(tcp_listen_fd_,
                              (sockaddr*)&peer, &len);
        if (fd < 0) break;                  // EAGAIN: queue drained
        set_nonblocking(fd);                // new fd inherits nothing!
        poller_->add(fd, false);            // start watching it
    }
}
```

Key points: the TCP three-way handshake is completed by the *kernel* (that's
the `listen()` backlog's job); `accept()` just pops an established
connection off that queue and returns a **new dedicated fd** per client.
The peer address from `accept` is available but we deliberately don't use
it for identity — identity comes from the handshake + session token (§4).
Loop-until-EAGAIN handles several clients connecting in one wakeup.

### 18.4 Reading TCP — `recv()` + stream reassembly

Per-client read, `server/net_server.cpp:215-243`:

```cpp
uint8_t chunk[2048];
for (;;) {
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n == 0) { disconnect(...); return; }          // peer closed (EOF)
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // drained
        if (errno == EINTR) continue;                 // signal interrupted
        disconnect(...); return;                      // hard error
    }
    accum.insert(accum.end(), chunk, chunk + n);      // append to buffer
    if (accum.size() > kAccumCap) { disconnect(...); }  // 4 KB cap
    if (n < (ssize_t)sizeof(chunk)) break;
}
```

The three return-value cases of `recv()` are exam gold:

| Result | Meaning | Our action |
|---|---|---|
| `> 0` | bytes received (**any** number — half a message or three messages) | append to accumulation buffer |
| `== 0` | orderly shutdown by peer (EOF) | definitive disconnect |
| `< 0`, `EAGAIN` | non-blocking, nothing more right now | stop reading, back to poll |
| `< 0`, other | RST / broken pipe etc. | definitive disconnect |

Then framing turns the stream back into messages — `recv_framed()`,
`shared/net_util.cpp:47-71`:

```cpp
if (len < 3) return 0;                       // not even a header yet
const uint16_t payload_len = (buf[0] << 8) | buf[1];   // big-endian peek
if (payload_len > kTcpFrameCapBytes)
    return SIZE_MAX;                         // oversize -> caller disconnects
if (len < 3 + payload_len)
    return 0;                                // frame still incomplete
out_payload = buf + 3;                       // point INTO the buffer
return 3 + payload_len;                      // bytes this frame occupies
```

Zero-copy: it returns a pointer into the caller's buffer plus consumed
length; the caller dispatches, then erases exactly those bytes
(`server/net_server.cpp:292-294`). This exact function is unit-tested with
artificially split reads.

### 18.5 Writing TCP — `send()`, partial writes, `MSG_NOSIGNAL`

Two layers:

**(a) The primitive**, `shared/net_util.cpp:24-45`:

```cpp
ssize_t send_all(int fd, const void* buf, size_t len) {
    while (sent < len) {
        const ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += n; continue; }
        if (EAGAIN || EWOULDBLOCK)
            return sent;          // socket buffer full: caller buffers rest
        if (EINTR) continue;      // retried after signal
        return -1;                // EPIPE / ECONNRESET: dead peer
    }
}
```

Like `recv()`, `send()` may accept **less than requested** — the kernel
socket send buffer filled up. `MSG_NOSIGNAL` matters: writing to a peer
that already closed raises `SIGPIPE`, whose default action kills the whole
process. We ignore SIGPIPE globally too (`server/main.cpp:74`) as belt and
braces; the client uses blocking sends through this same helper
(`client/net_client.cpp:221-230`).

**(b) The backpressure layer**, `flush_pending`
(`server/net_server.cpp:297-316`): broadcast events are appended to each
client's `pending_out` deque; POLLOUT says "writable now"; we drain as much
as the kernel accepts, keep the remainder, and cap at 64 KB
(`enforce_pending_cap`, `:599-608`). This is how one stalled Wi-Fi client
is prevented from stalling or bloating the whole server.

### 18.6 UDP receive — `recvfrom()`

`server/net_server.cpp:437-450`:

```cpp
uint8_t buf[2048];
for (;;) {
    sockaddr_in src{}; socklen_t len = sizeof(src);
    const ssize_t n = recvfrom(udp_fd_, buf, sizeof(buf), 0,
                               (sockaddr*)&src, &len);   // <- sender addr OUT
    if (n <= 0) break;               // EAGAIN: datagram queue drained
    on_udp_packet(src, buf, n);
}
```

Contrast with TCP: no accept, no connect, no EOF (`recvfrom` never returns
0-with-meaning — UDP has no connection to close), and crucially each call
yields **exactly one complete datagram** with the sender's address, which
is what enables the token-vs-source-address check in `auth_udp_sender`
(§4). One socket serves all 10 players simultaneously — there is no
per-client UDP fd, just per-client registered addresses.

Client mirror: `client/net_client.cpp:367-393` — same loop, plus dropping
datagrams shorter than the 8-byte header before any parsing.

### 18.7 UDP send — `sendto()`

`server/broadcast.cpp:39-66` — the whole snapshot transmit path:

```cpp
std::array<uint8_t, 600> dgram;                 // stack, no heap per tick
dgram[0] = kMagic >> 8;  dgram[1] = kMagic & 0xFF;   // hand-stamped header
dgram[2] = kProtocolVersion;
dgram[3] = udp_msg_type;                        // 9 = full, 19 = delta
/* dgram[4..7] = u32 tick, big-endian */

registry.for_each_live([&](const ClientEntry& e) {
    const size_t off = config::kUdpHeaderBytes;         // patch offset 0
    dgram[off+0..3] = acks[e.player_id];    // THIS client's last_input_seq
    sendto(udp_fd_, dgram.data(), dgram_size, 0,
           (sockaddr*)&e->udp_addr, sizeof(e->udp_addr));  // per-recipient
});
```

`sendto` differs from `send` by taking the destination address per call —
no connection state exists. Note what we do *not* handle: delivery. If the
datagram vanishes, nothing retransmits; correctness relies entirely on the
next tick's snapshot (§2) or the keyframe cadence (§12).

### 18.8 Readiness multiplexing — `poll()`

The network thread must wait on ~13 fds at once without blocking on any.
Backend #1, `server/poller_poll.cpp:47-73`:

```cpp
int wait(PollResult* out_results, int max_results, int timeout_ms) override {
    const int ready = ::poll(fds_.data(), fds_.size(), timeout_ms);
    //                          ^array of pollfd ^count  ^bounded wait:
    //                           never blocks forever even if idle
    if (ready <= 0) return 0;                 // timeout or spurious wakeup
    for (auto& p : fds_) {                    // scan ALL fds: O(n)
        r.readable = p.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL);
        r.writable = p.revents & POLLOUT;
        ...
    }
}
```

Registration manipulates the events bitmask dynamically
(`poller_poll.cpp:35-45`): a client's fd carries `POLLOUT` only while its
pending buffer is non-empty — otherwise poll would report "writable"
continuously and busy-loop. Errors/hangups are surfaced as *readable*
(`:65`) so the normal `recv()` path runs disconnect handling uniformly.

The main event loop consuming this lives at `server/net_server.cpp:134-170`:
wait → dispatch by fd kind (eventfd / UDP / listener / client-readable /
client-writable) → toggle POLLOUT interests → enforce caps → silence check.

### 18.9 Readiness multiplexing — `epoll`

Backend #2, `server/poller_epoll.cpp`. Same `IPoller` interface, different
syscalls:

```cpp
epfd_ = epoll_create1(0);                            // one per process
epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);            // register   (:28)
epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);            // change interest (:45)
epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);        // unregister (:35)

epoll_event events[32];
const int n = epoll_wait(epfd_, events, 32, timeout_ms);   // (:58-64)
for (int i = 0; i < n; ++i) { /* only READY fds returned */ }
```

The conceptual difference to articulate: `poll()` hands the kernel the
whole interest list **every call** and returns a list you must scan
(O fds per event); `epoll` keeps the interest list **in the kernel**
(`EPOLL_CTL_ADD/MOD/DEL` once) and `epoll_wait` returns only ready fds
(O ready). At our n≈13 the difference is unmeasurable — which is why we
measured rather than assumed (`docs/benchmark.md`: both ~31.4 Hz, CPU
identical). The seam making both swappable is `IPoller`
(`server/poller.h:17-28`), chosen at startup by `--poller`.

### 18.10 Thread wake-up — `eventfd`

`server/net_server.cpp:119-120` creates it, `server/sim.cpp:583-587`
signals it, `:531-534` drains it:

```cpp
event_fd_ = eventfd(0, EFD_NONBLOCK);        // an 8-byte counter as an fd

// sim thread, after publishing a snapshot:
const uint64_t one = 1;
write(wake_fd_, &one, sizeof(one));          // counter += 1

// network thread, when poll reports the eventfd readable:
uint64_t val;
while (read(event_fd_, &val, sizeof(val)) > 0) {}   // drain to zero
```

Why an eventfd and not, say, a pipe? It's a single fd, costs one slot in
the poll set, has no buffer to manage, and — the key property — being an
fd, poll/epoll can sleep on it. So the network thread sleeps with an
*arbitrary* poll timeout and still wakes within microseconds of the sim
publishing, instead of polling the outbound queue on a short timer.

### 18.11 Signals — `SIGINT` and `SIGPIPE`

`server/main.cpp:73-74, 109-118`:

```cpp
signal(SIGINT, on_sigint);      // handler ONLY sets std::atomic<bool>
signal(SIGPIPE, SIG_IGN);       // write-to-dead-peer must not kill us
...
while (!g_stop) ::poll(nullptr, 0, 100);   // idle; async-signal-safe
net.stop(); sim.stop();                     // set running_=false
net_thread.join(); sim_thread.join();       // graceful exit 0
```

Rule respected: a signal handler may safely touch only lock-free atomics —
ours sets one flag, and the main loop does all teardown work. `SIGPIPE`
handling was described in §18.5.

### 18.12 Syscall cheat-sheet

| Syscall | Where used | One-line role |
|---|---|---|
| `socket()` | net_server.cpp:92,109 · net_client.cpp:88,102 | create endpoint (SOCK_STREAM / SOCK_DGRAM) |
| `setsockopt(SO_REUSEADDR)` | net_server.cpp:96 | instant rebind after restart |
| `bind()` | net_server.cpp:100,112 · net_client.cpp:112 | attach to address/port (port 0 = ephemeral) |
| `listen(backlog=8)` | net_server.cpp:104 | mark TCP socket passive; kernel completes handshakes |
| `accept()` | net_server.cpp:180 | pop established connection → per-client fd |
| `connect()` | net_client.cpp:93 | client-side TCP handshake initiation |
| `getaddrinfo()` | net_client.cpp:79 | hostname+port string → sockaddr_in |
| `getsockname()` | net_server.cpp:106,116 | learn our own (ephemeral) port after bind |
| `fcntl(O_NONBLOCK)` | net_util.cpp:16-22 | make I/O calls return EAGAIN instead of sleeping |
| `recv()` | net_server.cpp:222 · net_client.cpp:208,332 | TCP read (partial/merged/EOF semantics) |
| `send(MSG_NOSIGNAL)` | net_util.cpp:28 | TCP write loop, partial-write aware |
| `recvfrom()` | net_server.cpp:442 · net_client.cpp:372 | UDP read + sender address |
| `sendto()` | broadcast.cpp:64 · net_client.cpp:246,281 | UDP write to explicit destination |
| `poll()` | poller_poll.cpp:48 · main.cpp:110 | wait many fds, bounded timeout |
| `epoll_create1/ctl/wait` | poller_epoll.cpp:79,28,45,58 | kernel-held interest set, O(ready) waits |
| `eventfd/read/write` | net_server.cpp:119,533 · sim.cpp:585 | fd-able semaphore for cross-thread wake |
| `clock_nanosleep(TIMER_ABSTIME)` | sim.cpp:65 | drift-free tick pacing (§7) |
| `signal(SIGPIPE, SIG_IGN)` | main.cpp:74 | survive writes to closed peers |

---

## Appendix: numbers to memorize

| Quantity | Value |
|---|---|
| Tick rate | 30 Hz (33.3 ms) |
| Max players | 10 |
| Threads / mutexes | 2 / 2 |
| UDP header | 8 bytes (magic 0x4346, version 2) |
| TCP frame header | 3 bytes ([u16 len][u8 type]) |
| Frame cap / pending cap | 4 KB / 64 KB |
| Snapshot size | ~128 B @ 10 players (9 B per player) |
| Bandwidth (delta vs full) | 1.85 vs 3.42 KiB/s/client (−46%) |
| Keyframe interval / worst delta stall | 10 publishes / ~333 ms |
| Input ring / redundancy | 8 slots / 3 inputs per packet |
| Interpolation delay | 2 ticks (≈66 ms) |
| Respawn / flag auto-return | 90 ticks (3 s) / 450 ticks (15 s) |
| Damage / cooldown / win | 34 (3 shots) / 10 ticks / 3 captures or 10 min |
| UDP silence timeout | 3 s |
| Test suite | 141 cases / 18,444 assertions (ASan clean) |

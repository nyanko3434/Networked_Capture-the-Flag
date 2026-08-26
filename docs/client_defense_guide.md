# client_defense_guide.md — The Client Side, in Simple Terms

Defense-preparation document for **Nayan Khusu** (PUL080BCT048), owner of
`client/`, `bot/`, and the tooling (`tools/`). Companion to
`docs/defense_guide.md` (whole-system view); read that one for server-side
questions. This document goes deep on what *your* half does and why —
with real code and `file:line` refs so you can open them live.

---

## Table of contents

1. [The 60-second client pitch](#1-the-60-second-client-pitch)
2. [Client architecture: four modules, one thread](#2-client-architecture-four-modules-one-thread)
3. [The client's two sockets](#3-the-clients-two-sockets)
4. [The handshake, implemented](#4-the-handshake-implemented)
5. [Sending input: the 30 Hz client tick](#5-sending-input-the-30-hz-client-tick)
6. [Receiving: draining both sockets](#6-receiving-draining-both-sockets)
7. [Delta snapshots on the client](#7-delta-snapshots-on-the-client)
8. [Prediction and reconciliation](#8-prediction-and-reconciliation)
9. [Interpolation](#9-interpolation)
10. [HUD and debug tooling — your evaluation methodology](#10-hud-and-debug-tooling)
11. [A subtle bug worth telling as a story: the liveness trap](#11-a-subtle-bug-worth-telling-as-a-story-the-liveness-trap)
12. [The bot: the same wire code, headless](#12-the-bot-the-same-wire-code-headless)
13. [Tooling: loadgen.py, netem.sh, benchmarks](#13-tooling-loadgenpy-netemsh-benchmarks)
14. [Decision table (client edition)](#14-decision-table-client-edition)
15. [Likely defense questions with model answers (client)](#15-likely-defense-questions-with-model-answers-client)
16. [Numbers to memorize (client)](#16-numbers-to-memorize-client)

---

## 1. The 60-second client pitch

> The client is a **dumb terminal with good manners**. It captures keyboard
> and mouse, sends its intent ("holding RIGHT, aiming at angle X") to the
> authoritative server over UDP at 30 Hz, and renders whatever world state
> the server publishes back. It computes **nothing that affects the
> outcome** — no hitscan, no flag logic, no scoring.
>
> Its networking is two raw sockets: a TCP connection for reliable lobby/
> match events, a UDP socket for inputs and snapshots. To hide latency it
> runs three mechanisms in strictly separated scopes: it **predicts** its
> own movement by calling the exact same physics function the server uses,
> **reconciles** against each snapshot by snapping to authority and
> replaying unacknowledged inputs, and **interpolates** every other player
> between snapshots at a deliberate 66 ms delay. A headless **bot** reuses
> the identical networking code minus raylib, which is also our load
> generator.

---

## 2. Client architecture: four modules, one thread

```
                 ┌──────────────────────────────────────────────┐
   keyboard/mouse│  main.cpp        fixed 30 Hz tick + render    │
  ──────────────▶│    │                                         │
                 │    ▼            ┌────────────────┐           │
                 │  net_client ◀──▶│  SERVER         │          │
                 │  (the wire)     │  TCP + UDP      │          │
                 │    │            └────────────────┘           │
                 │    ├─▶ prediction.cpp   (me)                  │
                 │    ├─▶ interpolation.cpp (everyone else)      │
                 │    └─▶ render.cpp       (raylib, squares)     │
                 └──────────────────────────────────────────────┘
```

| File | Responsibility | Key entry points |
|---|---|---|
| `client/main.cpp` | wiring, input sampling, the game loop | `sample_input` (:79), main loop (:170) |
| `client/net_client.{h,cpp}` | sockets, handshake, codec dispatch | `connect_and_join` (:66), `poll` (:322) |
| `client/prediction.{h,cpp}` | my movement + reconciliation | `on_local_tick` (:24), `on_snapshot` (:37) |
| `client/interpolation.{h,cpp}` | everyone else's smooth motion | `advance` (:64), `sample` (:88) |
| `client/render.{h,cpp}` | raylib drawing + debug overlays | `draw_frame` |

Design rule worth stating: the networking code is **raylib-free**, so the
bot links the identical `net_client` + `prediction` objects. That's why
"the bot behaves exactly like a client" is true by construction, not by
imitation.

The whole client is **one thread**. It never blocks on the network:
`net.poll()` drains both sockets non-blocking every frame
(`net_client.cpp:322-327`). Contrast with the server's two threads — a
nice symmetry point if asked "why doesn't the client need threads?"

### The main loop shape (`client/main.cpp:170-248`)

```cpp
while (!g_stop && !renderer.should_close()) {
    accumulator += frame_dt;                       // :174

    net.poll();                                    // drain sockets :176
    for (snap : net.take_snapshots()) {            // :177-182
        interpolation.push_snapshot(snap);
        prediction.on_snapshot(snap, my_id);       // reconciliation lives here
    }
    for (ev : net.take_events()) renderer.on_event(ev);

    // fixed 30 Hz sim ticks, decoupled from render rate:
    while (accumulator >= tick_dt && ticks_this_frame < 10) {   // :193
        if (alive_in_match) prediction.on_local_tick(sample_input(...));
        send_current_input(net, prediction);       // ALWAYS sent :208
    }

    interpolation.advance(frame_dt);               // :212
    renderer.draw_frame(...);                      // variable rate
}
```

Two patterns to name-drop here:

- **Fixed timestep with an accumulator** (:193): simulation advances in
  exact 1/30 s steps regardless of render rate (60 fps, 144 fps, stutters)
  — prediction must consume inputs at *exactly* the rate the server
  consumes them. The 10-ticks-per-frame cap stops a window drag from
  spiraling into hundreds of catch-up ticks.
- **Three separate clocks**: simulation = fixed 30 Hz; rendering = GPU
  pace; interpolation clock = its own nudged rate (§9).

---

## 3. The client's two sockets

All inside `NetClient::connect_and_join`, `client/net_client.cpp:66-170`.

### TCP — blocking connect once, non-blocking service forever after

```cpp
getaddrinfo(host.c_str(), "7777", &hints, &res);        // :73-86
// name+port string -> sockaddr_in; resolved ONCE, reused for both sockets
tcp_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);             // :88
::connect(tcp_fd_, (sockaddr*)&server_addr, ...);        // :93
```

Blocking `connect()` is fine here — joining happens exactly once, before
any gameplay, and `wait_for_tcp_frame` (:172-215) bounds the wait with a
deadline loop over `poll()` + `recv()`, guarding against a dead server.

### UDP — bound to port 0, and that matters

```cpp
udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);              // :102
local.sin_port = 0;          // kernel assigns an ephemeral port :111
::bind(udp_fd_, (sockaddr*)&local, sizeof(local));       // :112
set_nonblocking(udp_fd_);                                // :117
```

Say this clearly if asked: **our UDP local port is kernel-assigned and has
nothing to do with our TCP port** — which is precisely why the handshake
needs step 4 (UDP_HELLO): the server cannot know where to send snapshots
until we speak first over UDP. Symmetrically the *server's* UDP port is
ephemeral too and reaches us inside JOIN_ACCEPT
(:147: `server_udp_port_ = accept.udp_port`). Nothing is hardcoded in
either direction.

After setup everything is non-blocking: reads return EAGAIN when drained
(`drain_tcp` :342, `drain_udp` :375); writes go through `send_all()`
(`shared/net_util.cpp:24`), which reports partial progress instead of
sleeping.

---

## 4. The handshake, implemented

Five steps, mapped 1:1 to code:

| Step | Message | Client code | Server counterpart |
|---|---|---|---|
| 1 | TCP connect | net_client.cpp:93 | accept (net_server.cpp:176) |
| 2 | JOIN_LOBBY `{name[16]}` | :119-127 | on_join_lobby |
| 3 | JOIN_ACCEPT `{id, token, udp_port}` | :137-153 | send (:348) |
| 4 | UDP_HELLO `{id, token}` over UDP | :232-250 | registers our address |
| 5 | START_REQUEST (host) → GAME_START | :285-292 · main.cpp:185-187 | begin_match_and_notify |

Details that earn marks:

- The name field is **fixed 16 bytes, NUL-padded** (:120-124). A length
  prefix would work too; fixed width keeps the payload trivially bounded.
- JOIN_ACCEPT stores the three things that define us on the wire:
  `player_id_`, `session_token_` (attached to every later UDP packet — the
  anti-spoofing token), and the server's UDP port (:145-151).
- **UDP_HELLO resend**: the hello rides UDP and can be lost, so `poll()`
  resends it every 200 ms until the first snapshot proves the server knows
  our address (`maybe_resend_udp_hello`, net_client.cpp:294-302; interval
  at `shared/game_config.h:143`; flag cleared on first snapshot at :479).
- Host status is **derived, not assumed**: `is_host()` compares our player
  id against LOBBY_STATE's host_id (`net_client.h:112-116`). The load
  generator originally got this wrong by racing bot-0 assumptions (§13) —
  a good war story.
- JOIN_REJECT carries a reason enum — Full / InProgress / BadVersion
  (`shared/protocol.h:72-76`) — exposed via `join_reject_reason()` so the
  UI can say *why* it was refused.
- The handshake state machine is an explicit enum
  (`NetClientState`, net_client.h:26-32): Disconnected → WaitingJoinAccept →
  InLobby → InGame, with Rejected as a terminal state. Tests assert on it.

---

## 5. Sending input: the 30 Hz client tick

Every fixed tick, `client/main.cpp:197-208`:

```cpp
if (alive_in_match) {
    const InputCmd cmd = sample_input(prediction.local_state().position);
    prediction.on_local_tick(cmd);      // predict NOW, don't wait RTT
}
send_current_input(net, prediction);    // ALWAYS — even while dead/lobby
```

**Input sampling** (`sample_input`, main.cpp:79-105): WASD/arrows into a
button bitmask (`UP=1 DOWN=2 LEFT=4 RIGHT=8 FIRE=16`), aim = `atan2` from
my predicted center to the mouse cursor, quantized to the wire's u16 angle
(`angle / 2π × 65536`). Raylib stays confined to this one function — the
bot substitutes its own synthetic sampler (§12).

**Packet building** (`send_current_input`, main.cpp:119-132 →
`NetClient::send_input`, net_client.cpp:252-283):

```
UDP header (8B) | player_id u8 | session_token u32 | base_seq u32
                | count u8     | count x { buttons u8, aim u16 }
```

- carries up to **3 inputs**: base_seq and the two before it — the input
  redundancy scheme. One lost datagram costs nothing because the next
  packet re-delivers its inputs (~6 extra bytes).
- seq numbers are strictly increasing; the server ignores anything ≤ its
  newest seen seq, so redundant copies are free.

**The idle-input subtlety** (this is §11's story — preview): input is sent
every tick even when dead or waiting in the lobby; only *prediction* is
gated on "alive in a live match". Sending is unconditional, content varies.

---

## 6. Receiving: draining both sockets

One non-blocking pump per frame — `NetClient::poll`
(net_client.cpp:322-327) does four things:

1. `drain_tcp()` (:329-365)
2. `drain_udp()` (:367-394)
3. `maybe_resend_udp_hello()` (:294)
4. `maybe_send_heartbeat()` (:304) — HEARTBEAT every 1 s

### TCP side (`drain_tcp`)

Same shape as the server's reader: loop `recv()` until EAGAIN appending to
`tcp_rx_buf_`, then repeatedly run the shared framer `recv_framed()`
(:353) dispatching each complete frame to `dispatch_tcp_frame` (:396).
That dispatcher is a decode-and-queue switch over all S→C event types
(LOBBY_STATE, GAME_START, PLAYER_KILLED, FLAG_*, MATCH_END…), each guarded
by `decode(...) && r.ok()` so malformed frames are dropped silently.
Events land in `pending_events_` as a `std::variant` queue
(`net_client.h:39-46`) — one drain point for render code.

### UDP side (`drain_udp`)

```cpp
const ssize_t n = ::recvfrom(udp_fd_, buf, sizeof(buf),
                             MSG_DONTWAIT, (sockaddr*)&from, &from_len);
if (n < kUdpHeaderBytes) continue;              // too short even for header
decode_udp_header(r, hdr) || !r.ok() -> drop    // magic/version gate
dispatch_udp_payload(hdr.type, hdr.tick, ...);  // :391
```

Note there's no per-peer check here — snapshots are accepted from whoever
holds the server's address; authenticity of *inputs* is what the server
enforces. The transport-header tick travels alongside the payload because
DELTA_SNAPSHOT bodies carry no tick field of their own
(`net_client.h:128-131`).

Ownership rule worth quoting (`net_client.h:7-12`): this layer does **not**
drop stale snapshots. Every successfully decoded snapshot is handed upward
in arrival order; the staleness decision (`snap.tick <= last_applied_tick`)
is made in exactly one place — reconciliation. One owner per invariant.

---

## 7. Delta snapshots on the client

`dispatch_udp_payload`, net_client.cpp:470-506:

- **Full WORLD_SNAPSHOT (type 9)**: decode → store as
  `last_snapshot_cache_` → queue for prediction/interpolation (:474-482).
- **DELTA_SNAPSHOT (type 19)** (:483-496):
  ```cpp
  if (!have_last_snapshot_) return;                 // nothing to diff against
  if (!protocol::decode_delta_snapshot(r, tick, last_snapshot_cache_)) {
      have_last_snapshot_ = false;                  // stale/malformed baseline
      return;                                       // stall till next keyframe
  }
  pending_snapshots_.push_back(last_snapshot_cache_);
  ```

The delta decodes **onto our cached copy**, which then surfaces as a
perfectly ordinary full snapshot — so prediction, reconciliation,
interpolation, rendering never learned deltas exist. Only the transport
changed.

Loss handling with zero ACKs: each delta names its baseline as
`baseline_ticks_ago`; if `cache.tick != hdr.tick − baseline_ticks_ago`,
a datagram was lost upstream → invalidate the cache and wait for the next
full keyframe (every 10th publish ≈ 333 ms worst case). The decoder works
on a copy and commits only on success, so garbage can never corrupt the
cache — fuzz-tested by truncating at every byte offset.

---

## 8. Prediction and reconciliation

`client/prediction.cpp` — 95 lines owning the hardest problem in the
project. Applies to **my player only**.

### Prediction (`on_local_tick`, :24-35)

```cpp
++seq_;
local_state_ = movement_step(local_state_, cmd, map_);   // INSTANT feedback
history_.push_back({seq_, cmd, state_after: local_state_});
```

We apply the input locally the same tick we sample it, using the very same
`shared/movement_step` the server runs — identical compiled physics, so
prediction matches authority *by construction*. There are not two physics
implementations that can drift apart. The stored `state_after` powers the
HUD misprediction counter only.

### Reconciliation (`on_snapshot`, :37-93)

Four steps, in order:

```cpp
if (snap.tick <= last_applied_tick_) return;     // 1. drop stale (UDP reorders)
// 2. locate my authoritative PlayerState + acked seq
history_.remove_if(e.seq <= ack);                // 3. discard acked history
local_state_.position = auth.position;           // 4a. snap to authority
for (e : history_)                                // 4b. replay unacked, ascending
    local_state_ = movement_step(local_state_, e.input, map_);
```

Points to defend:

- **Replay-always, no branch.** If prediction was correct, deterministic
  replay reproduces the same position and nothing visibly moves; skipping
  on "no misprediction" saves microseconds and creates a hiding place for
  bugs. The misprediction comparison exists purely as a counter (:66-74),
  never as control flow.
- **History trim matters** (:78-80): leaving acked entries in would
  re-apply ancient inputs during every replay — desync-checklist item 5.
- **Scope discipline**: prediction touches position/velocity through
  `movement_step` and nothing else. Shooting, pickups, deaths are never
  predicted — a wrongly predicted kill flash looks far worse than 30 ms of
  delay, and keeping outcomes server-side is also the anti-cheat story.
- History depth ≈ RTT × 30 = 2–4 entries on LAN; ring caps at 64
  (`shared/game_config.h:140`). The unacked count is literally RTT made
  visible (§10).

### What misprediction feels like

Wall collisions are the classic case: you predict a slide along a wall;
the server agrees exactly (same code!) — but if any constant ever diverged,
the ghost overlay (§10) shows the divergence growing tick by tick. That's
why determinism testing (10k inputs ×2 byte-identical) backs the design.

---

## 9. Interpolation

`client/interpolation.cpp` — applies to **every player except me**
(my player is predicted, never interpolated; no overlap).

### The mechanism

Keep a ring of the last ~30 snapshots (`kSnapshotRingSize`). Maintain a
fractional `render_tick` targeting `newest_tick − 2` — a deliberate
2-tick (≈66 ms) delay so there are always two snapshots to blend between.
Each frame, `sample()` (:88-130) finds the bracketing pair a/b and lerps:

```cpp
t = (render_tick − a.tick) / (b.tick − a.tick);   // clamped [0,1]
pos = lerp(a.pos, b.pos, t);
```

Three subtleties, each a potential exam question:

1. **Angle wraparound** (`lerp_angle`, :30-37): aim_angle is u16 over
   0..2π. Naive lerp of 65000 → 500 spins almost a full revolution; we
   interpolate the *signed* 16-bit delta (shortest arc).
2. **Clock drift** (`advance`, :64-86): our clock ≠ server clock, so the
   buffer depth drifts. Correction nudges the rate — 1.02× when too deep,
   0.98× when too shallow (:70-76) — never a jump; a hard correction is a
   visible pop.
3. **Stall behavior**: `advance` clamps `render_tick` to the newest tick
   (:83). On packet loss the lerp collapses to t=0 and simply holds the
   last known position instead of extrapolating — overshoot-then-snapback
   looks worse than a brief freeze.

Also note what is *not* interpolated: health/alive/team are discrete
(:120-123) — taken from whichever bracket is closer, so state changes show
immediately rather than fading.

Floats are fine here (`:8-14` header comment): interpolation output is
display-only and never feeds back into simulation or reconciliation, so
rounding can't accumulate anywhere. The "no floats" rule scopes only to
the deterministic movement path.

The trade to articulate: 66 ms of freshness for smoothness. Demo it:
`--snapshot-rate 10` on the server makes raw snapshots visibly chunky and
interpolation's value obvious.

---

## 10. HUD and debug tooling

Built early, doubling as evaluation methodology:

| Toggle | Effect | What it proves |
|---|---|---|
| F1 | prediction off | renders me at raw authority = what the network really feels like |
| F2 | interpolation off | remote players snap between snapshots |
| F3 | server ghost | authoritative position drawn beside my predicted one |

The ghost is the money shot: when reconciliation converges, the outline
sits exactly on your player and disappears; any desync becomes a visible,
growing gap with a direction and trigger.

HUD metrics (`client/main.cpp:221-239`):

- **RTT derived, not measured**: the protocol has no ping/pong; RTT comes
  from `unacked_inputs / 30 Hz` (:228-229) — "the unacked count IS the RTT
  made visible".
- misprediction rate = counter / total seqs (:230-234)
- snapshot buffer depth (:235), actual measured tick rate (:214-219)

These turn defense claims into live numbers on screen.

---

## 11. A subtle bug worth telling as a story: the liveness trap

Documented in the file-level comment at `client/main.cpp:6-28`. Two server
constants coincide:

- UDP-silence disconnect threshold: **3 s**
- respawn timer: 90 ticks @ 30 Hz = **exactly 3.0 s**

Naive design: send PLAYER_INPUT only while alive and predicting. Then a
dead player goes silent for up to 3 s — landing exactly on the disconnect
threshold right as they respawn. Same hazard in the lobby: once the first
snapshot stops the UDP_HELLO resends, nothing refreshes the server's
liveness clock until input starts flowing.

Fix: the 30 Hz tick — and therefore one PLAYER_INPUT per tick — runs
continuously through the whole session; only `on_local_tick` (movement)
is gated on alive-in-match (:197-206). This is safe because the server's
`on_player_input` refreshes its liveness timestamp unconditionally, even
for duplicate seqs. Why this story scores: it shows you understood the
*liveness protocol across both codebases*, not just your half.

Related shutdown detail: clean exit just closes the TCP socket
(main.cpp:251) — the server's `recv()==0` definitive-detector does the
rest within one poll cycle.

---

## 12. The bot: the same wire code, headless

`bot/` links the identical `client_core` library (`net_client`,
`prediction`) without raylib — same handshake, same input packets, same
reconciliation. It differs in exactly two places:

1. **Input source**: synthetic `BotAI::tick()` (`bot/bot_ai.cpp:88`)
   instead of keyboard sampling.
2. **Navigation**: BFS **flow fields** (`bot/flow_field.cpp:43`) replacing
   the old memoryless greedy steering that oscillated dead against wall
   gaps. Descent is strictly downhill in precomputed distance + per-axis
   physical feasibility probes + corridor re-centering — walking downhill
   cannot oscillate or dead-end. Target selection (`bot_ai.h:33`):
   carry→home; enemy has our flag→hunt carrier; else→enemy base.

Wiring mirrors the client exactly: `connect_and_join` (bot/main.cpp:112),
snapshot pump feeding `prediction.on_snapshot` (:150), BotAI constructed
once GAME_START reveals our team (:130-158). Bots exist for two reasons:
testing the game solo, and being the load generator behind every benchmark
number we claim.

Known limitation to concede gracefully: the wire carries flag *state* +
carrier id but not dropped-flag position, so "chase our dropped flag"
is not derivable from snapshots — bots push the enemy base instead and
rely on the 450-tick auto-return (`docs/optimization.md` §3).

---

## 13. Tooling: loadgen.py, netem.sh, benchmarks

| Tool | What it does |
|---|---|
| `tools/loadgen.py` | Independent **Python** headless client: full v2 protocol incl. delta decode; counts keyframes/deltas/bytes; drives n=10-bot benchmarks |
| `tools/netem.sh` | `tc netem` wrapper — delay/jitter/loss injected on loopback to test degradation |
| `tools/bench_bandwidth.sh` | full-vs-delta bandwidth measurement → docs/benchmark_snapshots.md (−46%) |
| `tools/bench_pollers.sh` | poll-vs-epoll benchmark → docs/benchmark.md |

Why a Python client matters (say this): it implements the protocol purely
from the documentation — proving the spec is complete and unambiguous,
independent of our C++ headers. Any third party could join the game from
the docs alone.

Bugs the tooling caught (war stories with substance):

1. **Host race**: loadgen assumed bot 0 wins "first joiner is host". Under
   concurrent connects it sometimes doesn't → START_REQUEST rejected →
   match silently never starts → benchmark measures an empty world. Fixed:
   every client parses LOBBY_STATE; whoever actually holds host_id starts.
2. **TCP stream desync**: bytes arriving in the same recv() as JOIN_ACCEPT
   were discarded, permanently misaligning frame parsing. Leftovers now
   stay in the reassembly buffer — a textbook framing bug.
3. **Unrealistic load**: constant motion + spinning aim was the pathological
   worst case for deltas. Bots now move intermittently (~50% duty cycle)
   with frozen aim while idle, matching real play — making the −46%
   bandwidth number honest.

---

## 14. Decision table (client edition)

| Question | Answer | Where |
|---|---|---|
| Why predict my own movement? | Input feels instant; replay corrects when wrong | §8 |
| Why the *same* movement function as the server? | Identical compiled physics ⇒ prediction matches by construction, not tuning | §8 |
| Why replay-always? | Correct replay is a visual no-op; branching hides bugs | §8 |
| Why never predict kills/pickups? | Wrongly shown outcomes must be yanked back — worse than delay; also anti-cheat | §8 |
| Why interpolate others at −2 ticks (66 ms)? | Smoothness for staleness; demoable via `--snapshot-rate 10` | §9 |
| Why hold position on stall, not extrapolate? | Overshoot-then-snapback looks worse than freeze | §9 |
| Why nudge interpolation rate instead of jumping? | Hard corrections are visible pops | §9 |
| Why shortest-arc angle lerp? | Naive 65000→500 spins nearly full circle | §9 |
| Why floats allowed in interpolation but not movement? | Display-only output can't accumulate anywhere | §9 |
| Why fixed-timestep accumulator on the client? | Prediction must consume inputs at server's exact rate | §2 |
| Why one thread on the client vs two on the server? | Client has no shared mutable state to protect; poll-drain suffices | §2 |
| Why send input even while dead/in lobby? | Server's silence timeout coincides with respawn timer — §11's story | §11 |
| Why resend UDP_HELLO every 200 ms? | Hello itself rides lossy UDP; first snapshot proves delivery | §4 |
| Why derive host from LOBBY_STATE? | "First joiner" is racy under concurrent connects — loadgen bug proved it | §13 |
| Why keep stale-drop out of net_client? | One owner per invariant: reconciliation makes the call | §6 |
| Why a headless bot sharing client code? | Same wire path ⇒ honest load tests + solo playability | §12 |
| Why flow fields for bots? | Greedy steering oscillates at wall gaps; downhill descent cannot dead-end | §12 |
| Why an independent Python client? | Proves the documented spec is implementable without our headers | §13 |

---

## 15. Likely defense questions with model answers (client)

**Q1. Trace one keypress end-to-end from your side.**
Frame: `net.poll()` drains sockets → snapshot loop runs reconciliation →
fixed tick fires: `sample_input` builds `{buttons, aim}` →
`prediction.on_local_tick` applies `movement_step` locally and pushes
history → last 3 inputs serialized into PLAYER_INPUT with token+seq over
UDP. ~RTT/2 later the snapshot returns with my ack; I trim history, snap,
replay unacked inputs; meanwhile everyone else saw me move via their
interpolation.

**Q2. What exactly happens when a snapshot arrives out of order?**
`dispatch_udp_payload` queues both; reconciliation drops the older tick
(`snap.tick <= last_applied_tick`) — the single staleness check in the
codebase. Interpolation separately refuses non-ascending ticks into its
ring (`interpolation.cpp:49-51`). Each mechanism guards its own scope.

**Q3. How do you know your prediction is correct?**
Three ways: (1) F3 ghost overlay sits exactly on the player when
converged; (2) HUD misprediction counter ≈ 0 on loopback; (3) the shared
`movement_step` determinism test proves identical physics. Under netem
delay the ghost separates then re-converges after each snapshot — visible
proof of the correction cycle.

**Q4. What if two snapshots arrive in one frame?**
Both are processed: each reconciles me (the second supersedes), both enter
the interpolation ring. The fixed-tick loop just consumes them in order.

**Q5. Why does the client bind UDP to port 0?**
We don't care which local port we get — we announce it implicitly by
sending UDP_HELLO from it. Choosing a fixed port would collide when two
clients run on one machine.

**Q6. Your aim uses floats (atan2). Isn't that a desync risk?**
No: aim_angle is quantized to u16 *before* it enters any packet or
movement math; the float exists only inside input sampling on my machine.
The desync-critical rule bans floats in `movement_step`, not at the UI
boundary.

**Q7. How does the client survive a lost JOIN_ACCEPT?**
It doesn't need to — TCP delivered it or the connect times out cleanly.
Lost UDP_HELLO is the real case: the 200 ms resend loop covers it until
the first snapshot confirms registration.

**Q8. What happens if the server dies mid-match?**
TCP `recv()` returns 0 in `drain_tcp` → `disconnect()` closes both fds →
state Disconnected → main loop exits via renderer shutdown. No half-dead
zombie state.

**Q9. Explain the delta snapshot stall. Worst case?**
A delta applies onto the cached previous snapshot. If that baseline was
lost, `decode_delta_snapshot` fails the baseline check, we invalidate the
cache and render frozen positions until the next keyframe — every 10th
publish = ≤333 ms at 30 Hz. We chose this over NACKs: no new protocol
states, bounded staleness, and bandwidth still fell 46%.

**Q10. Why is your RTT estimate derived rather than measured?**
The protocol has no ping/pong — adding one costs messages and code for a
number already encoded for free: unacked history depth × 33.3 ms
(`client/main.cpp:228-229`). That value is the age of my oldest
unacknowledged input ≈ network RTT + server queueing — exactly the latency
the player actually feels, so it's the right thing to display.

**Q11. How do you test against bad networks?**
`tools/netem.sh` injects delay/jitter/loss on loopback (`tc netem`).
Expectations written down: inputs survive single-packet loss via
redundancy; reconciliation converges after bursts; interpolation holds on
snapshot gaps; TCP events arrive regardless.

**Q12. Why did bots get stuck before, and how do flow fields fix it?**
Old greedy steering chose the best single step per tick — near wall gaps
"step away" beat "step back", so the bot oscillated forever. Flow fields
precompute BFS distance-to-target per tile; moves must strictly descend
and pass an AABB feasibility probe; corridor re-centering stops corner
clipping. Strict descent provably reaches the source — tested from every
spawn point through real collision code.

**Q13. What would you add next on the client side?**
Lag compensation is server-side, but its client half is hit feedback;
client-side prediction of tracers already exists cosmetically. Real list:
mid-match join state seeding, snapshot interest filtering beyond change
masks, and connection migration (re-hello on NAT rebinding is manual today).

**Q14. Why is the client "dumb terminal" framing important?**
Because every gameplay computation living server-side is simultaneously
the cheat-prevention story, the consistency story (one authority, no
distributed simulation), and the simplicity story (client crash ≠ game
corruption). It's the project's central architectural claim.

**Q15. What was the hardest client bug?**
Best candidates: (a) the liveness trap (§11) — cross-layer timing
coincidence; (b) loadgen's post-JOIN_ACCEPT byte discard silently
desyncing all later TCP frames — found only when the benchmark harness
started measuring an empty world; (c) shot tracers offset because they
were drawn from box corner while aiming used box center — fixed jointly
with the server's ray origin.

---

## 16. Numbers to memorize (client)

| Quantity | Value |
|---|---|
| Client sim tick | 30 Hz, accumulator-capped at 10 ticks/frame |
| Inputs per packet | 3 (~27 B payload + 8 B header) |
| UDP_HELLO resend | every 200 ms until first snapshot |
| HEARTBEAT | every 1 s (courtesy; consumed as no-op) |
| Interpolation delay | 2 ticks ≈ 66 ms; ring = 30 snapshots |
| Drift nudge | ×1.02 / ×0.98 |
| Input history ring | 64 cap; typical depth = RTT×30 ≈ 2–4 |
| Delta stall worst case | ≤333 ms (keyframe every 10th publish) |
| Bandwidth delta vs full | 1.85 vs 3.42 KiB/s/client (−46%) |
| Handshake timeout | 5 s default |
| Test suite | 141 cases / 18,444 assertions incl. bot-AI navigation |

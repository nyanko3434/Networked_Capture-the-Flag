# optimization.md — Network Optimizations

What was optimized, why, how it works, and what it cost. Companion to the
measured numbers in `docs/benchmark_snapshots.md` (bandwidth) and
`docs/benchmark.md` (poll vs epoll). Design context lives in `README.md`
§5.5; this document is the optimization-specific summary.

---

## 1. Delta-compressed snapshots

### The problem

Before this work, every client received a full `WORLD_SNAPSHOT` (~128-byte
body for 10 players) at up to 30 Hz, even though in real play most fields are
identical between consecutive snapshots: players stand still, aim angles
don't move, scores don't change. At n=10 that is a fixed ~3.9 KB/s of UDP
per client regardless of what actually happened.

### The change

A new message, `DELTA_SNAPSHOT` (type **19**), carries field-level diffs
against the *previous published snapshot* instead of the whole world:

```
u32 last_input_seq      <- offset 0, patched per recipient (same as full)
u8  baseline_ticks_ago  <- baseline tick = header tick - this
u8  header_mask         <- which snapshot-header fields changed
[changed header fields]
u16 player_change_mask  <- bit i = player slot i changed
per changed player:
  u8 id, u8 field_mask (pos/aim/health/flags), [only the set fields]
```

- A **full `WORLD_SNAPSHOT` keyframe** is published every 10th snapshot
  (`config::kSnapshotKeyframeInterval`) — the wire format is unchanged, so
  all pre-existing tests keep passing.
- Server flag `--snapshots delta|full`, default `delta`. `full` restores the
  legacy behavior byte-for-byte.
- Positions compare at **wire granularity** (i16, 1/16 px) so the encoder
  never emits a "change" the decoder cannot represent.
- An idle player costs literally nothing in the delta; an idle *world*
  produces a minimal ~8-byte payload.

### Loss handling without ACKs (the interesting part)

UDP datagrams get lost, and a delta is useless without its baseline. Instead
of adding a NACK channel (more protocol, more states):

1. Every delta names its baseline as `baseline_ticks_ago`.
2. The client caches the last applied snapshot; if
   `cache.tick != header_tick - baseline_ticks_ago`, a datagram went missing
   upstream → the delta is dropped and the cache invalidated.
3. Updates stall until the next keyframe — at worst
   `kSnapshotKeyframeInterval` snapshot intervals (**~333 ms** at 30 Hz).

The decoder validates everything (mask bits, flag-state enums, counts) onto
a copy and commits only on success, so a malformed or stale packet can never
corrupt the cache. Malformed-packet fuzz tests cover truncation at every
length.

### Protocol version bump (1 → 2)

A stale v1 binary receiving type 19 would mis-decode it silently. Bumping
`kProtocolVersion` makes the handshake reject old binaries cleanly with
`JOIN_REJECT BadVersion` instead.

### Measured result (`tools/bench_bandwidth.sh`, n=10 bots, loopback)

| mode | UDP KiB/s/client |
|---|---|
| full | 3.42 |
| delta | **1.85 (−46%)** |

The win scales with idle time: with everyone moving every tick (pathological
worst case) deltas shrink it only ~15%; a camping-heavy game saves more than
half. Keyframe cadence bounds the worst-case staleness under loss.

### Files touched

| Layer | File | Change |
|---|---|---|
| shared codec | `shared/protocol.{h,cpp}` | type 19, encode/decode pair, version 2 |
| constants | `shared/game_config.h` | `kSnapshotKeyframeInterval = 10` |
| thread handoff | `server/queues.h` | `OutboundEventType::UdpDeltaSnapshot` |
| sim | `server/sim.{h,cpp}` | baseline cache + keyframe cadence in `publish()` |
| send path | `server/broadcast.{h,cpp}` | transport-header type-byte parameter |
| routing | `server/net_server.{h,cpp}` | delta event routing + `udp_delta_snapshots_sent()` counter |
| CLI | `server/main.cpp` | `--snapshots delta|full` |
| client | `client/net_client.{h,cpp}` | decode delta onto cache; surfaces as normal `WorldSnapshot` |
| tooling | `tools/loadgen.py`, `tools/bench_bandwidth.sh` | v2 protocol, measurement |

Client-side prediction, reconciliation, interpolation, and rendering are
**untouched**: they still consume full `WorldSnapshot`s; only the transport
changed.

---

## 2. Interest management (folded into #1)

The original plan listed relevance filtering as a separate feature. It
turned out to be unnecessary as a mechanism: the per-player change mask
*is* interest management. A player who did nothing relevant this tick has no
bits set and contributes zero bytes; dead/idle players drop out of deltas
automatically while TCP events (`PLAYER_KILLED`, `FLAG_RETURNED`, …)
continue to carry their state transitions reliably. No distance-based
culling was added — on a 1280×800 map everyone is relevant anyway, and the
change-mask approach gets the bandwidth win without any risk of missing
state.

---

## 3. Measurement harness + load-generator fixes

New `tools/bench_bandwidth.sh` (modeled on `bench_pollers.sh`) records
full-vs-delta numbers into `docs/benchmark_snapshots.md`. Writing it
exposed three latent bugs in `tools/loadgen.py`, all fixed:

1. **Host race:** the generator assumed bot 0 wins the "first joiner is
   host" race. Under concurrent connects it sometimes doesn't — its
   `START_REQUEST` is then rejected and the match silently never starts,
   making benchmarks measure an empty world. Now every client parses
   `LOBBY_STATE`, and whichever client actually holds `host_id` sends
   `START_REQUEST` once the roster is complete.
2. **TCP stream desync:** bytes arriving in the same `recv()` as
   `JOIN_ACCEPT` were discarded, permanently misaligning frame parsing when
   lobby broadcasts interleaved with the handshake. Leftover bytes now stay
   in the reassembly buffer.
3. **Unrealistic load:** bots used to hold movement keys and spin their aim
   every tick — the pathological worst case for any delta scheme. They now
   move intermittently (~50% duty cycle, per-bot phase) with frozen aim
   while idle, which matches real play and makes the benchmark honest.

Also fixed en route (found by ASan): the 64KB pending-cap test claimed
`payload_size=1024` on the 512-byte `OutboundEvent` array — a test-side
buffer overread, not a server bug.

---

## Verification

- **135 test cases / 16,411 assertions green**, normal build and ASan build.
- New tests: delta round-trips, empty-delta minimality, patch-offset
  preservation, stale-baseline rejection, malformed/truncated fuzzing,
  keyframe cadence, full-mode purity, end-to-end loss-and-recovery over
  real sockets.
- Live loopback smoke tests pass in delta mode, full mode, and with both
  poller backends; SIGINT shutdown stays clean.

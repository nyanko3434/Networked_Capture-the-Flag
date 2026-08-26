# HANDOFF.md — Client-side integration package for Nayan

The server (`ctf_server`) is **complete, unit-tested, and live-verified**.
Everything below tells you how to build the other end of the wire. Nothing
on the server will change while you work — `shared/` header APIs are frozen.

---

## 1. Server quick-start

```bash
git checkout main && git pull
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCTF_BUILD_CLIENT=OFF
cmake --build build -j$(nproc)
./build/ctf_server --port 7777 --tick 30 [--poller poll|epoll]
```

- Startup prints: `listening tcp=7777 udp=NNNNN ...` — **the UDP port is
  ephemeral**; your client learns it from JOIN_ACCEPT's `udp_port` field,
  never hardcode it.
- `--snapshot-rate 10` decimates UDP snapshots for demos (sim keeps ticking).
- SIGINT shuts down cleanly (exit 0).
- Run the test suite anytime: `./build/tests/ctf_tests` (91 cases green).

---

## 2. Byte-level protocol cheat sheet

All integers are **big-endian** (network order). No struct memcpy anywhere —
field-by-field only. Reference encoder: `shared/protocol.cpp`; working Python
client: `tools/loadgen.py`.

### Transports

```
TCP frame:    [u16 payload_len][u8 type][payload...]     (len EXCLUDES header)
UDP datagram: [u16 magic=0x4346][u8 version=2][u8 type][u32 tick][payload...]
              (8-byte header on EVERY UDP packet; wrong magic/version is
               dropped server-side without any response)
```

### Message types (u8)

| id | Name | Transport | Direction |
|----|------|-----------|-----------|
| 1 | JOIN_LOBBY | TCP | C→S |
| 2 | JOIN_ACCEPT | TCP | S→C |
| 3 | JOIN_REJECT | TCP | S→C |
| 4 | LOBBY_STATE | TCP | S→all |
| 5 | START_REQUEST | TCP | C→S (host only) |
| 6 | GAME_START | TCP | S→all |
| 7 | UDP_HELLO | UDP | C→S |
| 8 | PLAYER_INPUT | UDP | C→S |
| 9 | WORLD_SNAPSHOT | UDP | S→all |
| 10 | SHOT_FIRED | UDP | S→all (cosmetic) |
| 11–17 | PLAYER_KILLED, PLAYER_RESPAWNED, FLAG_PICKED_UP, FLAG_DROPPED, FLAG_RETURNED, FLAG_CAPTURED, MATCH_END | TCP | S→all |
| 18 | HEARTBEAT | TCP | C→S every 1s |
| 19 | DELTA_SNAPSHOT | UDP | S→all (delta vs previous publish; protocol version 2) |

### Payload layouts (in wire order)

```
JOIN_LOBBY      name[16]                                  (fixed 16 bytes, NUL-padded)
JOIN_ACCEPT     u8 player_id, u32 session_token, u16 udp_port   (= server UDP port)
JOIN_REJECT     u8 reason                                 (0=Full, 1=InProgress)
LOBBY_STATE     u8 player_count, then per player:
                  u8 id, char name[16]
                u8 host_id
START_REQUEST   (empty)
GAME_START      u8 player_count, u32 start_tick, then per player:
                  u8 id, u8 team(0=red,1=blue), i16 spawn_x, i16 spawn_y
UDP_HELLO       u8 player_id, u32 session_token
PLAYER_INPUT    u8 player_id, u32 session_token, u32 base_seq, u8 count,
                count x { u8 buttons, u16 aim_angle }
                count <= 3; seqs are base_seq-2 .. base_seq
WORLD_SNAPSHOT  u32 last_input_seq          <- PATCHED PER RECIPIENT (offset 0!)
                u32 tick                    <- repeated here AND in udp hdr
                u8 player_count
                u8 flag_carrier_red, flag_carrier_blue        (0xFF = none)
                u8 flag_state_red, flag_state_blue            (0=base,1=carried,2=dropped)
                u8 score_red, score_blue
                u16 seconds_remaining
                per player (10 bytes):
                  u8 id, i16 x, i16 y, u16 aim_angle, u8 health, u8 flags
                  flags bits: 1=alive, 2=carrying_flag, 4=team_blue, 8=firing
SHOT_FIRED      u8 shooter_id, i16 ox, i16 oy, u16 angle, i16 hx, i16 hy
PLAYER_KILLED   u8 victim_id, u8 killer_id, u32 tick
PLAYER_RESPAWNED u8 player_id, i16 x, i16 y, u32 tick
FLAG_PICKED_UP  u8 flag_team, u8 player_id, u32 tick
FLAG_DROPPED    u8 flag_team, i16 x, i16 y, u32 tick
FLAG_RETURNED   u8 flag_team, u8 player_id, u32 tick
FLAG_CAPTURED   u8 flag_team, u8 player_id, u32 tick
MATCH_END       u8 winning_team, u8 score_red, u8 score_blue
HEARTBEAT       (empty)
DELTA_SNAPSHOT  u32 last_input_seq          <- PATCHED PER RECIPIENT (offset 0!)
                u8  baseline_ticks_ago      <- baseline = hdr tick - this
                u8  header_mask             bit0 score_red, bit1 score_blue,
                                            bit2 seconds_remaining,
                                            bit3 flag red (state+carrier),
                                            bit4 flag blue, bit5 player_count
                [changed header fields in bit order]
                u16 player_change_mask      bit i = player slot i changed
                per changed player:
                  u8 id
                  u8 field_mask             bit0 pos(2xi16), bit1 aim(u16),
                                            bit2 health(u8), bit3 flags(u8)
                  [only the set fields]
```

### Delta snapshot rules (protocol version 2)

- The client caches the last applied snapshot; each DELTA_SNAPSHOT is decoded
  onto that cache and then surfaces as a normal full `WorldSnapshot` —
  prediction/interpolation code does not change.
- Baseline mismatch (a lost datagram upstream) → the delta is dropped; updates
  stall until the next full WORLD_SNAPSHOT keyframe (every 10th publish,
  ~333 ms at snapshot-rate 30). No client action needed.
- Reference decode: `client/net_client.cpp` (`dispatch_udp_payload`);
  codec: `shared/protocol.cpp` (`encode/decode_delta_snapshot`).

### Positions: fixed-point scales

| Domain | Type | Units | Conversion |
|---|---|---|---|
| Internal (movement math) | int32 | 1/256 px | — |
| Wire | int16 | 1/16 px | `wire = internal >> 4`, `internal = wire << 4` |

Never use floats in the movement path — you link the same
`shared/movement.cpp` the server uses, so prediction matches authority by
construction.

### Input buttons bitmask

`UP=1, DOWN=2, LEFT=4, RIGHT=8, FIRE=16`. `aim_angle`: u16 covering 0..2π
(`angle_rad * 65536 / (2π)`).

---

## 3. Handshake walkthrough

```
1. TCP connect to server :port
2. C->S  JOIN_LOBBY {name}                      -> S->C  JOIN_ACCEPT {id, token, udp_port}
                                                 or JOIN_REJECT {reason}
3. C->S  UDP_HELLO over UDP to udp_port         (resend every 200ms until first
                                                 snapshot arrives — hello can be lost)
4. S->all LOBBY_STATE                            (broadcast on every roster change)
5. First joiner is HOST. Host sends:
   C->S  START_REQUEST                          -> S->all GAME_START {teams, spawns}
```

Worked example (matches `tools/loadgen.py` exactly):

```
JOIN_LOBBY "smoke":
  00 10 01 | 73 6d 6f 6b 65 00 00 00 00 00 00 00 00 00 00 00
  len=16 type=1  name="smoke"+NULs

JOIN_ACCEPT reply:
  00 07 02 | 00 e9 2b 5a 77 45 c3
  len=7 type=2  player_id=0x00 token=0xe92b5a77 udp_port=0x45c3(17859)

UDP_HELLO (datagram to udp_port):
  43 46 01 07 | 00 00 00 00 | 00 | e9 2b 5a 77
  magic version type=tick0..   id   token

PLAYER_INPUT (seq 42, holding RIGHT+FIRE, aim ~90deg right-ish):
  43 46 01 08 | <tick u32> | 00 | e9 2b 5a 77 | 00 00 00 2a | 03
  | 08 80 00 | 00 7d 00 | 00 00 00
  header | id token base_seq=42 count=3 | three {buttons,aim} entries
```

---

## 4. Known server behaviors (don't fight these)

- **Session token on every PLAYER_INPUT** — also source-address checked;
  if your UDP socket rebinds, re-send UDP_HELLO.
- **Stale/duplicate input seqs are silently ignored** — redundancy re-sends
  are free; just keep seq strictly increasing.
- **No mid-match joining** — rejected with reason `InProgress`. Roster
  survives MATCH_END; a new START_REQUEST restarts immediately.
- **Full roster = 10**; duplicates of an already-connected id are rejected.
- **UDP silence timeout is 3 s from join** — keep sending inputs or you get
  disconnected even with TCP open.
- **Pending TCP output > 64 KB disconnects you** — read your TCP socket.
- **HEARTBEAT is consumed as a no-op** — send it anyway (README §5.4), but
  liveness is actually driven by UDP input + TCP state.
- **Capture happens at YOUR OWN base**, with your own flag at home.
- Respawn: 90 ticks after death at your spawn point, full health.
- Flag auto-return: 450 ticks untouched. One input popped per tick; empty
  buffer = zero movement that tick (your ack stays put — expect it).
- Movement: no acceleration; diagonal normalized ×181/256; collision
  X-then-Y against a 24×24 box — all inside `shared/movement_step`, which
  you call directly for prediction.

---

## 5. Reference material in this repo

| File | Use it for |
|---|---|
| `tools/loadgen.py` | Working headless Python client — diff your bytes against it |
| `tests/test_net_server.cpp` | Exact frames the server expects/emits, incl. edge cases |
| `shared/protocol.h/.cpp` | Frozen API + reference encode/decode |
| `tests/test_protocol.cpp` | Round-trip examples for every message |
| `implementation_guide.md` §3 | Your full task checklist (§3.1–§3.6) |

---

## 6. Joint integration acceptance checklist

Run these together once your client works, in order:

- [ ] **H1** Headless client connects to live server → logs all handshake
      steps → reaches GAME_START
- [ ] **M1** Hold RIGHT: snapshot shows your player advancing ~1365 fp/tick
      (~160 px/s); release stops instantly
- [ ] **M2** Two clients: both see each other move; one fires 3 hits into
      the other → PLAYER_KILLED event, victim respawns after ~3 s
- [ ] **F1** Red steals blue flag, carries it home with red flag at base →
      FLAG_CAPTURED, score increments, MATCH_END at 3, rematch starts
      without reconnecting
- [ ] **D1** Ctrl-C one client → other client sees LOBBY_STATE update within
      ~100 ms; server log stays clean
- [ ] **S1** `--snapshot-rate 10` demo: game still ticks 30 Hz, snapshots
      visibly chunky → interpolation's value becomes obvious
- [ ] **P1** (after prediction lands) F3 ghost sits exactly on your player
      on loopback; separates under netem delay

---

## 7. Suggested build order

1. **Headless bot first** (`bot/main.cpp`): handshake + input sender +
   snapshot logger. H1/M1 pass within a day — proves the whole wire.
2. **Renderer second**: link `client_core` (net_client/prediction-free) +
   raylib, draw map tiles / players / flags straight from snapshots.
   Playable-but-laggy milestone.
3. **Prediction + reconciliation third** (README §6.2–6.3): replay-always,
   history trim on ack, mispredictions as HUD counter only.
4. **Interpolation last** (§6.4): ring of 30, render_tick − 2, shortest-arc
   angles, drift nudge 1.02×/0.98×.
5. Debug toggles F1/F2/F3 + HUD early — they are also the report's
   evaluation screenshots.

You need `libxrandr-dev` (and friends) for raylib to configure; server-only
machines skip all of it with `-DCTF_BUILD_CLIENT=OFF`.

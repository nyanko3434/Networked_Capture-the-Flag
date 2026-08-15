#pragma once

// Single source of truth for every tuning constant in the project (README §4,
// structural rule 2). Nothing outside this file may redefine a constant found
// here — a movement constant duplicated in a client file causes silent
// prediction desync that takes days to find.

#include <cstddef>
#include <cstdint>

namespace ctf::config {

// ---------------------------------------------------------------------------
// SHARED — desync-critical (README §8: exactly seven entries).
//
// These constants feed shared/movement.cpp, which client and server both
// compile and run identically. Any difference between the two binaries —
// including this header drifting out of sync — breaks reconciliation.
// ---------------------------------------------------------------------------

// Tick rate: 30 Hz.
constexpr int32_t kTickRateHz = 30;
constexpr int64_t kTickDurationNs = 1'000'000'000LL / kTickRateHz;

// Tile size: 32 px.
constexpr int32_t kTileSizePx = 32;

// Player size: 24 x 24 px (AABB collider, README §7.2).
constexpr int32_t kPlayerSizePx = 24;

// Move speed: 160 px/s, expressed in fixed-point units per tick (1365).
constexpr int32_t kMoveSpeedFpPerTick = 1365;

// Diagonal factor: 181/256 (~0.707), applied when both axes are non-zero so
// diagonal movement is not 41% faster than axis movement (README §7.2).
constexpr int32_t kDiagonalFactorNum = 181;
constexpr int32_t kDiagonalFactorDen = 256;

// Max health: 100.
constexpr uint8_t kMaxHealth = 100;

// Input redundancy: 3 inputs packed into every PLAYER_INPUT packet.
constexpr int32_t kInputRedundancy = 3;

// Fixed-point scale implied by §5.1: internal positions are int32, in
// 1/256 px units. Desync-critical — it is baked into every movement result.
constexpr int32_t kFixedShift = 8;
constexpr int32_t kFixedScale = 1 << kFixedShift; // 256

// ---------------------------------------------------------------------------
// SHARED — structural / protocol.
//
// Not itemized in the §8 table, but required by shared/map.h and
// shared/net_util.h, which are compiled once into `shared` and linked into
// all three binaries. Consistency here comes from single-sourcing the code,
// not from discipline, so these are lower-risk than the desync-critical
// group above — but they still must never be redefined elsewhere.
// ---------------------------------------------------------------------------

// Map: 40 x 25 tiles at 32 px = 1280 x 800 (README §7.1).
constexpr int32_t kMapWidthTiles = 40;
constexpr int32_t kMapHeightTiles = 25;
constexpr int32_t kMapWidthPx = kMapWidthTiles * kTileSizePx;   // 1280
constexpr int32_t kMapHeightPx = kMapHeightTiles * kTileSizePx; // 800

// Player count: 2-10 players (README §1).
constexpr int32_t kMaxPlayers = 10;

// Wire position scale: on the wire, positions are int16, 1/16 px units
// (README §5.1). Both encode (server) and decode (client) paths must agree.
constexpr int32_t kWireFixedShift = 4;
constexpr int32_t kWireFixedScale = 1 << kWireFixedShift; // 16

// TCP framing: [u16 payload_len][u8 type][payload...] (README §5.3). The
// framing/reassembly code lives in shared/net_util, used by both sides.
constexpr size_t kTcpFrameHeaderBytes = 3;
constexpr size_t kTcpFrameCapBytes = 4096;

// UDP header (README §5.3): magic 0x4346, version, type, tick.
constexpr size_t kUdpHeaderBytes = 8;

// ---------------------------------------------------------------------------
// SERVER-ONLY (README §8: six table entries, plus additional constants
// needed by server/ code). Safe to differ from the client — the client never
// computes damage, timers, or scoring.
// ---------------------------------------------------------------------------

// Damage per shot: 34 (3 shots to kill).
constexpr uint8_t kDamagePerShot = 34;

// Fire cooldown: 10 ticks.
constexpr int32_t kFireCooldownTicks = 10;

// Respawn delay: 90 ticks (3 s).
constexpr int32_t kRespawnDelayTicks = 90;

// Flag auto-return: 450 ticks (15 s).
constexpr int32_t kFlagAutoReturnTicks = 450;

// Score to win: 3 captures.
constexpr int32_t kScoreToWin = 3;

// Match time limit: 10 minutes.
constexpr int32_t kMatchTimeLimitSec = 10 * 60;

// Server input ring buffer: exactly 8 entries per player (README §6.1). The
// simulation pops exactly one input per tick, no more, no less.
constexpr int32_t kInputRingSize = 8;

// UDP silence timeout: 3 seconds of no input packets is a disconnect
// (README §5.7).
constexpr int32_t kUdpSilenceTimeoutSec = 3;

// Pending TCP output buffer cap per client: 64 KB, beyond which the client is
// disconnected rather than let one slow client buffer unbounded (README §3.2).
constexpr size_t kTcpPendingBufferCapBytes = 64 * 1024;

// Heartbeat: client sends HEARTBEAT over TCP every 1 s (README §5.4).
constexpr int32_t kHeartbeatIntervalSec = 1;

// ---------------------------------------------------------------------------
// CLIENT-ONLY (README §8: one table entry, plus additional constants needed
// by client/ code).
// ---------------------------------------------------------------------------

// Interpolation delay: 2 ticks.
constexpr int32_t kInterpolationDelayTicks = 2;

// Snapshot ring: last ~30 snapshots kept for interpolation (README §6.4).
constexpr int32_t kSnapshotRingSize = 30;

// Input history ring: bounds unacked prediction history. RTT * 30 is 2-4
// entries on a LAN; 64 is ample headroom (README §6.3).
constexpr int32_t kInputHistoryRingSize = 64;

// UDP_HELLO resend interval until the first snapshot arrives (README §5.6).
constexpr int32_t kUdpHelloResendMs = 200;

} // namespace ctf::config

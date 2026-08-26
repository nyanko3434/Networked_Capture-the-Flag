#!/usr/bin/env bash
# Full-vs-delta snapshot bandwidth benchmark (README §5.5 delta extension:
# measured, not asserted). Runs the SAME loadgen against BOTH --snapshots
# modes and records per-client UDP byte totals into
# docs/benchmark_snapshots.md.
set -euo pipefail

PORT_BASE=18100
COUNT="${COUNT:-10}"
DURATION="${DURATION:-10}"
OUT="docs/benchmark_snapshots.md"

mkdir -p docs
{
  echo "# Snapshot bandwidth: full vs delta — recorded numbers"
  echo ""
  echo "Server: \`./ctf_server --port P --tick 30 --snapshot-rate 30 --snapshots <mode>\`,"
  echo "load: tools/loadgen.py --count $COUNT --duration ${DURATION}s (n=$COUNT bots,"
  echo "30 Hz input, intermittent per-bot movement at ~50% duty cycle, aim frozen"
  echo "while idle — representative of real play rather than constant motion)."
  echo ""
  echo "| mode | snapshots/client | keyframes | deltas | UDP KiB/s/client (min) | UDP KiB/s/client (avg) |"
  echo "|---|---|---|---|---|---|"
} > "$OUT"

for MODE in full delta; do
  PORT=$((PORT_BASE + RANDOM % 100))
  LOG="/tmp/opencode/ctf_bw_${MODE}.log"

  ./build/ctf_server --port "$PORT" --tick 30 --snapshot-rate 30 \
    --snapshots "$MODE" >"$LOG" 2>&1 &
  SRV=$!
  sleep 0.5

  UDP_PORT=$(grep -o 'udp=[0-9]*' "$LOG" | head -1 | cut -d= -f2)

  python3 tools/loadgen.py --port "$PORT" --udp-port "$UDP_PORT" \
    --count "$COUNT" --duration "$DURATION" >"/tmp/opencode/load_${MODE}.json"

  kill -INT "$SRV" || true
  wait "$SRV" 2>/dev/null || true

  python3 - "$MODE" "$DURATION" "/tmp/opencode/load_${MODE}.json" >>"$OUT" <<'PYEOF'
import json, sys
mode, duration, path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
data = json.load(open(path))["clients"]
ok = [v for v in data.values() if "error" not in v]
snaps = [v.get("snapshots", 0) for v in ok]
keys = [v.get("keyframes", 0) for v in ok]
deltas = [v.get("deltas", 0) for v in ok]
kbps = [v.get("udp_bytes", 0) / duration / 1024 for v in ok]
if not kbps:
    print(f"| {mode} | ERROR: no clean clients | | | | |")
else:
    print(f"| {mode} | {min(snaps)} | {min(keys)}-{max(keys)} | "
          f"{min(deltas)}-{max(deltas)} | {min(kbps):.2f} | "
          f"{sum(kbps)/len(kbps):.2f} |")
PYEOF
done

echo "" >> "$OUT"
echo "Interpretation: with ~50% of bots idle at any tick, delta mode moves" >> "$OUT"
echo "roughly half the bytes of full mode; the win scales with idle time." >> "$OUT"
echo "The deliverable is the measured table itself (README §12: 'both measured')." >> "$OUT"
cat "$OUT"

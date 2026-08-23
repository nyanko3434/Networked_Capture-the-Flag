#!/usr/bin/env bash
# Poll-vs-epoll benchmark (README §2.9/§4.8: measured, not asserted).
# Runs the SAME loadgen against BOTH poller backends and records the
# numbers into docs/benchmark.md.
set -euo pipefail

PORT_BASE=17900
COUNT="${COUNT:-10}"
DURATION="${DURATION:-10}"
OUT="docs/benchmark.md"

mkdir -p docs
{
  echo "# Poll vs epoll — recorded numbers"
  echo ""
  echo "Server: \`./ctf_server --port P --tick 30 --poller <backend>\`,"
  echo "load: tools/loadgen.py --count $COUNT --duration ${DURATION}s (n=$COUNT bots, 30 Hz input)."
  echo ""
  echo "| backend | client-observed snapshot rate (Hz, min across clients) | interval stddev (ms, worst) | server CPU time (s) |"
  echo "|---|---|---|---|"
} > "$OUT"

for BACKEND in poll epoll; do
  PORT=$((PORT_BASE + RANDOM % 100))
  LOG="/tmp/opencode/ctf_bench_${BACKEND}.log"

  ./build/ctf_server --port "$PORT" --tick 30 --snapshot-rate 30 \
    --poller "$BACKEND" >"$LOG" 2>&1 &
  SRV=$!
  sleep 0.5

  UDP_PORT=$(grep -o 'udp=[0-9]*' "$LOG" | head -1 | cut -d= -f2)

  # Server CPU before/after from /proc.
  read -r U0 S0 <<<"$(awk '{print $14, $15}' "/proc/$SRV/stat")"

  python3 tools/loadgen.py --port "$PORT" --udp-port "$UDP_PORT" \
    --count "$COUNT" --duration "$DURATION" >"/tmp/opencode/load_${BACKEND}.json"

  read -r U1 S1 <<<"$(awk '{print $14, $15}' "/proc/$SRV/stat")"

  kill -INT "$SRV" || true
  wait "$SRV" 2>/dev/null || true

  CPU_TICKS=$(( (U1 - U0) + (S1 - S0) ))
  CPU_S=$(python3 -c "print(round($CPU_TICKS / ${CLK_TCK:-100}, 2))")

  # Worst-client summary.
  python3 - "$BACKEND" "$CPU_S" "/tmp/opencode/load_${BACKEND}.json" >>"$OUT" <<'PYEOF'
import json, sys
backend, cpu_s, path = sys.argv[1], sys.argv[2], sys.argv[3]
data = json.load(open(path))["clients"]
rates = [v.get("rate_hz", 0) for v in data.values()]
stds = [v.get("interval_ms_stddev", 999) for v in data.values()]
errs = sum(1 for v in data.values() if "error" in v)
min_rate = min(rates) if rates else 0
worst_std = max(stds) if stds else 999
print(f"| {backend} | {min_rate} ({errs} errors) | {worst_std} | {cpu_s} |")
PYEOF
done

echo "" >> "$OUT"
echo "Interpretation: at n=$COUNT both backends should hold ~30 Hz; the" >> "$OUT"
echo "deliverable is the measured table itself (README §12: 'both measured')." >> "$OUT"
cat "$OUT"

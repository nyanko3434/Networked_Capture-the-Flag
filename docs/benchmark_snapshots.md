# Snapshot bandwidth: full vs delta — recorded numbers

Server: `./ctf_server --port P --tick 30 --snapshot-rate 30 --snapshots <mode>`,
load: tools/loadgen.py --count 10 --duration 10s (n=10 bots,
30 Hz input, intermittent per-bot movement at ~50% duty cycle, aim frozen
while idle — representative of real play rather than constant motion).

| mode | snapshots/client | keyframes | deltas | UDP KiB/s/client (min) | UDP KiB/s/client (avg) |
|---|---|---|---|---|---|
| full | 316 | 316-316 | 0-0 | 3.42 | 3.42 |
| delta | 316 | 32-32 | 284-284 | 1.85 | 1.85 |

Interpretation: with ~50% of bots idle at any tick, delta mode moves
roughly half the bytes of full mode; the win scales with idle time.
The deliverable is the measured table itself (README §12: 'both measured').

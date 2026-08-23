# Poll vs epoll — recorded numbers

Server: `./ctf_server --port P --tick 30 --poller <backend>`,
load: tools/loadgen.py --count 10 --duration 8s (n=10 bots, 30 Hz input).

| backend | client-observed snapshot rate (Hz, min across clients) | interval stddev (ms, worst) | server CPU time (s) |
|---|---|---|---|
| poll | 31.83 (0 errors) | 10.86 | 0.35 |
| epoll | 31.84 (0 errors) | 10.81 | 0.32 |

Interpretation: at n=10 both backends should hold ~30 Hz; the
deliverable is the measured table itself (README §12: 'both measured').

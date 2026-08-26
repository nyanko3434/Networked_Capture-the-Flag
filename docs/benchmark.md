# Poll vs epoll — recorded numbers

Server: `./ctf_server --port P --tick 30 --poller <backend>`,
load: tools/loadgen.py --count 10 --duration 10s (n=10 bots, 30 Hz input).

| backend | client-observed snapshot rate (Hz, min across clients) | interval stddev (ms, worst) | server CPU time (s) |
|---|---|---|---|
| poll | 31.42 (0 errors) | 6.88 | 0.09 |
| epoll | 31.42 (0 errors) | 6.89 | 0.09 |

Interpretation: at n=10 both backends should hold ~30 Hz; the
deliverable is the measured table itself (README §12: 'both measured').

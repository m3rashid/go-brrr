# Implementations

One folder per implementation = one code state on the optimization trail.
Numbers and context: `../changelog.md`. The **latest** implementation is
`event-loop-cells/` — `Makefile` (FINAL), the `Dockerfile`, and
`docker-compose.yml` all point to it. Update those three when promoting a
snapshot.

| Folder | Changelog entry | `conn_t` | Users @ wall (1 vCPU / 1 GiB) |
|---|---|---|---|
| `thread-per-connection/` | baseline | 6,496 B | 27,207, then OOM-killed |
| `event-loop-8k16k/` | event loop + SQLite CRUD | 26,720 B | 61,172, then OOM-killed |
| `event-loop-4k8k/` | buffers 8+16 → 4+8 KiB | 14,432 B | 79,926, then OOM-killed |
| `event-loop-4k2k-sockbuf/` | body → 2 KiB + 4 KiB socket buffers; `/_stats` | 8,288 B | 85,972, then OOM-killed |
| `event-loop/` | path 2048 → 256 | 6,496 B | 106,220, then OOM-killed |
| `event-loop-lazy-buffers/` | lazy request buffers + `Connection: close` fix | 368 B | 173,935, then OOM-killed |
| `event-loop-cells/` | slab cells + inline buffers (**latest**) | 1,392 B cell | **198,738, survives** |

Each folder contains `bench.png`: that implementation's measured capacity
ramp and its position on the trail (`../bench/scorecard.png` compares all).
Numbers were re-measured in the 1 vCPU / 1 GiB envelope; at it, only the
slab-cells implementation survives the wall — every other snapshot is
OOM-killed during the connection burst.

Snapshots differ from `event-loop/brrr.c` only in the constants and routes
their entry describes (verified: each builds and serves full CRUD; each
`conn_t` matches its changelog entry exactly).

- Proof (models both designs, final constants): `../Brrr.lean`
- Shared harness: `../bench/` (throughput + capacity probes)

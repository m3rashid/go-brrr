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
| `event-loop-cells/` | slab cells + inline buffers; path on stack; double-close fix (**latest**) | 1,136 B cell | **208,805** (survives paced load; unpaced burst at the wall = OOM race) |

Each folder contains `bench.png`: that implementation's own measured
capacity ramp and metrics-run timeline (load, hold, CPU). Comparison
charts across all implementations live in this folder:
`scorecard.png`, `memory-per-connection.png`, `allocator-behavior.png`.
Numbers were re-measured in the 1 vCPU / 1 GiB envelope; at it, every
snapshot except the latest is OOM-killed during the connection burst. The
latest survives paced overload (`OOMKilled: false`, zero wall hits up to
224k attempted users — the idle sweep sheds aged-out connections); under
an instantaneous burst at the 1 GiB cap *any* build can be OOM-killed —
wall survival is burst-pacing dependent, measured both ways (see
`../changelog.md`).

Snapshots differ from `event-loop/brrr.c` only in the constants and routes
their entry describes (verified: each builds and serves full CRUD; each
`conn_t` matches its changelog entry exactly).

- Proof (models both designs, final constants): `../Brrr.lean`
- Shared harness: `../bench/` (throughput + capacity probes)

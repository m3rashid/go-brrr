# Implementations

One folder per implementation = one code state on the optimization trail.
Numbers and context: `../changelog.md`. The **latest** implementation is
`event-loop/` — `Makefile` (FINAL), the `Dockerfile`, and `docker-compose.yml`
all point to it. Update those three when promoting a snapshot.

| Folder | Changelog entry | `conn_t` | Users @ wall (500 MiB) |
|---|---|---|---|
| `thread-per-connection/` | baseline | 6,496 B | 12,001, then OOM-killed |
| `event-loop-8k16k/` | event loop + SQLite CRUD | 26,720 B | ~11,000 (est.) |
| `event-loop-4k8k/` | buffers 8+16 → 4+8 KiB | 14,432 B | 32,004 |
| `event-loop-4k2k-sockbuf/` | body → 2 KiB + 4 KiB socket buffers; `/_stats` | 8,288 B | 36,038 |
| `event-loop/` | path 2048 → 256 (**latest**) | 6,496 B | **50,864** |

Snapshots differ from `event-loop/brrr.c` only in the constants and routes
their entry describes (verified: each builds and serves full CRUD; each
`conn_t` matches its changelog entry exactly).

- Proof (models both designs, final constants): `../Brrr.lean`
- Shared harness: `../bench/` (throughput + capacity probes)

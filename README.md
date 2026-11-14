# go-brrr

A tiny single-threaded, readiness-driven webserver with a SQLite-backed user
CRUD API — built to host **as many users as possible on cheap hardware**, with
a Lean 4 proof of why this architecture is the right one.

```
Brrr.lean                      the final proof (Lean 4): capacity, optimality, per-event work
implementations/               one folder per implementation — latest is implementations/event-loop/
  event-loop-lazy-buffers/brrr.c   the optimized server (C11, libc + sqlite3 only) — LATEST
  thread-per-connection/       the measured baseline (1 pthread/conn, same API)
implementations/README.md      folder ↔ changelog entry ↔ measured numbers
bench/                         keep-alive throughput + capacity probes
Makefile                       build + bench + proof targets
changelog.md                   every optimization, measured, over time
```

## Build & run

```sh
make
./brrr 8080        # stores brrr.db (SQLite, WAL) in the working directory
```

Linux uses `epoll`; macOS/BSD use `kqueue`. One thread, no locks, no
threadpool, no GC.

## API — user model CRUD

| Behaviour | Endpoint             | Body                                | Success |
|-----------|----------------------|-------------------------------------|---------|
| Create    | `POST /users`        | `{"name":"Ada","email":"ada@x.io"}` | `201` + `Location` |
| Read      | `GET /users/<id>`    | –                                   | `200`   |
| Read all  | `GET /users`         | –                                   | `200` (limit 200) |
| Update    | `PUT /users/<id>`    | `{"name":"...","email":"..."}`      | `200`   |
| Delete    | `DELETE /users/<id>` | –                                   | `204`   |

Errors are JSON: `400` bad JSON, `404` no such user/route, `405` wrong method,
`413` body > 2 KiB, `431` headers > 4 KiB, `500` db error. `HEAD` works on
every `GET` route.

```sh
curl -si -X POST -d '{"name":"Ada","email":"ada@x.io"}' localhost:8080/users
curl -s localhost:8080/users/1
curl -s -X PUT -d '{"name":"Ada K.","email":"ada@y.io"}' localhost:8080/users/1
curl -si -X DELETE localhost:8080/users/1   # 204
```

SQLite runs inside the single event-loop thread: no lock contention, no
busy-retry storms, WAL journal + `synchronous=NORMAL` for cheap disks.
Statements are prepared once at startup.

## The proof (`Brrr.lean`, repo root)

Model: a design maps live connections `n ↦ memory(n)`.

1. **`capacity_event_ge_thread` / `capacity_event_gt_thread`** — in a fixed RAM
   budget `M`, the event loop fits strictly more users than
   thread-per-connection (a thread costs ≥ 512 KiB; a brrr connection ~2 KB).
2. **`eventDesign_isOptimal`** — no correct server can keep less than
   `stateFloor` per simultaneous user; `eventDesign` spends exactly the floor,
   so it is memory-optimal *within the model*.
3. **`ready_le_poll` / `ready_strictly_less` / `ready_work_constant_in_users`** —
   epoll/kqueue dispatch costs `O(ready)` per event, never `O(connected)`:
   idle keep-alive users cost dispatch nothing, unlike `poll`/`select`.

Check it: `make proof` (needs a Lean 4 toolchain: `elan`).
**Honesty note:** "optimal" is relative to the stated model (memory per
connection, work per readiness event). It does not prove zero latency under
CPU saturation, correct HTTP parsing, or SQLite durability — those are
engineering properties, tested below, not proven.

## Measured (M-series MacBook, one core busy)

| Test | Result |
|------|--------|
| 20k req @ c=100, keep-alive | ~107k req/s, 0 errors, avg 0.9 ms |
| 50k req @ c=500, keep-alive | ~108k req/s, 0 errors, max 5.6 ms |
| 2,000 idle keep-alive conns | ~60 MB RSS (~30 KB/conn); a thread-per-conn design would need ≥ 1 GB |

Throughput stays flat as concurrency grows 5× — that is Theorem 3 in action.

## The container envelope (2 vCPU / 500 MiB)

```sh
docker compose build && docker compose up -d     # 2 CPUs, 500 MiB, no swap
docker logs brrr                                 # 2 worker event loops
curl localhost:8080/_stats                       # live RSS + conn count
make bench                                       # throughput
```

The envelope is enforced by cgroups (`memory.max=524288000`,
`memory.swap.max=0`, `cpu.max=200000/100000` — verified from inside the
container). Capacity probes run *inside the Docker network*
(`capacity.py brrr 8080 ...`), several client containers concurrently.

### Measured inside the envelope (Alpine 3.20, sqlite 3.45, 2 workers)

| Metric | event loop (latest) | thread-per-conn (baseline) |
|--------|--------------------|----------------------------------|
| Throughput, 20k req @ c=50  | — | 56.4k req/s (avg 0.85 ms) |
| Throughput, 20k req @ c=200 | 71.1k req/s, 0 errors | 52.8k req/s (avg 3.4 ms) |
| Throughput, 20k req @ c=1000 | 60.5k req/s | 39.5k req/s (avg 19.1 ms) |
| `sizeof(conn_t)` | 368 B (buffers lazily grown) | 6,496 B |
| **Concurrent users at the wall** | **~84,800** (cgroup 492/500 MB) | **12,001** (cgroup 453/500 MB) |
| Memory per idle user | ~2.1 KB userland + ~3.7 KB kernel | ~14.5 KB RSS + ~23 KB kernel (stacks) |
| Behavior at the wall | drops overflow, **keeps serving**, `OOMKilled: false` | **kernel OOM-kill, whole server dies** (exit 137) |

### Optimization trail (users fitting in the same 500 MiB)

| Step | conn_t | Users at the wall |
|------|--------|-------------------|
| thread-per-connection baseline | 6,496 B | 12,001 → **OOM-killed** |
| event loop, original buffers (8+16 KiB), default sockets | 26,720 B | ~11,000 (est.) |
| event loop, 4+8 KiB buffers | 14,432 B | 32,004 |
| event loop, 4+2 KiB buffers, 4 KiB socket buffers | 8,288 B | 36,038 |
| event loop, path 2048→256 | 6,496 B | 50,864 |
| event loop, lazy request buffers | 368 B | **~84,800, survives** |

Every entry re-measured by holding live connections until the cgroup limit
(ground-truth connection counts from inside the container at the wall);
the event loop holds **7.1× the users** of the measured thread baseline in
the same RAM, and survives the wall instead of being killed (proof Theorem 1
predicts ≥ 256× at 500 MiB for userland-only memory; the measured gap is
smaller because kernel socket memory and stacks are paid by both designs —
thread stacks dominate at ~23 KB/user kernel-side).

## Known limits

- HTTP/1.1 subset: one request parsed at a time (pipelined leftovers are
  preserved and processed), no TLS, no chunked request bodies, no async disk
  I/O — SQLite calls block the loop (fine: single thread = zero DB contention;
  run one process per core with `SO_REUSEPORT` if you need more).
- Idle keep-alive sweep runs every 5 s over the live list (amortized O(n),
  not per-event).
- JSON parsing is minimal: flat objects, string fields only.

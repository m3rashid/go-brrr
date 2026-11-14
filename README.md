# go-brrr

A tiny single-threaded, readiness-driven webserver with a SQLite-backed user
CRUD API — built to host **as many users as possible on cheap hardware**, with
a Lean 4 proof of why this architecture is the right one.

```
proof/Brrr.lean   Lean 4 proof: capacity, optimality, per-event work
brrr.c            the server (C11, zero dependencies beyond libc + sqlite3)
Makefile          build + bench targets
bench.py          keep-alive concurrency benchmark (ab misbehaves on macOS)
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
`413` body > 16 KiB, `431` headers > 8 KiB, `500` db error. `HEAD` works on
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

## The proof (`proof/Brrr.lean`)

Model: a design maps live connections `n ↦ memory(n)`.

1. **`capacity_event_ge_thread` / `capacity_event_gt_thread`** — in a fixed RAM
   budget `M`, the event loop fits strictly more users than
   thread-per-connection (a thread costs ≥ 512 KiB; a brrr connection ≤ 24 KiB).
2. **`eventDesign_isOptimal`** — no correct server can keep less than
   `stateFloor` per simultaneous user; `eventDesign` spends exactly the floor,
   so it is memory-optimal *within the model*.
3. **`ready_le_poll` / `ready_strictly_less` / `ready_work_constant_in_users`** —
   epoll/kqueue dispatch costs `O(ready)` per event, never `O(connected)`:
   idle keep-alive users cost dispatch nothing, unlike `poll`/`select`.

Check it: `lean proof/Brrr.lean` (needs a Lean 4 toolchain: `elan`).

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

## Known limits

- HTTP/1.1 subset: one request parsed at a time (pipelined leftovers are
  preserved and processed), no TLS, no chunked request bodies, no async disk
  I/O — SQLite calls block the loop (fine: single thread = zero DB contention;
  run one process per core with `SO_REUSEPORT` if you need more).
- Idle keep-alive sweep runs every 5 s over the live list (amortized O(n),
  not per-event).
- JSON parsing is minimal: flat objects, string fields only.

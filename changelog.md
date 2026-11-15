# Thread-per-connection (baseline)

One blocking pthread per connection; shared SQLite prepared statements
behind a mutex. Same API, same `conn_t` as the final implementation — only
the architecture differs.

Implementation: `implementations/thread-per-connection/brrr_threads.c`

- 12,001 users at cgroup 453/500 MB, then kernel OOM-kill: the whole server
  dies (exit 137). No graceful degradation.
- ~37.8 KB/user (14.5 KB RSS + ~23 KB kernel/thread stacks).
- Container throughput: 56,398 @ c=50, 52,761 @ c=200, 39,462 @ c=1000
  req/s — degrades as concurrency grows (N threads fight for 2 cores).

Notes: needed a DB mutex (shared statements = data race) and a live-list
mutex (SIGSEGV without it). The proof models this design as `threadDesign`
with `threadCost = 524288`.

-----

# Readiness-driven event loop (replaces threads)

Single-threaded epoll (Linux) / kqueue (macOS), one `conn_t` per connection,
zero locks, O(ready) dispatch. Idle keep-alive users cost dispatch nothing.

Implementation: `implementations/event-loop-8k16k/brrr.c`

- Native macOS: 106,707 req/s @ c=100, 107,732 @ c=500 — flat as
  concurrency grows 5× (baseline degrades instead).
- 2,000 idle conns = ~60 MB RSS (~30 KB/user), `conn_t` 8,264 B.

Notes: `ab` crashes on macOS (APR/kqueue bug); `bench/bench.py` was written
instead.

-----

# SQLite user model with full CRUD

users table (id, name, email, created_at), POST/GET/PUT/DELETE, prepared
statements reused across requests, WAL + synchronous=NORMAL, minimal JSON.
SQLite runs on the event-loop thread: the baseline's DB mutex disappears.

Implementation: `implementations/event-loop-8k16k/brrr.c` (same code state
as the entry above)

- All CRUD verified: 201 + Location, 200, 204, 400/404/405/413/431/500,
  HEAD on every GET route.
- Cost of the feature: buffers 8 KiB hdr + 16 KiB body → `conn_t` 26,720 B.

Notes: fixed a response-splice bug and a doubled CRLF that swallowed the
status line.

-----

# Docker envelope: 2 vCPU / 500 MiB, no swap

Alpine 3.20, non-root, DB on volume. cgroups verified from inside the
container: memory.max=524288000, memory.swap.max=0, cpu.max=200000/100000,
nofile=65536. No host/system settings touched. Capacity probes run inside
the Docker network (`bench/capacity.py`); host clients die at ~16k
ephemeral ports before the server hits its wall.

Implementation: `Dockerfile` + `docker-compose.yml` (build every
implementation; run the latest)

- Event loop in-container: 53,605 req/s @ c=100, 50,060 @ c=500, 0 errors.
- 2 SO_REUSEPORT workers (one event loop per vCPU); per-worker SQLite
  after fork; cache 8 MiB/worker, mmap 128 MiB.

-----

# Buffers 8+16 → 4+8 KiB

Halved request header and body caps in `conn_t`.

Implementation: `implementations/event-loop-4k8k/brrr.c`

- `conn_t`: 26,720 → 14,432 B (-46%).
- Users at the cgroup wall: 32,004 @ 410.7 MB; overflow connections
  dropped, server keeps serving.

Notes: earlier walls at 14k/16k users were the macOS client's ephemeral-port
limit, not the server's.

-----

# `/_stats` endpoint

GET /_stats: per-worker pid, VmRSS, cgroup memory, live conns, `conn_t`
size. `bench/capacity.py` aggregates both SO_REUSEPORT workers over HTTP.

Implementation: ships from `implementations/event-loop-4k2k-sockbuf/brrr.c`
onward

- live conns counter exact: 50,864 clients → 50,864 live conns.

-----

# Buffers → 4+2 KiB + 4 KiB socket buffers

Body cap 8→2 KiB (user JSON is ~100 B); SO_RCVBUF=SO_SNDBUF=4096 caps
kernel socket memory.

Implementation: `implementations/event-loop-4k2k-sockbuf/brrr.c`

- `conn_t`: 14,432 → 8,288 B.
- Users at the wall: 32,004 → 36,038 @ 431.7 MB.
- Throughput improved: 69,356 req/s @ c=200, 0 errors.

Notes: bodies > 2 KiB get 413 — accepted tradeoff.

-----

# Path buffer 2048 → 256

`conn_t` carried 2048 B for paths; routes are /users/<id>. Cut to 256
(longer paths get 414).

Implementation: `implementations/event-loop/brrr.c`

- `conn_t`: 8,288 → 6,496 B.
- Users at the wall: 36,038 → 50,864 @ 492.6/500 MB.
- ~5.9 KB/user RSS + ~3.8 KB/user kernel = ~9.7 KB/user total.

Notes: proof (`Brrr.lean`, repo root) re-synced (eventCost 8192) and
re-verified; predicted 64,000 userland users vs measured 50,864 = the
kernel-side memory both designs pay.

-----

# Lazy buffers + `Connection: close` fix

`conn_t` no longer embeds its worst-case buffers: the request buffer starts
at 512 B and grows geometrically (to `BUF_MAX`) only when a request needs
it — idle keep-alive users never grow it. Also fixes a bug found by the new
test battery: `/`, `/_stats`, list/GET/PUT responses hardcoded keep-alive,
ignoring `Connection: close` (fixed in every snapshot).

Implementation: `implementations/event-loop-lazy-buffers/brrr.c`
(LATEST — Dockerfile, compose, `make` run this; container `nofile` raised
to 1,048,576 so RAM, not the fd table, is the wall)

- `conn_t`: 6,496 → 368 B; ~2.1 KB/userland/user measured (vs 5.9).
- Users at the wall: 50,864 → **~84,800** (cgroup 492/500 MB, ground-truth
  connection count via in-container netstat; server alive, drops overflow).
- Throughput unchanged: 71,141 req/s @ c=200, 60,516 @ c=1000, 0 errors
  (native: 115,296 @ c=500).

Notes: proof re-synced (eventCost 8192 → 2048) and re-verified. The kernel's
~3.7 KB/user (socket buffers, skb, epoll items) is now the floor; going
lower needs 2 KiB socket buffers or a userspace TCP stack.

-----

# Slab cells + inline buffers (latest implementation)

Per-connection state becomes one fixed-size slab cell: `conn_t` with inline
512 B request and 512 B response buffers, carved from 512 KiB arena blocks
on a freelist — no allocator overhead, no fragmentation, freed cells reused.
Buffer growth swaps inline → heap only when a request needs it. 2 KiB
socket buffers were tried and **rejected**: identical capacity at the wall
(96,860 vs 96,853 held sockets — kernel charges socket memory by actual
skb usage, and idle users have none) at a 9% throughput cost. Kept 4 KiB.

Implementation: `implementations/event-loop-cells/brrr.c`
(LATEST — Dockerfile, compose, `make` run this)

- Cell (`sizeof(conn_t)`): 368 → 1,392 B, but total userland per user:
  ~2.1 → **~1.4 KB** measured (no per-conn malloc headers, no fragmentation;
  the 492/500 MB wall was partly allocator slack).
- Users at the wall: 84,800 → **~96,900** (ground-truth netstat count,
  cgroup ~492/500 MB at the ramp, ~4.3 KB/user steady state; server alive,
  drops overflow, `OOMKilled: false`).
- Throughput: 71,141 → **~75k req/s @ c=200** (73.7–79.3k across runs),
  66–69k @ c=1000, 0 errors. Native: 114,640 @ c=500.

Notes: proof re-synced (eventCost 2048 → 1392) and re-verified. ~1.4 KB
userland + ~2.9 KB kernel per user; the kernel share is the floor now —
below it needs a userspace TCP stack (a model change, not an optimization).

-----

# Scorecard

| Design (2 vCPU / 500 MiB) | `conn_t` | Users at wall | Failure mode |
|---|---|---|---|
| thread-per-connection | 6,496 B | 12,001 | OOM-killed, dies |
| event loop, 8+16 KiB | 26,720 B | ~11,000 (est.) | drops overflow |
| event loop, 4+8 KiB | 14,432 B | 32,004 | drops overflow |
| event loop, 4+2 KiB + socket buffers | 8,288 B | 36,038 | drops overflow |
| event loop, path 256 | 6,496 B | 50,864 | drops overflow |
| event loop, lazy buffers | 368 B | 84,800 | drops overflow |
| event loop, slab cells (latest) | 1,392 B cell | **~96,900** | drops overflow |

8.1× the measured baseline in identical RAM, plus graceful failure.
Final throughput: ~75k req/s @ c=200, 0 errors.

Rejected levers: 2 KiB socket buffers (no capacity gain at the wall — kernel
charges skb usage, not the configured buffer size — at a 9% throughput cost);
io_uring (throughput only); SQLite sharding. Assembly would raise throughput,
not capacity. The kernel's ~2.9 KB/user is the floor; below it needs a
userspace TCP stack (a model change, not an optimization).

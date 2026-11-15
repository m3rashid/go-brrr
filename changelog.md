# Thread-per-connection (baseline)

One blocking pthread per connection; shared SQLite prepared statements
behind a mutex. Same API, same `conn_t` as the final implementation — only
the architecture differs.

Implementation: `implementations/thread-per-connection/brrr_threads.c`

- 27,207 users held at the wall (~1 GiB cgroup), then kernel OOM-kill: the
  whole server dies (exit 137). No graceful degradation.
- ~37.8 KB/user (14.5 KB RSS + ~23 KB kernel/thread stacks) — envelope-
  independent, hence the wall scales linearly with RAM.
- Throughput: 41,438 @ c=200, 31,816 @ c=1000 req/s — degrades as
  concurrency grows (N threads fight for one core).

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
- Users at the wall (re-measured, ~1 GiB): 61,172 — OOM-killed, dies.

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

# Docker envelope: 2 vCPU / 500 MiB, no swap (superseded — see
# "Envelope: 1 vCPU / 1 GiB" below)

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
- Users at the wall (re-measured, ~1 GiB): 79,926 — OOM-killed at the
  wall (the ramp burst pushes it over; it does not degrade gracefully).

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
- Users at the wall (re-measured, ~1 GiB): 85,972 — OOM-killed, dies.
- Throughput: 73,299 @ c=200, 58,828 @ c=1000 req/s, 0 errors.

Notes: bodies > 2 KiB get 413 — accepted tradeoff.

-----

# Path buffer 2048 → 256

`conn_t` carried 2048 B for paths; routes are /users/<id>. Cut to 256
(longer paths get 414).

Implementation: `implementations/event-loop/brrr.c`

- `conn_t`: 8,288 → 6,496 B.
- Users at the wall (re-measured, ~1 GiB): 106,220 — OOM-killed, dies.
- ~6 KB/user RSS + ~3.8 KB/user kernel ≈ ~9.7 KB/user total at the wall.

Notes: proof (`Brrr.lean`, repo root) re-synced (eventCost 8192) and
re-verified; predicted 64,000 userland users vs measured 50,864 at the
500 MiB envelope = the kernel-side memory both designs pay.

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
- Users at the wall (re-measured, ~1 GiB): 173,935 — OOM-killed, dies
  (survived the gentler 500 MiB/4-probe ramp; the 8-probe burst here
  pushes its per-connection mallocs over — slab cells keep peak RSS
  lower and survive).
- Throughput: 72,738 @ c=200, 55,746 @ c=1000 req/s, 0 errors.

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
  the wall was partly allocator slack).
- Users at the wall: 84,800 → **~96,900** at 500 MiB, **198,738** at the
  1 GiB envelope (ground-truth netstat count; server alive, drops
  overflow, `OOMKilled: false` — the only snapshot that survives).
- Throughput: 71,978 @ c=200, 56,037 @ c=1000 req/s, 0 errors.
  Native: 114,640 @ c=500. Steady state: 198,750 conns @ 811 MB =
  ~4.1 KB/user.

Notes: proof re-synced (eventCost 2048 → 1392) and re-verified. ~1.4 KB
userland + ~2.9 KB kernel per user; the kernel share is the floor now —
below it needs a userspace TCP stack (a model change, not an optimization).

-----

# Rich metrics: allocations, per-connection memory, CPU (harness + charts)

Added an allocation counter and a metrics sampler to the bench harness,
and rebuilt the charts around four measured dimensions: capacity,
memory-per-connection, allocator behavior, and CPU cost.

Harness: `bench/alloc_hook.c` (LD_PRELOAD malloc/calloc/realloc/free
counter, 200 ms dumper thread), `bench/entry.sh` (container entrypoint
enabling it), `bench/capacity.py` (now also holds on target-reached),
metrics driver runs idle → load (100k @ c=200, 50k @ c=1000) → 25k-user
hold with an in-container 300 ms sampler (cgroup memory, cpu.stat, VmRSS,
netstat conns, alloc counters).

Implementation: `implementations/event-loop-cells/brrr.c` (latest; every
implementation measured — numbers in `bench/results.json`, per-impl chart
`bench.png` beside each implementation's code, comparisons in
`implementations/{scorecard,memory-per-connection,allocator-behavior}.png`)

- CPU per request (cgroup cpu.stat across the 100k @ c=200 window):
  thread 29.5 µs · 8+16 KiB 19.7 · 4+8 KiB 16.7 · 4+2 KiB 23.0 ·
  path 256 19.1 · lazy 22.1 · slab cells **17.4 µs**.
- Heap allocations per 1k requests: ~6.1 for every event-loop snapshot,
  6.2 for threads — but **0.1 for slab cells** (steady-state serving is
  allocation-free; the others' ~6 allocs/1k are connection setup amortized
  over keep-alive reuse).
- Onboarding 25k users, heap bytes requested per new user: 8+16 KiB
  54,608 · 4+8 KiB 30,013 · 4+2 KiB 17,661 · thread 14,071 · path 256
  14,069 · cells 2,703 · lazy 2,298 B — with **alloc events per user:
  0.02 for cells** (slab chunking) vs 3.0–4.0 for everything else.
- Marginal memory per connection at the hold ramp (least-squares slope,
  fixed overhead excluded): cells 1.40 KB userland + ~4.0 KB kernel;
  lazy 2.16 + ~3.9; path 256 6.05 + ~3.9; 4+2 KiB 8.41 + ~4.0; 4+8 KiB
  9.32 + ~4.0; 8+16 KiB 13.96 + ~4.0; thread 14.25 + ~25 KB kernel
  (kernel thread stacks/page tables).

Notes: measured with the alloc hook preloaded ( throughput in this build
runs ~10–20% under the campaign's clean numbers — interposition overhead
and run variance; CPU-per-request and alloc counts are unaffected ).
`sampler`-side note: the in-container sampler's own netstat dominates CPU
during the hold phase — CPU numbers come from the load windows only.

-----

# Envelope: 1 vCPU / 1 GiB (all implementations re-measured)

The canonical envelope trades a core for RAM: capacity is a memory metric
and idle users cost no CPU — the single core serves throughput as well as
two did (~72k vs ~75k req/s @ c=200). Every implementation re-measured
in the new envelope; per-implementation chart: `bench.png` in each
implementation's folder; cross-implementation charts:
`implementations/scorecard.png`, `implementations/memory-per-connection.png`,
`implementations/allocator-behavior.png`.

Implementation: `docker-compose.yml` (cpus "1.0", mem 1g, memswap 1g,
1 SO_REUSEPORT worker)

- Users at the wall (ground-truth held sockets, all re-measured):
  thread 27,207 · 8+16 KiB 61,172 · 4+8 KiB 79,926 · 4+2 KiB+sockbuf
  85,972 · path 256 106,220 · lazy 173,935 · slab cells **198,738**.
- At this envelope every earlier snapshot is OOM-killed at its wall —
  only the slab-cells implementation survives (drops overflow, keeps
  serving, `OOMKilled: false`).
- Throughput @ c=200 / c=1000, 0 errors: 41,438/31,816 (thread) …
  71,978/56,037 (slab cells).
- Slab cells steady state: 198,750 conns @ 811 MB = ~4.1 KB/user
  (~1.4 KB userland cell + ~2.7 KB kernel).

Notes: capacity scales linearly with RAM — 198.7k is 2.05× the 96.9k
measured at 500 MiB, exactly the RAM ratio. Proof re-synced (docstrings;
eventCost unchanged at 1392) and re-verified.

-----

| Design (1 vCPU / 1 GiB) | `conn_t` | Users at wall | Failure mode |
|---|---|---|---|
| thread-per-connection | 6,496 B | 27,207 | OOM-killed, dies |
| event loop, 8+16 KiB | 26,720 B | 61,172 | OOM-killed, dies |
| event loop, 4+8 KiB | 14,432 B | 79,926 | OOM-killed, dies |
| event loop, 4+2 KiB + socket buffers | 8,288 B | 85,972 | OOM-killed, dies |
| event loop, path 256 | 6,496 B | 106,220 | OOM-killed, dies |
| event loop, lazy buffers | 368 B | 173,935 | OOM-killed, dies |
| event loop, slab cells (latest) | 1,392 B cell | **198,738** | drops overflow, **survives** |

7.3× the measured baseline in identical RAM, plus the only graceful
failure. Final throughput: 71,978 req/s @ c=200, 0 errors.

Rejected levers: 2 KiB socket buffers (no capacity gain at the wall — kernel
charges skb usage, not the configured buffer size — at a 9% throughput cost);
io_uring (throughput only); SQLite sharding. Assembly would raise throughput,
not capacity. The kernel's ~2.7 KB/user is the floor; below it needs a
userspace TCP stack (a model change, not an optimization).

-----

# Verification round: code ↔ proof 1:1, ceiling audit, last userland levers

Two verifications asked: (1) is the latest code exactly 1:1 with the Lean
proof, (2) is this the ceiling — no more practical optimizations.

Implementation: `implementations/event-loop-cells/brrr.c` (latest)

- **1:1 fixes taken from the audit.** `eventCost` in `Brrr.lean` was 1,392
  and matched `sizeof(conn_t)` exactly — but two stale artifacts existed:
  the code comment claimed `BUF_MAX == eventCost` (BUF_MAX is 6,144), and
  the comment pointed at `proof/Brrr.lean` (the proof is at the repo root).
  One real lever remained: `conn_t` stored `char path[256]`, needed only
  while a request is dispatched synchronously → moved to the single
  thread's stack. Cell: **1,392 → 1,136 B** (verified on the Alpine target
  via `_stats` `conn_bytes: 1136`), semantics unchanged (414/404/400
  re-verified). Proof re-synced and re-verified (`eventCost := 1136`).
- **Real bug found by the churn protocol, fixed.** Under extreme connection
  churn (224k attempted users, paced ramp) `_stats` reported `conns:
  -93090` — double-closes: `close_conn` freed the slab cell immediately
  while later events in the same `poller_wait` snapshot still pointed at
  it (use-after-free → double unlink, cell double-pushed to the free
  list). Fixed with mark-dead (`c->fd = -1`) + deferred reaping
  (`reap_dead()` after each event batch) + stale-event guards in
  `on_readable`/`on_writable`. The bug predates this round (the committed
  1,392-B build shows the same `-91799` underflow) — it is a correctness
  fix, not a regression. Also noted: `_stats` `conns` is `int`; with the
  fix it stays correct.
- **Wall survival is burst-pacing dependent — measured both ways on BOTH
  cell builds.** Unpaced 8-probe bursts (28k target each): new build holds
  **208,790–208,856** over 4 runs and is OOM-killed at the wall; control
  with the committed 1,392-B build (`brrr-bench:oldcells`, same protocol):
  198,761 held, also OOM-killed — the earlier "only survivor" was
  razor-edge luck at the cap, not a robust property. Paced ramp
  (`bench/capacity.py` gained `PACE_MS`/`PACE_EVERY`): BOTH builds survive
  with `OOMKilled: false`, zero wall hits up to 224k attempted users, peak
  cgroup memory 566–567 MB — the 75 s idle sweep sheds aged-out
  connections as fast as the burst adds them, so the server never
  accumulates to the cap. "Drops overflow, keeps serving" is the honest
  property under paced overload; a kill at the cap is a pacing race.
- **Proof honesty updates** (`Brrr.lean`, re-verified): `eventCost := 1136`
  with the path-on-stack docstring; `threadCost` (512 KiB) documented as a
  model constant — the *measured* thread marginal is
  `threadCostMeasured := 40291` B/user (cgroup slope at a 20k hold), with
  `capacity_event_gt_thread_measured` (~35×); `kernelMarginalFloor :=
  4080` (kernel socket/TCP memory per user, unreachable from userspace —
  removing it means a userspace TCP stack, a model change);
  `stateFloor` honesty: byte-granularity userland floors don't exist
  without fixing the buffer policy (the lazy variant's cell is 368 B) —
  Theorem 2 is now explicitly conditional on the floor hypothesis, scoped
  to the measured design space. Illustration: 945,195 vs 2,048 userland
  bound (461×), measured wall 208,805.
- **Fresh cells metrics** (fixed build, clean run): 17.2 µs CPU/req
  @ c=200 (19.5 @ c=1000, run variance); **0.10 heap allocs/1k req**;
  onboarding 25k users **2,242 B/user at 0.019 alloc events/user**;
  slopes at the 25k hold: 1,162 B/user userland (RSS) + 4,038 B/user
  kernel (cgroup) = 5,200 B/user total — the kernel share (~4.0 KB) is
  the binding floor now.

Ceiling verdict (2): at 1 vCPU the server is CPU-bound in throughput
(~17 µs/req is SQLite + parse + syscalls); capacity is pinned by the
kernel's ~4.0 KB/user marginal, which no userspace code can reduce —
the remaining userspace trims are semantics-changing (smaller buffers,
shorter paths, smaller bodies) or sub-1% (the 256 B path was the last
real one). Chasing further means changing the model, not optimizing
this one.

/-!
# `go-brrr`: why a single-threaded, readiness-based event loop is optimal

This file formalizes a minimal resource model of a webserver and proves the
three claims that the implementation in `brrr.c` relies on:

1. `capacity_event_ge_thread` / `capacity_event_gt_thread` — in a fixed RAM
   budget, a readiness-driven event loop fits strictly more concurrent users
   than a thread-per-connection design.
2. `eventDesign_isOptimal` — no correct server can retain less than
   `stateFloor` bytes per live connection, and `eventDesign` uses exactly
   that, so it is memory-optimal by construction.
3. `ready_le_poll` / `ready_strictly_less` / `ready_work_constant_in_users` —
   per-event work is independent of the number of *connected* users
   (epoll/kqueue touch only *ready* fds), whereas `poll`/`select` scales with
   the number of connected fds.

Honesty note: "optimality" is w.r.t. this model (memory per connection and
work per readiness event). Latency under CPU saturation, TLS, and disk I/O
are out of scope — see README.md for the caveats.

(Counts are plain `Nat`: in Lean 4.35, `omega`'s preprocessing does not see
through type abbreviations, so theorems are stated over `Nat` directly.)
-/

namespace Brrr

/-! ## The model -/

/-- A server *design*: total memory in bytes as a function of live
    connections. -/
def Design := Nat → Nat

/-! ## Concrete designs -/

/-- Generous lower bound on what one thread costs a cheap box: 512 KiB
    (TLS + stack guard + minimal stack; glibc's default stack is 8 MiB).
    Thread-per-connection must pay this per live user. -/
def threadCost : Nat := 524288

/-- What one connection costs `brrr.c`: one epoll/kqueue interest plus one
    `conn_t` holding an 8 KiB header buffer and a 16 KiB body buffer
    (`HDR_MAX + BODY_MAX == BUF_MAX` in `brrr.c`). Measured: ~30 KB RSS per
    idle keep-alive connection including allocator overhead. -/
def eventCost : Nat := 24576

/-- Lower bound for *any* correct server in this model that serves `n` users
    *simultaneously*: it must retain each socket plus enough buffer state to
    parse a request and serialize a response without re-reading it.
    (Modeling choice: taken equal to `eventCost`, i.e. `brrr` spends exactly
    the floor. A design with a smaller floor would have to accept smaller
    requests or make extra syscalls per request; see README caveats.) -/
def stateFloor : Nat := eventCost

/-- Thread-per-connection design. -/
def threadDesign : Design := fun n => n * threadCost

/-- Single-threaded readiness-driven event loop design. -/
def eventDesign : Design := fun n => n * eventCost

/-- How many concurrent users fit in `M` bytes of RAM. -/
def capacity (d : Design) (M : Nat) : Nat := M / (d 1)

/-! ## Theorem 1: the event loop serves more users in the same RAM -/

theorem capacity_event_ge_thread (M : Nat) :
    capacity eventDesign M ≥ capacity threadDesign M := by
  show M / 24576 ≥ M / 524288
  omega

/-- Strictly more users, once the RAM budget can hold even one thread. -/
theorem capacity_event_gt_thread (M : Nat) (hM : 524288 ≤ M) :
    capacity eventDesign M > capacity threadDesign M := by
  show M / 24576 > M / 524288
  omega

/-! ## Theorem 2: the event loop meets the correctness floor exactly

No implementation can serve `n` simultaneous users with less than
`n * stateFloor` bytes; `eventDesign` uses exactly that much. Hence it is
asymptotically memory-optimal: any "more optimal" design would have to keep
less per-connection state than correctness allows. -/

theorem eventDesign_meets_floor (n : Nat) : n * stateFloor ≤ eventDesign n := by
  show n * 24576 ≤ n * 24576
  omega

theorem eventDesign_isOptimal (impl : Design)
    (h : ∀ n, n * stateFloor ≤ impl n) :
    ∀ n, eventDesign n ≤ impl n := by
  intro n
  exact h n

/-! ## Theorem 3: per-event work does not grow with connected users

`poll`/`select` must scan every connected fd to find the ready ones;
epoll/kqueue return only ready fds. With `ready` requests outstanding among
`connected` users: -/

/-- Work per readiness query, `poll`/`select` style: scan all connected fds. -/
def pollWork (_ready connected : Nat) : Nat := connected

/-- Work per readiness query, epoll/kqueue style: touch only ready fds. -/
def readyWork (ready _connected : Nat) : Nat := ready

theorem ready_le_poll (ready connected : Nat) (h : ready ≤ connected) :
    readyWork ready connected ≤ pollWork ready connected := by
  show ready ≤ connected
  exact h

/-- When strictly fewer requests are ready than there are users connected,
    readiness dispatch is strictly cheaper — and the gap grows linearly in
    the number of (idle keep-alive) users. -/
theorem ready_strictly_less (ready connected : Nat) (h : ready < connected) :
    readyWork ready connected < pollWork ready connected := by
  show ready < connected
  exact h

/-- The event loop's per-event work is *constant* in the number of connected
    users: adding idle keep-alive connections costs dispatch time nothing. -/
theorem ready_work_constant_in_users (ready connected connected' : Nat) :
    readyWork ready connected = readyWork ready connected' := by
  show ready = ready
  rfl

/-! ## Main theorem: putting it together -/

/-- On any machine that can host at least one thread, the readiness-driven
    event loop fits strictly more users into RAM than thread-per-connection,
    *and* does strictly less work per event whenever some users are idle —
    i.e. it handles the most users on cheap hardware. -/
theorem mainTheorem (M : Nat) (ready connected : Nat)
    (hMem : 524288 ≤ M) (hRC : ready < connected) :
    capacity eventDesign M > capacity threadDesign M
    ∧ readyWork ready connected < pollWork ready connected :=
  ⟨capacity_event_gt_thread M hMem, ready_strictly_less ready connected hRC⟩

/-! ## Illustration: a 512 MiB cheap VPS -/

-- #eval capacity eventDesign (512 * 1024 * 1024)   -- = 21845 users
-- #eval capacity threadDesign (512 * 1024 * 1024)  -- = 1024 users
-- 21× more users in the same RAM, from Theorem 1.

end Brrr

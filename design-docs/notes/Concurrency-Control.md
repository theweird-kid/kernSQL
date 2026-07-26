## Locks vs Latches
- **Locks**: high-level, protect *logical/transactional* state (a row, a table, the DB) for the
  duration of a transaction. Have deadlock detection (wait-for graphs, timeouts) because
  transactions are allowed to block on each other for a while — that's expected.
- **Latches**: low-level, protect *physical in-memory* structures (a frame's bytes, a pin count,
  a linked-list pointer). Short-duration by design. ==No deadlock detection== — a latch acquire
  is supposed to be so fast that you just avoid deadlock structurally (global ordering) instead
  of detecting and breaking it after the fact.
- Rule of thumb: if it could plausibly be held while waiting on a disk I/O or another
  transaction, it's a lock. If it's only ever held across a few instructions of pointer/counter
  manipulation, it's a latch.

## Why not just one mutex protecting everything
- An OS mutex (`std::mutex`, ultimately a futex) is nearly free when uncontended, but on
  contention the losing thread **sleeps** and is woken by the kernel in scheduler order — with
  zero awareness of whether the critical section it's waiting on is 3 instructions or 3ms.
- If one lock protects things with very different hold times (e.g. "check a pin count" vs "scan
  a page"), the cheap operation gets stuck behind the expensive one purely because they share a
  lock. This pile-up is called a **convoy**.
- Fix: don't use one lock per shared structure — split into *multiple independent mechanisms,
  each sized to what it actually protects*, so a fast path never queues behind a slow one just
  because they happen to touch the same object.
- ==This is the actual reason commercial engines (Postgres, InnoDB) have such elaborate
  locking hierarchies — it's not over-engineering, it's convoy avoidance at scale.==

## Latch implementation flavors
- **Spinlock**: busy-wait loop, no syscall, no context switch. Cheap only if the hold time is
  shorter than a context-switch would cost — otherwise it just burns CPU while achieving
  nothing. Never spin across anything that can block (syscalls, I/O, another lock acquire).
- **OS mutex / futex**: sleeps on contention, cheap uncontended, but pays a syscall + context
  switch + scheduler-order wakeup on contention. Good for hold times that are unpredictable or
  can be long.
- **Hybrid spin-then-block** (Postgres `LWLock` style): spin briefly first (covers the common
  case where the holder releases almost immediately), fall back to a real block if that fails.
  Gets uncontended-cheap *and* avoids CPU-burning under real contention. This is what kernSQL's
  frame content latch uses instead of a raw `pthread_rwlock_t` — using a hand-rolled wrapper
  buys control over the exact wait/wake path.

## Reader/Writer latches
- Shared (read) mode: many readers concurrently. Exclusive (write) mode: one writer, no
  readers. Standard trade-off — read-heavy workloads (most page accesses are reads) benefit a
  lot from not serializing readers against each other.
- Can be legitimately held for a *relatively* long span (e.g. across a whole page scan) as
  long as it's a **content** latch and not a metadata latch — this is why kernSQL splits these
  into two separate primitives per frame rather than one (see Buffer Pool Manager notes).

## Condition variables need a real mutex
- An atomic word (flag + spin) cannot implement wait/notify — there's no way for a thread to
  "sleep until notified" on a bare atomic; it can only poll, which burns CPU.
- `std::condition_variable` requires pairing with an actual `std::mutex` to avoid the
  classic missed-wakeup race (check condition → mutex unlocked → notify happens → then you
  wait forever). This is the concrete reason a "just use an atomic" scheme for coordinating
  "is this resource ready yet" waiters doesn't work once you actually need blocking waiters,
  not just spinners.

## Deadlock avoidance for latches (lock ordering)
- Because latches have no deadlock detection, the only way to prevent deadlock once a thread
  might hold more than one at a time is a **canonical global acquisition order** — every thread,
  every code path, always acquires latches in the same relative order.
- If two code paths acquire the same two latches in *opposite* order, that's a deadlock waiting
  to happen the instant they interleave — doesn't matter how rare, it will eventually fire under
  load. ==This is exactly the kind of bug that's invisible in single-threaded testing and shows
  up as a production hang under concurrency.==
- Corollary: any operation that legitimately needs to hold two latches of the *same kind*
  simultaneously (e.g. two different page latches) must pick a deterministic order between them
  (e.g. by numeric id), never an order that depends on caller/argument order.

## Sharding as a concurrency technique
- Splitting one global structure (e.g. a `page_id → frame_id` map) into `N` independently-locked
  shards means lookups for unrelated keys never serialize against each other — contention drops
  roughly by a factor of `N` for uniformly-distributed keys.
- Shard count is usually a small fixed power of two rather than derived from core count at
  runtime — simpler, no CPU-detection dependency, and easy to bump later if profiling shows
  contention.
- Hash function only needs to be as strong as the key distribution requires — sequential integer
  keys (like auto-incrementing page ids) already spread evenly across shards with a plain
  bitmask (`key & (N-1)`); a fancier hash (multiplicative, Fibonacci) is solving a problem that
  doesn't exist for that access pattern.
- Cost: any operation that needs to move an entry *between* shards (e.g. changing what a key
  maps to when the shard boundary matters) has to acquire multiple shards, in ascending order,
  and re-validate state after acquiring the second one — someone else could have changed things
  while only the first shard lock was held.

See also: [[Buffer-Pool-Manager]] for how kernSQL actually applies all of this.

# Threading & Execution Model

**Status:** Accepted
**Component:** engine-wide — constrains every layer from the shell down to `src/storage`

## Context

A statement descends shell → concurrency control (MVCC) → planning → execution → `BufferPoolManager`
([DD-002](./DD-002-buffer-pool-manager.md)) → `DiskManager` ([DD-001](./DD-001-storage-file-layout.md)).
This doc fixes **which thread runs each of those layers**, because the answer constrains every layer
above storage and is expensive to change once the MVCC layer starts making assumptions.

## The rule everything follows

> **One thread carries a statement down the entire stack and back. Layers are a code decomposition,
> never a scheduling decomposition.**

## Thread-per-session

One OS thread per client connection, owned by that connection for its lifetime:

```
accept() -> spawn thread -> loop { read SQL; parse; plan; execute; send results } -> disconnect -> exit
```

The thread *is* the session's execution context. Its call stack holds the statement's state; session
state — current transaction, prepared statements, session settings — lives with it. 200 connected
clients means 200 threads, most blocked in `read()`.

This is Postgres (process-per-connection), MySQL/InnoDB (thread-per-connection), and SQLite
(caller's thread). It is already what the README commits to.

### Why not a thread pool per layer

The staged/SEDA shape — an MVCC pool handing off to a buffer-pool pool — was tried in the DB
literature (StagedDB, Harizopoulos & Ailamaki) and abandoned. Threads should be sized to **hardware
parallelism** and switched at **blocking points**, not at architectural boundaries.

A handoff between layers gains no parallelism: the caller has nothing to do but wait for the callee.
It costs a queue push/pop, a context switch, and the request's entire L1/L2 working set. A
`FetchPageRead` **hit** is on the order of 100ns of pointer chasing; a thread handoff is a few
microseconds. Staging would make the common case ~20x slower to solve a problem we do not have.

The same reasoning rejects an I/O thread pool under `DiskManager`: offloading a blocking `pread` and
then blocking on the result is strictly worse than blocking directly. That trade only pays when the
caller has other work, which under thread-per-session it does not.

### Consequences already baked in

DD-002 is synchronous and blocking by construction — `cv_.wait` on `Loading`, `shared_mutex`
content latches, blocking `pread`, and `FetchPageRead` returning a `Result<ReadPageGuard>` by value
rather than a future. **The thread that calls into the buffer pool must be able to park in the
kernel.** That is a deliberate choice, not an oversight.

## Threads vs. transactions

> **Concurrency control exists because transactions overlap, not because threads overlap.**

The two are unrelated, and the 1:1 correspondence in this model — one thread runs one transaction at
a time — is coincidental, not causal. One thread interleaving statements from two open transactions
still needs MVCC; a thousand threads with only one active transaction would need none.

| | Latch | Lock / MVCC |
|---|---|---|
| Protects | a physical in-memory structure | logical database state |
| Held for | ns–µs | the whole transaction, possibly minutes |
| Scope | one page, one structure | many pages, many statements |
| On conflict | wait | wait, or read an older version |
| Deadlock | avoided by fixed ordering | detected, resolved by rollback |

`Frame::mtx_` and `Frame::latch_` are latches. They make *reading the bytes* safe. They provide no
isolation whatsoever:

- A `WritePageGuard` releases its latch when the statement ends — long before `COMMIT`. In that gap
  another session's `FetchPageRead` legally reads uncommitted data. Every mutex was respected;
  ThreadSanitizer is silent; the read is still wrong.
- A scan touching 10,000 pages needs them **as of one instant**, or a concurrent transfer is counted
  twice. Each page read is individually correct. Snapshot isolation is a guarantee across pages and
  across time; a latch is a guarantee about one structure at one instant, and no quantity of
  latching composes into the former.

**Layering:** `BufferPoolManager` has no notion of a transaction and must keep it that way. It hands
out page bytes; it has no opinion on which tuples inside them a caller may see. Every tuple access is
two questions at two layers — the guard makes reading the bytes safe, the visibility check against
the caller's snapshot decides whether the tuple is theirs to see.

## Disciplines (these keep the model swappable)

The migration path, if concurrency ever demands it, is to replace *one thread per session* with *one
task per query on a fixed pool sized to cores*. That works only if the layers below were never
thread-aware. Three rules, enforced from now:

1. **No `thread_local` engine state.** Transaction context, resource ownership, and snapshots are
   passed explicitly down the stack. A `thread_local` current-transaction pointer welds the engine to
   thread-per-session permanently — this is exactly what makes Postgres's `MyProc` /
   `CurrentResourceOwner` globals so hard to move off processes.
2. **A page guard never outlives the function that acquired it.** Short, stack-scoped, released
   before returning up a layer. A guard held across an unbounded call is a latch held across an
   unbounded call.
3. **A pin never spans a client round-trip.** An open cursor holding pinned pages while the client
   decides whether to fetch more rows is pool exhaustion by a slow network. Materialize a batch or
   re-fetch on resume.

## Where separate threads do belong

Not per layer — per **background activity with a different cadence**. These are off the request path,
so they are not handoffs:

- **WAL writer / group commit.** Not optional once a log exists: batching many transactions' fsyncs
  into one fundamentally requires a thread that is none of them.
- **Checkpointer.**
- **MVCC garbage collection** — reclaiming versions no snapshot can see. MVCC without GC is a
  slow-motion disk leak.
- **Background page cleaner.** A DD-002 non-goal today; the first candidate once write throughput
  matters, since a foreground miss stalling on a dirty victim's writeback is the obvious stall.

## Shutdown

Shutdown is a phase the owner drives, not an operation workers participate in — a worker cannot know
it is the last one, and a flush failure is a decision (exit code, clean-shutdown marker) that no
worker can make or propagate.

1. **Stop accepting work.** Close the request queue; workers finish their current unit and exit.
2. **Join every worker.** This is what produces the quiescence `Shutdown()` requires: guards are
   stack-scoped, so an unwound stack has released every pin, and `join()` supplies the
   synchronizes-with edge.
3. **`BufferPoolManager::Shutdown()`**, then `DiskManager::Sync()`, single-threaded, checked.
4. Destroy.

Postgres does exactly this — backends never checkpoint on exit; the postmaster reaps every child and
only then runs the shutdown checkpoint. InnoDB's phased `srv_shutdown` is the same shape.

**Stopping work is not the pool's business.** A `stopping_` flag inside `BufferPoolManager` that
`FetchPage` checks would put an atomic load and a branch on the hottest path in the engine to handle
a condition that occurs once per process lifetime.

## Non-goals

- Async/coroutine execution. The cost is not plumbing, it is guard lifetime: a coroutine suspended
  while holding a pinned, latched page can resume after an arbitrary delay, and two such tasks
  pinning pages in different orders is a deadlock this design has no detector for. Under
  thread-per-session, latch hold times are bounded by function duration; under async, by the
  scheduler. Revisit only with a measured reason.
- Intra-query parallelism (parallel scan/join, DuckDB-style morsel scheduling). A separate axis from
  the connection model. Deferred, but not precluded: it requires only that the buffer pool be safe
  under concurrent access from several threads on behalf of one transaction, which DD-002 already is
  — nothing there assumes a pin's owner is a particular thread.
- Scaling to very high connection counts. That is a connection-pooler problem (pgbouncer), not an
  engine-architecture problem.
- Thread-per-core / shared-nothing partitioning (ScyllaDB, Umbra).

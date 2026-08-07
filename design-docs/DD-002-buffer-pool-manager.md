# Buffer Pool Manager: Concurrency & Latching

**Status:** Accepted
**Component:** `src/buffer` (`BufferPoolManager`)

Caches a fixed number of `PAGE_SIZE` pages from `DiskManager` ([DD-001](./DD-001-storage-file-layout.md))
as **frames**, and is what the catalog, heap and index use instead of `DiskManager` directly.
`DiskManager`'s `ReadPage`/`WritePage` are safe to call concurrently but provide no exclusion
between two callers on the *same* page; that is this layer's job.

## The rule everything follows

> **No lock is held across an I/O operation, and a frame's identity — which page it holds —
> changes only inside a single critical section that performs no I/O.**

## Frame lifecycle

One `enum class FrameState` per frame, under the frame's metadata mutex.

| State      | Mapped | Contents    | Pin count | In replacer          |
|------------|--------|-------------|-----------|----------------------|
| `Free`     | no     | meaningless | 0         | no (on free list)    |
| `Loading`  | yes    | meaningless | ≥ 1       | no                   |
| `Resident` | yes    | valid       | ≥ 0       | iff `pin_count == 0` |
| `Failed`   | no     | meaningless | ≥ 1       | no                   |

| Transition          | Owner                                                          |
|---------------------|----------------------------------------------------------------|
| `Free → Loading`    | a fetch miss, under the new page's shard lock                   |
| `Loading → Resident`| the loader, on a successful read; notifies waiters              |
| `Loading → Failed`  | the loader, on a read error; erases the mapping first           |
| `Failed → Free`     | the loader only, once waiters have dropped their pins           |
| `Free → Resident`   | `NewPage` — nothing on disk to read, so no `Loading` phase      |
| `Resident → Free`   | reclaim or `DeletePage`; requires unpinned and clean            |

There is no `Reclaiming` state: reclaim performs no I/O, so it is one critical section and nothing
can need to cancel it.

## Frame fields

`page_id`, `state`, `pin_count`, the inline `PAGE_SIZE` buffer, `dirty_epoch`/`flushed_epoch`, a
metadata mutex + condition variable, and a content latch.

The pool is one contiguous allocation made once at construction — a frame owns a mutex and condvar,
so it is neither copyable nor movable.

Not stored: `frame_id` (it is the array index), `page_lsn` (a WAL concern), `usage_count` (the
replacer's).

## Locks

1. **Content latch** (per frame, reader/writer) — the page bytes. Held as long as a caller's access
   needs. `std::shared_mutex` behind `Frame::AcquireRead`/`AcquireWrite`, which return `auto`-bound
   locks so the type stays swappable.
2. **Metadata mutex** (per frame) — `state`, `pin_count`, `page_id`. Must be a real mutex, not an
   atomic word: `Loading` needs a condition variable to wait on.
3. **Page-table shard locks** — the `page_id → frame_id` map split into 16 shards, keyed
   `page_id & (N-1)`. Page ids are sequential, so low bits already spread evenly.
4. **Free-list lock** — one mutex, one push or pop.
5. **Replacer lock** — clock hand and candidate set only. `usage_count` is `std::atomic<uint8_t>[]`
   outside it, bumped by a capped relaxed CAS, so the per-access path is lock-free.

I/O coordination is not a lock: it is the `Loading` state plus the frame's condvar.

## The pin invariant

> **No thread may read, write, wait on, or flush a frame's contents without holding a pin. Every
> lock here is momentary; the pin is what spans.**

- A miss waiter **pins before** it sleeps on `Loading`, so `page_id` is provably unchanged across
  the wait — no re-validation, no restart.
- A flush **pins the frame it flushes**, or a concurrent miss repurposes it mid-writeback.
- A reclaimer flushing a dirty victim **pins across the writeback** and re-validates after.

## Dirtiness

Two monotonic `std::atomic<uint64_t>` counters outside every lock. `dirty_epoch` is bumped by each
`WritePageGuard` release; `flushed_epoch` is raised by each completed flush. Dirty iff
`dirty_epoch > flushed_epoch`. This keeps the content latch and metadata mutex from ever being held
together.

**Flush sequence**, used by every flush path:

1. Acquire the shared content latch.
2. Sample `D = dirty_epoch`.
3. `DiskManager::WritePage`.
4. Release the content latch.
5. Raise `flushed_epoch` to `D` (CAS upward only).

A writer that dirties after step 4 bumps past `D`, so the frame stays dirty; one cannot dirty
between 1 and 4, which needs the exclusive latch. Never falsely clean.

## Free list and replacer

- **Free list** — frame_ids in state `Free`. A LIFO stack behind a mutex, fully populated at
  construction. No condvar, no exhausted flag, no watermarks; an empty list sends the caller to the
  replacer.
- **Replacer** — frames in `Resident` with `pin_count == 0`. `RecordAccess` bumps a capped
  `usage_count`; `SetEvictable` adds/removes a candidate; `Evict()` sweeps and returns the first
  candidate at zero, `nullopt` if none are evictable.

Frozen contracts:

- `SetEvictable` is **idempotent** — it adjusts the evictable count only when the flag actually
  flips.
- `Evict` **removes its victim from the candidate set itself** and resets its `usage_count`, under
  the replacer lock.
- A frame `Evict` returns that the caller declines to reclaim is **normal, not an error**; it stays
  out of the candidate set until its next unpin.

**Clock parameters:** `usage_count` caps at 3. One global clock hand. `Evict` is bounded by
construction — `nullopt` immediately when nothing is evictable, otherwise at most
`(cap + 1) × capacity` positions, after which it takes the first evictable frame regardless of
count.

## Reclaiming a frame

A thread that needs a frame and finds the free list empty reclaims one inline, holding **no other
lock** on entry.

1. `Evict()` → victim, already out of the candidate set. `nullopt` → return `BufferPoolFull`.
2. **Clean victim.** Take the victim's shard lock, then its metadata mutex. Re-validate:
   `Resident`, `pin_count == 0`, `!IsDirty()`, mapping still points here. If so — erase the
   mapping, set `page_id = INVALID_PAGE`, `state = Free`, reset epochs, release both. The frame is
   now private to this thread. **No I/O in this section.**
3. **Any check fails** — drop both locks, back to step 1 for another victim. Do not return it to
   the replacer; whoever pinned it owes a `SetEvictable(true)` on release.
4. **Dirty victim.** Under the metadata mutex verify `Resident` and `pin_count == 0`, take a pin,
   note `page_id`, release everything, run the flush sequence, then retry from step 2 with
   `pin_count == 1` (our own) as the condition. Drop the pin either way.

Step 4 spans I/O and is safe because identity never changes during the flush: the mapping stays
live, so a concurrent fetcher gets a cache hit and pins, and the reclaimer sees `pin_count > 1` at
re-validation and moves on.

The clean-check in step 2 is exact, not conservative: nothing can newly dirty the frame (that needs
a guard, which needs the page table we hold the shard lock for), and nothing already dirtied is
missed (a guard's release bumps `dirty_epoch` strictly before decrementing `pin_count`).

## Lock ordering

> **The only two locks ever held at once are a page-table shard lock and a frame metadata mutex, in
> that order. Enforce it as: never acquire a shard lock while holding a frame metadata mutex.**

Everything else is a leaf. The replacer lock is taken and dropped inside `Evict()`. The free-list
lock covers one push or pop. Content latches are taken only after both others are released. No path
needs two shard locks.

Standing rules:

- **Never block on I/O or a condvar while holding a shard lock.** Both waits here — a miss waiter on
  `Loading`, the loader on the failed-load path — drop it first.
- **Never call into the replacer or free list while holding a frame's metadata mutex.** Note the
  0→1 or 1→0 transition under the mutex, release it, *then* call `RecordAccess`/`SetEvictable`.
  This admits a benign race (a frame briefly evictable while about to be re-pinned), harmless
  because reclaim re-validates `pin_count == 0` before repurposing.
- Any operation needing two content latches takes them in a globally fixed order, not a
  caller-dependent one. State the order where that operation is designed.

## Page guards

RAII guards, never raw frames.

```
Result<ReadPageGuard>  FetchPageRead(page_id_t page_id);
Result<WritePageGuard> FetchPageWrite(page_id_t page_id);
Result<WritePageGuard> NewPage();
```

Latched from birth, in the mode the caller named. **No pin-only guard and no upgrade path**; a
future optimistic B+tree descent must drop the guard, re-fetch for write, and re-validate.

`ReadPageGuard` exposes `span<const byte, PAGE_SIZE>`; `WritePageGuard` also exposes a mutable
span. `PageId()` returns a value **stored in the guard**, never read back from `Frame::page_id` —
that field is under the metadata mutex.

**Release sequence (frozen):**

1. `WritePageGuard` only: bump `dirty_epoch`.
2. Release the content latch.
3. Metadata mutex; decrement `pin_count`; note a 1→0 transition.
4. Release the metadata mutex.
5. If it transitioned, `SetEvictable(frame_id, true)`.

Step 1 precedes 2 so a flusher sees an epoch covering every write it is about to capture, and
precedes 3 so a reclaimer's clean-check is exact. The frame cannot be reclaimed between 2 and 3 —
the pin, not the latch, protects identity. `WritePageGuard` bumps the epoch **unconditionally**.

**Value semantics:** non-copyable; movable, with the moved-from guard's manager pointer nulled and
move-assignment releasing before taking over. `Drop()` is explicit and idempotent; the destructor
calls it.

`UnpinPage` is **private**; the guards are friends. Guards live in `page_guard.{hpp,cpp}` with
`BufferPoolManager` forward-declared.

## Control flow

### FetchPage — hit

1. Shard lock for `page_id`; look up `frame_id`.
2. Metadata mutex. **Increment `pin_count` immediately**, before any wait. Note a 0→1 transition.
3. Release the shard lock. If `Loading`, wait on the condvar. If `Failed`, decrement the pin,
   notify, return the error.
4. Release the metadata mutex, then `RecordAccess`, and `SetEvictable(false)` if it went 0→1.
5. Acquire the content latch in the requested mode; return the guard.

### FetchPage — miss

**Release the shard lock first** — acquiring a frame may reclaim one, which needs the *victim's*
shard lock.

1. `free_list_.TryPop()`, else reclaim inline. `BufferPoolFull` propagates from there.
2. Re-acquire the shard lock and **look up again**. If present: release, push our frame back to the
   free list, restart from the hit path.
3. Still under the shard lock: insert the mapping, and under the metadata mutex stamp `page_id`,
   `Loading`, `pin_count = 1`, epochs zeroed. Release both. The mapping goes in before the read so
   a concurrent fetcher of the same page waits on `Loading` instead of racing.
4. `DiskManager::ReadPage`, under no lock.
5. Success: metadata mutex, `Resident`, notify all, release. Then `RecordAccess`, content latch,
   return the guard.
6. Failure: the failed-load path, then return the error.

### Failed load

The loader owns disposal.

1. Loader takes the shard lock, erases the mapping, releases it.
2. Metadata mutex: `state = Failed`, notify all.
3. Each waiter wakes, sees `Failed`, decrements its pin, notifies, returns the error. No disposal.
4. Loader waits until `pin_count == 1` (its own), then resets to `Free`, drops its pin, and pushes
   to the free list — not the replacer.

### NewPage / DeletePage / UnpinPage / FlushPage

- **NewPage** — get a frame as in miss step 1, then `DiskManager::AllocatePage` outside every lock.
  No `Loading` phase and no re-lookup race. Shard lock, insert mapping, stamp `Resident` and
  `pin_count = 1`, release, zero the buffer under the content latch, return a `WritePageGuard`.
- **DeletePage** — shard lock, then metadata mutex. Reject if `pin_count != 0`. Dirty contents are
  discarded without a flush. Set `INVALID_PAGE`, `Free`, reset epochs, erase the mapping, release
  both. Then `SetEvictable(false)`, `DiskManager::DeallocatePage`, push to the free list. Vacating
  before deallocating satisfies `DeallocatePage`'s not-resident contract.
- **UnpinPage** — private, guard-only. Decrement (guarding underflow); at zero mark evictable,
  after releasing the metadata mutex.
- **FlushPage / FlushAllPages** — pin first, run the flush sequence, unpin. `FlushAllPages` is a
  sequential loop, **best-effort, not atomic across frames**.

## Known behaviour

- `DeletePage` can spuriously fail while a reclaimer holds a transient pin across a dirty victim's
  writeback. It rejects rather than waiting, so deleting a page you still hold a guard on is an
  error rather than a deadlock.
- A reclaimer can flush a victim and then lose it to a fetcher that pins during the writeback.
  Wasted work, nothing corrupted.
- A miss that picks a dirty victim pays the write inline.

## Non-goals

- A background cleaner thread (bgwriter / page cleaner). Add it when measurement shows misses
  stalling on writeback.
- Packing `state` and `pin_count` into one CAS'd atomic word (LeanStore/Umbra style).
- Lock-free/RCU buffer pool; async I/O (io_uring); NUMA-aware placement.
- Read→write latch upgrade, and the optimistic B+tree descent that would want it.
- Sharding the free list or replacer; a per-shard clock hand.
- A spin-then-block content latch replacing `std::shared_mutex`.

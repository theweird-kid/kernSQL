# Buffer Pool Manager: Concurrency & Latching

**Status:** Accepted
**Component:** `src/storage` (`BufferPoolManager`)

## Context

`DiskManager` (see [DD-001](./DD-001-storage-file-layout.md)) owns raw page I/O and allocation
but is explicitly not thread-safe — its own header comments defer "concurrency control
(per-page latching)" to "one layer up." `BufferPoolManager` is that layer: it caches a fixed
number of `PAGE_SIZE` pages in memory as **frames**, and is the component everything above
(catalog, heap, index) actually talks to instead of touching `DiskManager` directly.

Target: throughput comparable to commercial engines (PostgreSQL/InnoDB-class), not the
textbook-simplest implementation, but also not bleeding-edge (lock-free/RCU, io_uring-driven
async I/O) — that level of complexity isn't justified for this goal.

## Why not a single mutex per frame

An OS mutex (`std::mutex`/futex) is cheap uncontended, but on contention the losing thread
sleeps until the kernel wakes it — with no awareness that the critical section it's waiting on
might be a handful of instructions. For latches (short-duration, protect physical in-memory
structures, no deadlock detection — as opposed to locks, which are long-duration and protect
logical/transactional state), that mismatch produces convoys: threads queue up and get woken
in scheduler order while the actual protected work is nearly free. A single latch per frame
also conflates two things with very different hold times — a pin-count check (nanoseconds)
and a page scan (potentially milliseconds) — so one blocks the other unnecessarily.

PostgreSQL's shared buffer design (see `src/backend/storage/buffer/README` upstream) solves
this by splitting responsibilities into independent mechanisms sized to what they protect,
rather than one lock per buffer. Consolidated below is kernSQL's version of that split.

## Frame structure

Each `Frame` owns:

- Its `PAGE_SIZE` content buffer, embedded inline rather than behind a pointer — the whole
  pool is one contiguous allocation of `capacity × sizeof(Frame)`, made once at
  `BufferPoolManager` construction. Better cache locality than one allocation per frame, and
  required anyway: once a frame owns a mutex and a condition variable (see below), it is
  neither copyable nor movable, so it has to live in storage that's sized once and never
  reallocated.
- `page_id_t page_id` — which page this frame currently holds (`INVALID_PAGE` if unused).
- `pin_count` — evictable only at zero.
- `is_dirty` — modified since last flush.
- `loading` — true while a disk read for this frame is in flight; distinct from `is_dirty`,
  gates whether the frame's content is safe to hand out yet.
- The frame's own metadata mutex and condition variable (see Synchronization primitives, #2).
- The frame's own content latch (see #1).

Deliberately **not** stored on `Frame`:

- `frame_id` — purely positional (the frame's index in the pool array). Redundant once frames
  live in a fixed contiguous array, and a stored copy risks drifting out of sync.
- `page_lsn` — a WAL-recovery concern; nothing reads or writes a WAL yet, so caching it here
  would be a field nothing uses. Revisit when recovery design starts.
- `usage_count` — clock-sweep bookkeeping belongs to the **Replacer** (see below), not the
  frame itself. This keeps `Frame`'s job narrowly "what page is this and how do I access it,"
  and lets the replacer be a self-contained component indexed by `frame_id`.

## Synchronization primitives

Six independent mechanisms, each sized to the critical section it protects:

1. **Frame content latch** (reader/writer, one per frame) — protects the actual `PAGE_SIZE`
   bytes. Shared mode for readers, exclusive for writers; can legitimately be held for the
   duration of a caller's access (e.g. across a scan). Implemented as a hybrid spin-then-block
   RW lock (à la Postgres's `LWLock`) rather than an OS `pthread_rwlock_t` directly, for control
   over the wait/wake path and cheap uncontended acquisition.

2. **Frame metadata mutex** (pin count, dirty flag, loading flag, current page_id) — a plain
   `std::mutex`, one per frame, deliberately separate from the content latch so a long content
   hold never blocks a quick "is this frame pinned?" check. This resolves what was an open
   question (spinlock vs. atomic word): the `loading` flag needs a real `condition_variable` to
   coordinate cache-miss waiters, and `condition_variable` requires a real mutex to pair with —
   an atomic word can't wait/notify. A per-frame mutex only ever contends when multiple threads
   hit that exact page simultaneously, a much narrower case than a shared bottleneck lock, so
   this stays consistent with the "comparable to commercial" target.

3. **Page table shard locks** — the `page_id → frame_id` map is split into `N` shards
   (`hash(page_id) % N`), each with its own lock, so lookups for unrelated pages never
   serialize against each other.

4. **I/O coordination** — not a lock: the frame's `loading` flag plus its condition variable
   (mechanism #2). The thread that misses first sets `loading`, issues the read, then clears it
   and notifies waiters. A spin-or-short-block latch must never be held across a blocking
   syscall — that's the one thing latches are specifically not for.

5. **Free list lock** — a plain mutex guarding the free-list stack itself (see below). Short
   critical section (push/pop one `frame_id`), touched only on the miss path.

6. **Replacer lock** — a plain mutex guarding the replacer's internal bookkeeping (clock hand
   position, per-frame `usage_count`, candidate membership). Also only touched on the miss path
   and on pin-count transitions, not on every access.

## Free list vs. replacer

Two separate structures track two different populations of frames:

- **Free list** — frame_ids that have never held a page, or were just vacated by
  `DeletePage`. A LIFO stack, fully populated with every index at construction. Popping is O(1)
  and touches nothing else — avoids running the clock sweep at all during warm-up, before the
  pool is full.
- **Replacer** — tracks eviction *candidates*: frames currently holding a page but unpinned. A
  frame enters this set the instant its pin count drops to zero, and leaves the instant it's
  pinned again. Interface: `RecordAccess(frame_id)` bumps `usage_count` (capped);
  `SetEvictable(frame_id, bool)` adds/removes a frame from the candidate set;
  `Evict() -> optional<frame_id>` runs the clock sweep, decrementing candidates' `usage_count`
  as it passes them and returning the first one found at zero. Returns `nullopt` if nothing is
  evictable (every resident frame pinned) — the trigger for `BufferPoolFull`.

The miss path checks the free list first, and only consults the replacer once it's empty.

**Clock-sweep parameters:** `usage_count` caps at **3**. It only needs to be large enough to
give a genuinely hot page a couple of extra sweeps of protection over a page touched once —
pushing the cap higher just slows down how fast eviction finds a victim under memory pressure,
without a corresponding hit-ratio benefit once a page's `usage_count` has already saturated.
The sweep uses one **global** clock hand, not one per page-table shard: the replacer is only
touched on the miss path and on pin-count transitions, not on every access (see Non-goals), so
it doesn't experience the per-access contention that motivated sharding the *page table*.
Revisit only if profiling shows the replacer lock itself as a bottleneck.

## Page table sharding

- **Shard count**: a small fixed power of two (e.g. 16) rather than derived from core count at
  runtime — simpler, no CPU-detection dependency, trivially bumped later if profiling shows
  contention.
- **Hash function**: a plain bitmask, `page_id & (N-1)`. `page_ids` are assigned sequentially
  by `AllocatePage` (plus freelist reuse on disk), and consecutive integers' low bits already
  cycle through every shard value evenly — no multiplicative/Fibonacci hashing needed for this
  access pattern.

## Lock ordering

Canonical global order, to prevent deadlock once a thread can hold more than one of the above
at a time:

**page-table shard lock → free-list lock / replacer lock → frame metadata mutex → frame
content latch.**

Two corollaries fall out of this:

- **Replacer/free-list calls must never be made while holding a frame's metadata mutex.**
  Increment/decrement `pin_count` under the metadata mutex, note the transition (0→1 or 1→0),
  release the metadata mutex, *then* call `RecordAccess`/`SetEvictable` if the transition
  requires it. This is a real correction from the initial control-flow sketch, which had Fetch
  and Unpin acquire the metadata mutex and then call into the replacer while still holding it —
  that's backwards relative to eviction, which acquires the replacer lock first and then wants
  the victim's metadata mutex. Two threads doing the mirror-image thing is a deadlock. The fix
  accepts a small benign race (a frame briefly marked evictable while about to be re-pinned,
  or vice versa) — harmless, because eviction re-validates `pin_count == 0` under the metadata
  mutex before actually reusing a frame.
- **Crossing two page-table shards** (eviction removing an old mapping while inserting a new
  one in a different shard) is handled by dropping the currently-held shard lock, acquiring
  both shards in ascending shard-index order, and re-checking that the target `page_id` is
  still absent before proceeding — another thread may have inserted it while the lock was
  released. If so, abandon the eviction attempt and fall back to the hit path.

Any future operation needing more than one frame's content latch simultaneously must also
acquire those in a fixed order (e.g. by `frame_id`), never in caller-dependent order.

## Control flow

### FetchPage(page_id) — hit path

1. Acquire the page-table shard lock for `page_id`; look up `frame_id`.
2. Acquire the frame's metadata mutex. If `loading`, release the shard lock and wait on the
   frame's condition variable until it clears — this is still a hit, just not yet ready.
3. Increment `pin_count`; note if it transitioned 0→1. Release the metadata mutex, then (per
   the ordering fix above) call `RecordAccess` and, if it transitioned, `SetEvictable(frame_id,
   false)`.
4. Release the shard lock, return the frame handle. The caller separately acquires the content
   latch (shared or exclusive) for what it intends to do.

### FetchPage — miss path

Still holding the shard lock:

1. Get a frame: pop the free list, or ask the replacer to `Evict()`. If neither yields one,
   release the shard lock and return `BufferPoolFull`.
2. If the victim holds another page (`old_page_id`) whose shard differs from the current one:
   drop the current shard lock, acquire both shards in ascending order, and re-check `page_id`
   is still absent (see Lock ordering).
3. If the victim is dirty, flush it — a shared content latch suffices, since `pin_count == 0`
   already implies no active writer.
4. Remove the old mapping, insert the new one (`page_id → frame_id`) *before* releasing any
   shard lock, so a concurrent fetcher of this same new page finds it and waits on `loading`
   instead of racing to fetch it independently.
5. Stamp frame metadata (`page_id`, `pin_count = 1`, `loading = true`, `is_dirty = false`).
   Release shard lock(s).
6. Perform `DiskManager::ReadPage` outside any lock. `loading = true` is what makes concurrent
   hit-path lookups wait instead of proceeding on stale content.
7. On completion: metadata mutex, `loading = false`, notify all waiters, release.

### NewPage / DeletePage / UnpinPage / FlushPage

- **NewPage** — same frame-acquisition path as a miss, but calls `DiskManager::AllocatePage`
  instead of reading; there's nothing on disk yet to read.
- **DeletePage** — rejects if `pin_count != 0`. Otherwise: acquire the replacer lock and remove
  the frame from the candidate set — it's in there, since `pin_count == 0` means the last
  `UnpinPage` already made it evictable — re-verifying `pin_count == 0` under the frame's
  metadata mutex while still holding the replacer lock, the same order eviction itself uses.
  This closes a race where a concurrent `FetchPage` miss's `Evict()` could otherwise select this
  exact frame as a victim at the same moment it's being deleted, handing two threads the same
  `frame_id`. Only then: `DiskManager::DeallocatePage`, remove the page-table entry, reset frame
  metadata, and return the frame to the **free list** (not back to the replacer — it holds no
  content worth tracking as an eviction candidate anymore).
- **UnpinPage** — decrement `pin_count` (guard against underflow); OR the caller's `is_dirty`
  into the frame's flag rather than overwrite it, since two concurrent pinners — one clean, one
  dirty — must leave the frame dirty. At `pin_count == 0`, mark the frame evictable (after
  releasing the metadata mutex, per the ordering fix).
- **FlushPage / FlushAllPages** — shared content latch (a flush is just another reader),
  `DiskManager::WritePage`, clear the dirty flag under the metadata mutex. `FlushAllPages` is a
  sequential loop over resident frames calling the same per-frame path — **best-effort, not
  atomic across frames**. Nothing above the buffer pool needs cross-frame flush atomicity yet:
  there's no WAL/checkpoint layer to coordinate with, and the crash-safety guarantee this
  codebase targets (surviving `kill -9`, not power loss — see [DD-001](./DD-001-storage-file-layout.md))
  doesn't demand it either. Revisit if/when a checkpoint protocol is designed.

## Non-goals (this iteration)

- Lock-free (RCU/hazard-pointer) buffer pool — not justified for "comparable to commercial,"
  which this design already achieves without it.
- Async I/O (io_uring) replacing the loading-flag/condvar path.
- NUMA-aware frame placement or partition affinity.
- Sharding the free list or replacer themselves — both are single global structures behind one
  lock each. Acceptable for now since they're only touched on the miss path and on pin-count
  transitions, not on every access; revisit only if measurement shows contention there.

# Heap Pages & Table Heap

**Status:** Accepted
**Component:** `src/storage` (`HeapPage`, `TableHeap`, `TableIterator`)

Where table rows actually live. A heap is an unordered pile of rows addressed by
`RID{page_id, slot}`; the B+tree will store RIDs as its payload, and the executor reads rows
through them. Sits on `BufferPoolManager` ([DD-002](./DD-002-buffer-pool-manager.md)) and
inherits the page format from [DD-001](./DD-001-storage-file-layout.md).

Deliberately shorter than DD-002. That doc is long because the buffer pool invents a latching
protocol; this layer invents none — it operates on bytes inside a latch someone else already
holds. The only sections here that carry real weight are the layout, the RID contract, and the
one place two threads can collide (page extension).

## The rule everything follows

> **Slots never move. Tuple bytes move freely.**

A `RID` names a slot, and the slot names an offset. That indirection is the entire reason the
layout exists: compaction can rewrite every byte in the tuple region and every RID in the
database stays valid, because the slot array did not move and the slot's *index* is what was
handed out. Postgres calls these line pointers (`ItemIdData`); the concept is universal.

The corollary is the thing to keep testing: **any operation that moves tuple bytes must update
the moved tuples' slot offsets in the same latched operation.** There is no window in which a
slot's offset is stale, because there is no window in which another thread can look.

## Page layout

Everything below is inside the 4064-byte body a page guard hands out. Offsets are
**body-relative** — offset 0 is the first byte after the 32-byte `PageHeader`. That is the only
origin any code in this layer uses, and it keeps every offset under 4064, comfortably inside a
`uint16_t`.

```
body offset 0
+---------------------------+
| heap sub-header (8 bytes) |
+---------------------------+
| slot[0] slot[1] ...       |  grows FORWARD, 4 bytes each
+---------------------------+
|                           |
|        free space         |  insert fails when this closes
|                           |
+---------------------------+
| ... tuple tuple tuple     |  grows BACKWARD from the end
+---------------------------+
body offset 4064
```

They grow toward each other because the slot-to-tuple ratio is not knowable at format time: one
page may hold three fat rows, another two hundred thin ones. A fixed-size slot array would have
to be sized for the worst case and waste the difference on every page.

### Heap sub-header (8 bytes)

| Field | Type | Purpose |
|---|---|---|
| `slot_count` | `uint16_t` | Number of entries in the slot array, live and dead alike. Only ever grows within a page's life as a heap page. |
| `tuple_data_start` | `uint16_t` | Body offset of the lowest occupied tuple byte. The tuple region is `[tuple_data_start, 4064)`. Initialised to 4064 for an empty page. |
| `live_count` | `uint16_t` | Non-dead slots. Maintained rather than derived so a scan can skip an empty page and stats do not cost a slot walk. |
| `reserved` | `uint16_t` | Zero. |

### Slot (4 bytes)

| Field | Type | Purpose |
|---|---|---|
| `offset` | `uint16_t` | Body offset of the tuple's first byte. **`0` means the slot is dead**, see below. |
| `length` | `uint16_t` | Tuple length in bytes. |

`offset == 0` is a safe sentinel rather than a magic number: body offset 0 is inside the heap
sub-header, so no tuple can ever legitimately start there. Postgres spends a flags bitfield on
the same distinction; the impossible offset gets it for free.

A slot is in one of two states — **live** (`offset != 0`) or **dead** (`offset == 0`). There is
no third "never used" state: a slot is created live by an insert, and a delete makes it dead.

### Free space

```
slot_array_end = HEAP_HEADER_SIZE + slot_count * SLOT_SIZE
free_space     = tuple_data_start - slot_array_end
```

An insert of `L` bytes needs `L + SLOT_SIZE` if it must append a slot, or `L` alone if it reuses
a dead one. Both are checked against `free_space` **after** deciding whether compaction is
needed, never before.

Largest possible tuple on an empty page: `4064 - 8 - 4 = 4052` bytes.

## The tuple size cap: 2000 bytes

A row must fit in one page — tuples do not span pages, and there are no overflow chains (see
Non-goals). The cap is **2000 bytes**, enforced twice: at `CREATE TABLE` against the maximum
possible row the schema can produce, and again at `INSERT` against the actual encoded size.

2000 rather than the 4052 ceiling because 2000 guarantees at least two rows per page
(`2 * (2000 + 4) + 8 = 4016 <= 4064`). That is InnoDB's rule, and the reason for it is that a
format allowing exactly one row per page degenerates: every page carries a full header and a
slot array to hold a single row, and a "heap" becomes a linked list with 50% overhead. Two rows
per page is the weakest guarantee that keeps the structure honest.

## Operations

All of these are on `HeapPage`, a **pure view over a `span<byte, PAGE_BODY_SIZE>`**. No buffer
pool, no disk, no guards, no ownership. That is what makes it testable against a bare
`std::array<std::byte, 4064>` in microseconds, which is where the offset arithmetic gets
debugged.

- **`Init`** — zero the sub-header, `slot_count = 0`, `live_count = 0`,
  `tuple_data_start = 4064`. The caller stamps `page_type = HEAP` through the guard.
- **`Insert(bytes) -> slot_id`** — reuse a dead slot if one exists, else append. Copy the tuple
  to `tuple_data_start - len` and lower `tuple_data_start`. Fails (does not compact) when there
  is not enough contiguous free space; the caller decides whether to compact and retry or move
  to another page.
- **`Get(slot) -> span`** — bounds-check the slot id, reject a dead slot. **A `Get` on a deleted
  RID returns "not found", never bytes.** This is why dead slots stay distinguishable instead of
  being removed: an RID handed out before the delete must get an answer, not garbage.
- **`Delete(slot)`** — mark the slot dead, decrement `live_count`. The tuple's bytes are *not*
  moved; the space is reclaimed at the next compaction.
- **`Compact()`** — rewrite the tuple region packed against the end of the body, in descending
  offset order, updating each live slot's offset as its tuple moves. Dead slots keep their
  index and stay dead. Afterwards `tuple_data_start` is the new low-water mark.
- **`Update(slot, bytes)`** — three cases, and the third is the one with consequences:
  1. new length `<=` old: overwrite in place, shrink `length`. The difference becomes waste,
     reclaimed at the next compaction.
  2. new length `>` old but the page has room after compaction: compact, then relocate the
     tuple. **The RID survives**, because the slot index did not change.
  3. it does not fit even after compaction: not `HeapPage`'s problem. `TableHeap` deletes here
     and inserts elsewhere, and **the RID changes**.

## The RID contract

Stated explicitly, because everything above this layer depends on it and case 3 above is a trap:

- An RID is stable across **insert, delete of other rows, compaction, and in-page update**.
- An RID is **invalidated by an update that outgrows its page**. `TableHeap::Update` returns the
  new RID; the caller owns re-pointing anything that referenced the old one.
- An RID of a deleted row resolves to "not found" **for the life of the page as a heap page**. It
  is not reused: a dead slot may be handed to a later insert, so a stale RID can, after enough
  churn, resolve to a *different live row*. This is the ABA problem, and it is accepted here
  because the only holder of long-lived RIDs will be the B+tree, which is updated in the same
  operation that deletes the row. Postgres avoids it with tuple visibility (`xmin`/`xmax`), which
  is MVCC machinery this engine deliberately does not have.

## Table heap

`TableHeap` owns the `BufferPoolManager&` and the page chain. Pages are linked through
`PageHeader::next_page_id`, per DD-001's chain invariant — a `HEAP` page's `next_page_id` is the
next page of that table, and the guard already exposes `SetNextPageId`.

The catalog holds each table's `first_page_id` and `last_page_id`.

### Insert and page extension — the one place two threads collide

Insert takes a write guard on the last page and tries. If it fits, done. If it does not, the page
must be extended, and two inserters arriving together must not both allocate.

> **The write latch on the current last page is the extension lock.**

A thread that finds the last page full **keeps its write guard** while it calls `NewPage`, stamps
`HEAP`, `Init`s the new page, sets the old page's `next_page_id`, and publishes the new
`last_page_id`. A second inserter blocks on that guard; when it acquires it, the page's
`next_page_id` is no longer `INVALID_PAGE`, so it follows the link instead of allocating. No new
lock, no new ordering rule — the latch that was already required does the job.

Getting this wrong is not subtle in its consequences and is very subtle in its symptoms: both
threads allocate, both set `next_page_id`, one link is overwritten, and one page is permanently
leaked and unreachable — a slow leak with no error anywhere.

Note this means a `NewPage` (which can block on a disk allocation) happens under a content
latch. That is allowed: DD-002's prohibitions are about the *metadata* mutex and shard locks.
A content latch is held across I/O routinely — that is what the `Loading` state exists for.

### Free space across pages

**Insert only ever appends to the last page.** Space freed by deletes in earlier pages is never
reused while those pages stay in the chain.

That is a real limitation, stated rather than hidden: a table with heavy delete churn grows
monotonically. It is the exact hole Postgres fills with the Free Space Map (a separate fork
holding a tree of per-page free-space bytes) and InnoDB with segment inodes and free/frag page
lists. Both are more machinery than a scoped engine needs, and both are meaningless without a
vacuum process to put pages back in circulation — which is itself out of scope. Appending gives
O(1) insert with no search; a chain walk looking for room would be O(pages) on every insert,
which is worse than the leak.

### Scanning

`TableIterator` holds **one page guard at a time** and copies tuples out. It must not hold a
guard across the whole scan: the pool is fixed-size, and a scan that pins a frame per page it
has visited exhausts it. Advance = release the current guard, fetch the next page id, skip pages
whose `live_count` is zero.

## Concurrency

This layer defines no locking of its own. Every `HeapPage` operation runs inside a
`WritePageGuard`, which already holds the frame's content latch exclusively plus a pin, so a
heap page is single-threaded by construction and its invariants can never be observed mid-update.

The only two cross-page concerns are page extension (above) and iteration, which takes and
releases one guard at a time and therefore sees a consistent page at each step but no consistent
snapshot of the table. Snapshot semantics are a transaction-layer concern and land with 2PL.

## Non-goals

- **Tuple encoding.** `HeapPage` stores opaque byte blobs. What a row *means* — column types,
  nulls, ordering — belongs to the catalog and executor. Keeping this seam is what lets the heap
  be built and finished before the schema format is settled.
- **Free space map / space reclamation.** See above.
- **Overflow pages / TOAST.** A row that does not fit a page is an error at `CREATE TABLE` or
  `INSERT`, not a chain of pages. Rejected because a multi-page insert has no atomicity without a
  WAL — a partial chain leaks pages with no undo — and the scoped SQL cannot produce such a row.
- **Page-level defragmentation on a background thread.** Compaction is inline, on the thread that
  needs the space.
- **Visibility / versioning.** No `xmin`/`xmax`. Deleted means gone, immediately, for everyone.

## Open questions

- Whether `live_count == 0` should return the page to the freelist rather than leaving an empty
  page in the chain. Cheap to do, but unlinking needs the previous page's latch, which is a
  second-latch ordering rule this layer otherwise does not have. Deferred until there is a
  workload that shows empty pages accumulating.
- Whether `TableIterator` should expose RIDs alongside tuples. The executor does not need them
  for `SELECT`, but `DELETE ... WHERE` will.

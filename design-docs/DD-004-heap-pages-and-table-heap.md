# Heap Pages & Table Heap

**Status:** Accepted
**Component:** `src/storage` (`HeapPage`, `TableHeap`, `TableIterator`)

Where table rows actually live. A heap is an unordered pile of rows addressed by
`RID{page_id, slot}`; the B+tree will store RIDs as its payload, and the executor reads rows
through them. Sits on `BufferPoolManager` ([DD-002](./DD-002-buffer-pool-manager.md)) and
inherits the page format from [DD-001](./DD-001-storage-file-layout.md).

Deliberately shorter than DD-002. That doc is long because the buffer pool invents a latching
protocol; this layer invents none — it operates on bytes inside a latch someone else already
holds. The only sections here that carry real weight are the layout, the RID contract, the one
place two threads can collide (page extension), and the rule that stops a scan from fighting the
writes it triggers.

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
| `dead_bytes` | `uint16_t` | Bytes in the tuple region that no live slot points at: deleted tuples, plus the waste left behind by an in-place shrinking update. Reset to zero by `Compact`, and reduced by nothing else. InnoDB's `PAGE_GARBAGE`. |

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

Two different numbers, and confusing them is the bug this section exists to prevent.

```
slot_array_end = HEAP_HEADER_SIZE + slot_count * SLOT_SIZE
contiguous     = tuple_data_start - slot_array_end          // usable right now
reclaimable    = contiguous + dead_bytes                    // usable after Compact
```

An insert of `L` bytes needs `L + SLOT_SIZE` if it must append a slot, or `L` alone if it reuses
a dead one. `HeapPage::Insert` checks that against `contiguous` and fails if it does not fit; the
caller compares the same requirement against `reclaimable` to decide whether compacting is worth
a retry.

`reclaimable` is also the number any cross-page free-space mechanism has to publish, which is the
reason `dead_bytes` is a maintained field rather than something derived on demand. It cannot be
derived: `Update` case 1 overwrites a slot's `length` with the smaller value, so the bytes it
orphans are referenced by no slot and no offset and are invisible to any later slot walk. Without
the counter, a page that has taken many in-place shrinks looks full while being mostly holes, and
nothing ever learns there is space to recover.

Largest possible tuple on an empty page: `4064 - 8 - 4 = 4052` bytes.

### The accounting invariant

Every byte of the tuple region belongs to exactly one live tuple or to nobody, which fixes a
total that must hold after **every** operation on the page:

```
tuple_data_start + sum(slot.length for live slots) + dead_bytes == PAGE_BODY_SIZE
```

This is worth a debug-only `Validate()` alongside `live_count == count of live slots` and
`slot_array_end <= tuple_data_start`, called at the end of every mutating operation in test
builds. It catches the entire class of accounting bug this layer can have — a missed increment in
`Update` case 1, a `dead_bytes` wrongly reduced by a dead-slot reuse, a `Compact` that forgets to
reset — at the operation that caused it rather than a thousand operations later.

`dead_bytes` is bounded by 4052 and therefore cannot overflow its `uint16_t`, and no operation
ever subtracts from it, so there is no path that can drive it negative. Because it lives in the
same page as the bytes it describes, and a page is the unit of atomicity, it also cannot disagree
with its own page across a crash — which is what allows it to be exact, unlike the cross-page
structures described under [Free space across pages](#free-space-across-pages).

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

- **`Init`** — zero the sub-header, `slot_count = 0`, `live_count = 0`, `dead_bytes = 0`,
  `tuple_data_start = 4064`. The caller stamps `page_type = HEAP` through the guard.
- **`Insert(bytes) -> slot_id`** — reuse a dead slot if one exists, else append. Copy the tuple
  to `tuple_data_start - len` and lower `tuple_data_start`. Fails (does not compact) when there
  is not enough contiguous free space; the caller decides whether to compact and retry or move
  to another page. **Reusing a dead slot does not reduce `dead_bytes`.** It reclaims the four
  bytes of the slot, not the tuple bytes: the dead tuple's offset was destroyed by `Delete`, so
  its bytes cannot be found, let alone reused. Only `Compact` reclaims them. This reads
  backwards and is the one place the field is likely to be got wrong.
- **`Get(slot) -> span`** — bounds-check the slot id, reject a dead slot. **A `Get` on a deleted
  RID returns "not found", never bytes.** This is why dead slots stay distinguishable instead of
  being removed: an RID handed out before the delete must get an answer, not garbage.
- **`Delete(slot)`** — add the slot's `length` to `dead_bytes` **before** overwriting the slot,
  then set it to `{offset: 0, length: 0}` and decrement `live_count`. Zeroing both fields gives a
  dead slot one canonical representation, which is what makes `offset == 0 => length == 0`
  assertable. The tuple's bytes are *not* moved; the space is reclaimed at the next compaction.
- **`Compact()`** — rewrite the tuple region packed against the end of the body, in descending
  offset order, updating each live slot's offset as its tuple moves. Dead slots keep their
  index and stay dead. Afterwards `tuple_data_start` is the new low-water mark and
  `dead_bytes = 0` — by definition, since every byte below the new low-water mark is now live.
  This is the only operation that reduces `dead_bytes`.
- **`Update(slot, bytes)`** — three cases, and the third is the one with consequences:
  1. new length `<=` old: overwrite in place, shrink `length`, and add the difference to
     `dead_bytes`. Those orphaned bytes are the ones no slot walk can ever find again, which is
     the whole reason the counter exists.
  2. new length `>` old but the page has room after compaction: compact, then relocate the
     tuple. **The RID survives**, because the slot index did not change. Sequence this as
     *kill the old tuple, compact, insert into the same slot id* — add the old length to
     `dead_bytes` and zero the slot first, so compaction drops the old bytes instead of copying
     bytes that are about to be abandoned. The slot is momentarily dead while still logically
     live; that is safe only because the whole operation runs under one write latch, and it is
     the reason `Compact` must be a private helper rather than something a caller can interleave.
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

Insert takes a write guard on a candidate page and tries. If it fits, done. When every candidate
is full — including the last page, which is where the insert-hint cursor below always falls back
to — the chain must be extended, and two inserters arriving together must not both allocate.

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

Space freed by a delete is reclaimed *within* its page by the next compaction. Getting it reused
*across* pages is a separate problem, and this is the one part of the layer that is deliberately
unfinished.

**The bound, stated exactly.** With a pure append-at-the-last-page insert, a table's file size
tracks the **peak** number of rows it has ever held, not the number it holds now. The
pathological workload is not exotic: any table at a steady state of churn — a queue, a session
table, a nightly load-and-drain staging table — holds a constant row count while growing without
limit.

The cost that actually decides this is not disk, which is cheap. It is `TableIterator`. Skipping
a page whose `live_count` is zero skips the tuple work but still fetches the page and still
evicts something useful to do it, so a table that peaked at a million rows and now holds a
thousand pays for a million-row scan forever and flushes the pool every time it runs.

**What rules out the textbook fix.** The obvious mechanism is an intrusive free-page list — a
head in the catalog, links threaded through a page header field, O(1) push on delete and O(1) pop
on insert. That is InnoDB's free/frag list, and it is the wrong shape for this engine, because a
linked list is an *exact* structure: every node must be correct or the structure is broken.
KernSQL has no WAL. A crash leaves dirty pages unflushed in arbitrary order, so a head that
points at a page whose link never reached disk is a dangling pointer into a free page or another
table, with no undo to repair it and nothing to detect it. InnoDB can afford exact free lists
because it has redo logging; Postgres, whose FSM is explicitly allowed to be wrong and is rebuilt
by vacuum, is the better model here for the opposite reason. Note this is unrelated to
visibility: with no `xmin`/`xmax` this engine knows space is free the moment a delete commits,
so it never needs vacuum's hard part — deciding *when* space became free.

> **The rule for anything that spans pages: it is a hint, never a truth.** Every entry is
> re-verified against the real page before use, so a stale entry costs one wasted fetch rather
> than corruption. That single property is what makes a cross-page structure crash-safe without a
> WAL, and — because staleness is harmless — it also removes any reason to hold two page latches
> at once, which is what keeps this layer free of a lock-ordering rule.

**v1 — a rotating insert cursor.** `TableHeap` keeps an in-memory `insert_hint_page_id`, set to
`first_page_id` on open. Insert latches the hint page and tries; on failure it advances the hint
along `next_page_id` (wrapping at the end) for a bounded K attempts, then falls back to appending
at the last page. Delete is untouched, nothing is persisted, no latch is added, and a restart
merely starts the sweep again. Because the cursor traverses the whole chain over the table's
life, any page that frees up is eventually revisited, and a churning table reaches a stable size.
Its honest weakness is that it stumbles into space rather than finding it: deletes scattered
sparsely across a large table mean the cursor mostly lands on full pages and burns its K probes.

**v2 — a per-table free-space map, when the cursor stops being enough.** A chained page per
table, `fsm_first_page_id` in the catalog, holding an array of `{page_id: uint32,
free_units: uint16}` — `reclaimable` quantised to 16-byte units, which fits 0–254 and answers
"at least N bytes?" directly for any N, with no size-class buckets. At 6 bytes an entry that is
~670 pages, about 2.7 MB of table, per FSM page, and finding a candidate is a linear scan of one
cached page. Postgres needs a *tree* only because its array is large enough that scanning it
matters; at this scale it is not. It is an array of pairs rather than an array indexed by page
number because pages come from a global freelist and are scattered, so this table has no dense
block numbering to index by.

Two rules make it safe. The FSM is written **after** the heap page's guard is released, since
staleness is harmless and there is therefore no reason to hold both. And it is **insert-only**:
`TableIterator` keeps walking `next_page_id`, because a scan that trusted a structure allowed to
be wrong would silently drop rows from query results. It can be rebuilt at any time by walking
the chain, which is worth having as an admin operation regardless.

Neither tier needs an on-disk format change beyond `dead_bytes`, which is already in the
sub-header, and one `page_id_t` in the catalog — which is why v1 ships now and v2 is written down
rather than built.

### Scanning

`TableIterator` holds **one page guard at a time** and copies tuples out. It must not hold a
guard across the whole scan: the pool is fixed-size, and a scan that pins a frame per page it
has visited exhausts it. Advance = release the current guard, fetch the next page id, skip pages
whose `live_count` is zero.

**It yields the RID alongside the tuple, always** — not behind a flag, a mode, or a second
iterator type. The iterator already holds the page id in order to walk the chain and the slot id
as its cursor, so the RID is not something it fetches; it is something it would otherwise throw
away. `SELECT` ignores it. `DELETE ... WHERE` needs it to name the row it matched, and the B+tree
build needs `{key -> RID}` pairs for every row in the table — that pair *is* the index's payload.
A scan that cannot say *which* row it handed back is unusable to the layer above. Postgres does
the same thing unconditionally: a seqscan's `HeapTuple` carries `t_self`, and no caller opts in.

**The RID is a value; the tuple bytes are not.** A `RID` copied out of the iterator stays
meaningful after the iterator advances. A span into the page body does not — advancing drops the
guard, and the frame may then be evicted and refilled with a different page, which reads as valid
memory holding another table's bytes. That is the `ConstHeapPage` lifetime rule at the one place
it is easiest to reintroduce, so the iterator copies tuple bytes into caller-owned storage rather
than handing out a view whose validity silently ends at the next advance.

### Mutating during a scan

`DELETE ... WHERE` and `UPDATE ... WHERE` read rows through a scan and then write them. How they
are planned belongs to the executor, but this layer constrains the shape of it hard enough that
the constraint is recorded here.

Two facts rule out the obvious "match a row and write it where it sits":

- **There is no read-to-write latch upgrade.** [DD-002](./DD-002-buffer-pool-manager.md) states
  it: a shared-to-unique upgrade has to drop the shared latch first. So a mutation cannot be
  performed from inside the iterator's read guard — the guard must be dropped and the page
  re-fetched for write. That window is survivable precisely because of the rule at the top of
  this doc: a compaction in between may have rewritten every offset in the page, and the RID
  still resolves, because the slot index did not move. The row itself may be gone, which `Get`
  already answers as "not found".
- **A relocating update can be seen twice.** `Update` case 3 moves a row that outgrew its page to
  somewhere else in the chain — possibly a page the scan has not reached yet. The scan arrives
  there, the row still matches the predicate, and it is updated again. This is the Halloween
  problem. Postgres cannot hit it because a tuple written by the current command is invisible to
  that command (`cmin`/`cmax`); this engine has no visibility machinery to borrow, so nothing
  stops the loop on its own.

> **Collect first, then mutate.** The scan runs to completion gathering RIDs, its guard drops,
> and only then does each mutation take its own write guard.

Both problems die to that single rule. No latch upgrade is ever needed, because nothing is
latched during the apply phase. The Halloween case is impossible, because the set of RIDs was
frozen before the first write. The price is memory proportional to the number of matched rows,
which is affordable at this engine's scope and is the reason a rule this blunt is enough.

Each RID is re-verified against the real page as it is applied, and a row that has vanished is
skipped rather than treated as an error — the same **hint, never a truth** discipline the
cross-page structures follow, applied to a list of RIDs instead of a free-space map.

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
- **Free space map.** Not a non-goal, a staged one: `dead_bytes` and the rotating insert
  cursor land with this doc, the FSM page is designed above and deferred. See
  [Free space across pages](#free-space-across-pages).
- **Overflow pages / TOAST.** A row that does not fit a page is an error at `CREATE TABLE` or
  `INSERT`, not a chain of pages. Rejected because a multi-page insert has no atomicity without a
  WAL — a partial chain leaks pages with no undo — and the scoped SQL cannot produce such a row.
- **Page-level defragmentation on a background thread.** Compaction is inline, on the thread that
  needs the space.
- **Visibility / versioning.** No `xmin`/`xmax`. Deleted means gone, immediately, for everyone.
  The one place that absence costs something concrete is the Halloween problem, priced and
  handled under [Mutating during a scan](#mutating-during-a-scan).

## Open questions

None outstanding. The collect-then-mutate rule under
[Mutating during a scan](#mutating-during-a-scan) is a constraint this layer imposes, not a
decision it owns — how `DELETE` and `UPDATE` statements are actually planned around it settles in
the executor design doc.

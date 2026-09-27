# B+tree Index

**Status:** Draft — under review
**Component:** `src/indices` (`BPlusTree`, `IndexIterator` in `b_plus_tree.hpp`; node views in `b_plus_tree_page.hpp`)

The primary-key index. Maps a unique `int64` key to the `RID` of the row that holds it, so that
`WHERE pk = 42` and `WHERE pk BETWEEN 10 AND 20` touch a handful of pages instead of the whole
heap. Sits on `BufferPoolManager` ([DD-002](./DD-002-buffer-pool-manager.md)), takes its pages
from the file layout in [DD-001](./DD-001-storage-file-layout.md), and stores the `RID`s that
[DD-004](./DD-004-heap-pages-and-table-heap.md) hands out.

This is the one component above the buffer pool that gets DD-002's level of rigor, and it spends
it differently. DD-002 was long because it invented a latching protocol. This doc invents none —
the table lock (see [Concurrency](#concurrency)) guarantees one writer or many readers per tree —
so the weight moves to the **algorithms**: split, redistribute and merge are where B+tree bugs
live, and each one is written out step by step below with its arithmetic checked.

## The rule everything follows

> **Separators bound their children.** For an internal node with separator keys
> `k[1] < k[2] < … < k[n-1]` and children `c[0] … c[n-1]`, every key reachable through `c[i]`
> lies in `[k[i], k[i+1])`, with `k[0] = −∞` and `k[n] = +∞`.

Every structural operation — split, redistribute, merge, root growth and root collapse — is a
rewrite of some separators and some children, and each one is correct exactly when it preserves
this. It is the invariant worth stating first because it is the one whose violation is
**silent**: a key that lands on the wrong side of a separator is still in the tree, still in the
leaf chain, still returned by a full scan — and invisible to every point lookup, which follows
the separators to a leaf that does not contain it. Nothing fails. The key just stops existing
for `WHERE pk = …`.

The corollary: **separators are routing information, not data.** A separator does not have to
equal any key currently in the tree. Deleting the key a separator was copied from leaves the
separator in place, and it remains a correct bound. Only splits, redistributions and merges
rewrite separators, never a plain delete.

## Scope and contract

- **Keys are `int64`, unique.** A duplicate insert is `kDuplicateKey`, which `ErrorCode` already
  carries. One index per table, on the primary key.
- **The value is a `RID`.** The tree never dereferences it; it is an opaque 8-byte payload.
- **Operations:** `Insert(key, rid)`, `Delete(key)`, `Get(key) -> RID`, and a forward range scan
  over a closed interval `[lo, hi]`.
- **Threading:** one writer *or* many readers, never both, **enforced by the caller** holding the
  table's reader-writer lock. The tree takes page guards on every access because the pool
  requires them, but it defines no protocol of its own. See [Concurrency](#concurrency).

Integer keys make every comparison predicate a closed interval, which is why the scan takes
`[lo, hi]` and nothing else: `pk < 5` is `[INT64_MIN, 4]`, `pk > 5` is `[6, INT64_MAX]`,
`pk = 5` is `[5, 5]`. The executor does that translation, including the two predicates that
become empty at the ends of the domain (`pk < INT64_MIN`, `pk > INT64_MAX`).

## Where the root lives: the index header page

The root moves. A root split creates a new root above the old one; in phase 2, a root that is
down to one child is replaced by that child. So "which page is the root" is mutable state, and it
needs a home whose own address never changes.

> **An index is identified by its header page id, which never changes. The header page holds the
> root page id, which does.**

The catalog stores the header page id once, at `CREATE TABLE`, and never writes it again. Every
operation starts by reading the root id out of the header. This is Postgres's nbtree metapage
(block 0 of every index relation) and BusTub's header page.

**Alternatives rejected:**

- **Root id stored directly in the catalog** — DD-001's original text. Every root split would
  write-latch the catalog page, which holds every table's schema, from inside a tree operation.
  It couples the tree to a component that does not exist yet and turns a tree-internal event
  into a write to a global page.
- **A fixed root page** — the root's page id never changes; a root split copies the root's
  contents down into two new children and rewrites the root in place (SQLite's
  `balance_deeper`; InnoDB). Removes the header page and its extra hop. Rejected in favour of the
  header page, whose root changes are a pointer swap rather than a page copy: root collapse is
  "point the header at the child, free the old root", with no contents moved in either
  direction.

### Header page layout

A new page type, **`INDEX_HEADER`**, appended to the end of `PageType` — values are persisted, so
the enum is append-only. Not called "meta": `META_PAGE_ID` and `PageType::META` already mean the
file superblock.

| Field | Type | Purpose |
|---|---|---|
| `root_page_id` | `page_id_t` | Current root. Never `INVALID_PAGE`: an empty tree's root is an empty leaf. |
| `height` | `uint16_t` | Number of levels; `1` means the root is a leaf. Always `root.level + 1`. Stored so the invariant checker can assert every leaf sits at the same depth against a recorded number rather than against whichever leaf it happened to reach first. |
| `leaf_max` | `uint16_t` | Maximum entries per leaf. The physical capacity in production; smaller in tests. See [Test fanout](#test-fanout). |
| `internal_max` | `uint16_t` | Maximum children per internal node. Same. |

Deliberately **not** stored: an entry count. It would be a nice `size()`, and it would make
every insert and delete write the header page. The header is written on root changes only.

**An empty tree still has a root**, and it is an empty leaf. `Create` allocates the header and
that leaf together. Search, insert and scan therefore have no "tree is empty" branch — the same
reason `TableHeap::Create` always allocates a first page.

## Node layout

With fixed-width keys, **there is no slot array.** Slots exist in `HeapPage` for two reasons —
tuples vary in length, and RIDs must survive compaction — and neither applies here: every entry
is the same size, and nothing outside the tree holds the position of an entry. A node is a
**sorted array of fixed-width entries**. Search is binary search; insert and delete shift the
tail of the array with `memmove`.

```
body offset 0
+-----------------------------+
| node sub-header (8 bytes)   |
+-----------------------------+
| entry[0] entry[1] ...       |  sorted by key, 16 bytes each
|                             |
|        unused space         |
+-----------------------------+
body offset 4064
```

### Node sub-header (8 bytes, both node types)

| Field | Type | Purpose |
|---|---|---|
| `level` | `uint16_t` | `0` for a leaf, parent's level + 1 above it. Redundant with `page_type` on purpose: the checker asserts `child.level == parent.level - 1` for every link, which catches a split that hung a node at the wrong depth or a pointer written into the wrong parent. |
| `count` | `uint16_t` | Entries in use. For a leaf, keys; for an internal node, **children**. |
| `next_page_id` | `page_id_t` | Leaf: the right sibling, `INVALID_PAGE` for the rightmost leaf. Internal: always `INVALID_PAGE`. Also keeps the entry array 8-byte aligned. See [The leaf chain](#the-leaf-chain). |

### Leaf entry (16 bytes)

| Field | Type |
|---|---|
| `key` | `int64_t` |
| `rid` | `RID` — `page_id_t` + `slot_id_t` + 2 bytes padding |

`sizeof(RID)` is **8, not 6**: an `int32` followed by a `uint16` pads to the struct's 4-byte
alignment. The padding bytes are written as zero so that two leaves holding the same entries are
byte-identical, which is what makes a page dump or a byte comparison in a test meaningful.

### The leaf chain

Leaves are chained left to right through the **sub-header's `next_page_id`**, not through
`PageHeader::next_page_id`, which stays `INVALID_PAGE` on every index page. This reverses
DD-001's original text, which listed `INDEX_LEAF → right sibling` among the page-header chains.

- **The two links mean different things.** DD-001's chains answer *which pages belong to this
  object*: every page of a heap is on its chain, and the link changes only when the object grows.
  The leaf chain answers *what comes next in key order*: the header page and internal nodes are
  not on it, and splits and merges rewire it constantly. That is structure inside the tree, so it
  belongs in the tree's format. Postgres agrees — `btpo_next` lives in nbtree's per-page special
  space, not in the shared page header.
- **It makes the node views complete.** With the link in the page header, a leaf split would be
  half view (`Assign` the entries) and half guard (`SetNextPageId`), and the pure byte-array tests
  could not reach the one step whose order matters. In the sub-header, split and merge are view
  operations end to end.
- **It is free.** It occupies the four bytes the sub-header was already padding for alignment;
  capacity stays 253.

Right link only: merge finds a node's left sibling through the parent, and there is no reverse
scan. Internal nodes carry no link (no B-link tree), and the checker asserts theirs is
`INVALID_PAGE`.

### Internal entry (16 bytes)

| Field | Type |
|---|---|
| `key` | `int64_t` |
| `child` | `page_id_t` + 4 bytes padding |

An internal node with `count = n` holds `n` children and `n - 1` separators, stored as `n`
`(key, child)` pairs in which **`entry[0].key` is never read.** One entry type and one array
instead of two arrays with an off-by-one between them. `entry[0].key` is written as `INT64_MIN`
anyway — the value it stands for — so a dump reads correctly and the checker can assert it.

The internal entry could be packed into 12 bytes. It is padded to 16 so both node types share one
entry size, one capacity and one layout path. The cost is fanout, and fanout here is not scarce:
see the arithmetic below.

### Capacity and occupancy

Both node types: `(4064 − 8) / 16 = 253` entries. Write `N = 253`.

| | Max | Min (non-root) | Underflow when |
|---|---|---|---|
| Leaf | `N = 253` keys | `⌊N/2⌋ = 126` keys | `count < 126` |
| Internal | `N = 253` children | `⌈N/2⌉ = 127` children | `count < 127` |
| Root leaf | `N` | `0` | never |
| Root internal | `N` | `2` children | `count < 2` → collapse |

The minimums are chosen so that split and merge each produce legal nodes, and that is checkable
arithmetic rather than hope:

- **Leaf split:** a full leaf receiving one more key holds `N + 1 = 254`, splits `127 / 127`.
  Both `≥ 126`. ✓
- **Leaf merge:** only ever between a leaf that underflowed (`≤ 125`) and a sibling at exactly
  the minimum (`126`, else redistribution applies): at most `251 ≤ 253`. ✓
- **Internal split:** `254` children split `127 / 127`, one separator moving up. Both `≥ 127`. ✓
- **Internal merge:** `≤ 126` plus `127` is at most `253 ≤ 253`. ✓ The separator pulled down from
  the parent does not add a child, so it does not break the bound.

These hold for every `N ≥ 3`, not only 253, which is what makes small test fanouts legal (below).
They belong in `static_assert`s next to the constants.

**What the fanout buys.** Around 250 per level: two levels hold ~64 thousand keys, three hold ~16
million, four hold ~4 billion. The tree is never deeper than four levels at any size this engine
will see, and that small, bounded height is what keeps the frame budget below affordable.

### Test fanout

The header records `leaf_max` and `internal_max`, and `Create` accepts smaller values than the
physical capacity (any value `≥ 3`). **This is the most important testing decision in the doc.**
At `N = 253`, a test needs tens of thousands of keys before the first internal node splits and
the tree reaches three levels, and far more before a merge cascades through two levels. At `N = 4`, a handful of keys
produces a four-level tree, and every split, redistribution, merge and root change runs within
the first hundred operations of a randomised test. BusTub builds its tree the same way
(`leaf_max_size`, `internal_max_size`) for the same reason.

Stored in the header rather than passed on every `Open` so that a reopened tree cannot be
accidentally opened with a different fanout than it was built with.

## Descent

Every operation begins the same way: fetch the header page, read `root_page_id`, drop the header
guard, fetch the root.

**Choosing a child.** At an internal node with `count = n`, descend into `c[i]` for the largest
`i` in `[1, n)` with `k[i] ≤ key`, or `c[0]` if there is none. That is an `upper_bound` over
`entry[1 … n)` minus one. The range starts at 1, never 0: `entry[0].key` is not a separator.

**At the leaf,** a `lower_bound` over `entry[0 … count)` finds the key or the position it would
be inserted at.

### Readers: one guard at a time

`Get` and the scan hold **one page guard at a time**: fetch the child, drop the parent. With no
concurrent writer there is nothing for hand-over-hand coupling to protect against, and one guard
keeps a reader's footprint in the pool constant regardless of tree height.

### Writers: the whole path is held

`Insert` and `Delete` hold **write guards on the entire root-to-leaf path** for the duration of
the operation, plus the header page. Structural changes propagate upward — a split's separator
goes into the parent, a merge removes an entry from the parent — and holding the path means the
parent is already in hand, already latched, and cannot have changed.

The frame cost is bounded by the height, and the height is at most four:

| Operation | Frames held at peak |
|---|---|
| `Get`, scan | 1 |
| `Insert` | header + `H` path + up to `H + 1` new pages = `2H + 2` → **10** at `H = 4` |
| `Delete` | header + `H` path + 1 sibling = `H + 2` → **6** at `H = 4` |

The pool must be at least that large, and a test asserts it: a tree driven to height 4 must keep
working in a pool of exactly `2H + 2` frames. Tests that use a tiny fanout build much taller
trees, and they run with a proportionally larger pool.

## Insert

1. **Descend**, holding the path. At the leaf, `lower_bound` for the key. **If it is present,
   return `kDuplicateKey` before anything is modified.**
2. **If the leaf is not full,** shift the tail right by one entry, write the new entry, increment
   `count`. Done. This is the overwhelming majority of inserts.
3. **Otherwise, count the splits before making any.** Walk up the held path from the leaf and
   count the consecutive full nodes: that is how many nodes will split. If that run reaches the
   root, the root splits too and a new root is needed. Call `NewPage` that many times — plus one
   for a new root — **before modifying anything**. If any allocation fails, release what was
   allocated with `DeletePage` and return the error with the tree untouched.
4. **Split the leaf** (below), producing a new right leaf and a separator.
5. **Insert `(separator, new_right)` into the parent**, immediately after the entry pointing at the
   node that split. If the parent is full, split it the same way and carry its separator upward.
   Repeat.
6. **If the root split,** initialise the preallocated page as an internal node with
   `level = old_root.level + 1` and two children — the old root and its new sibling — then
   write the new root id and `height + 1` into the header.

> **Allocate everything, then mutate.** Step 3 is what makes insert all-or-nothing without a
> WAL. After it, every page the operation will write is already allocated, pinned and latched,
> so steps 4–6 call nothing that can fail. A split cannot stop halfway through because a later
> `NewPage` hit `kBufferPoolFull`, leaving a right sibling allocated but unlinked, or a
> separator in a parent whose child was never written.

### Leaf split

Copy the full leaf's `N` entries plus the new one, in order, into a temporary array of `N + 1`
(4 KB, on the stack). The left leaf — the original page — keeps the first `⌈(N+1)/2⌉`; the new
right leaf takes the rest.

- The separator is the **right leaf's first key**, and it is **copied up**: it stays in the right
  leaf as a real entry and also becomes a routing key in the parent.
- Chain: `right.SetNextLeaf(left.NextLeaf())`, then `left.SetNextLeaf(right)`. In that order —
  the reverse overwrites the only copy of the old link and loses the rest of the chain. Both are
  view operations, so the node tests cover this step.
- `right.level = 0`, `page_type = INDEX_LEAF`.

Building the `N + 1` array first and splitting it, instead of splitting first and then choosing
which half receives the new key, removes the case analysis where the classic off-by-one lives.

### Internal split

Same temporary array — `N + 1` children after inserting the new `(separator, child)`. The left
node keeps the first `⌈(N+1)/2⌉` entries; call the index of the next one `m`.

- `temp[m].key` is **moved up**, not copied. It becomes the separator in the parent and appears in
  neither half as a separator.
- The right node's `entry[0]` is `temp[m]` with its key reset to `INT64_MIN`: its child is the
  right node's first child, and its key is exactly the one just moved up.
- The right node takes `temp[m+1 …]` after that.
- `right.level = left.level`.

> **Leaves copy up. Internal nodes move up.** A leaf separator must also remain in the leaf,
> because leaves hold the data. An internal separator must not remain below, or it would bound
> two ranges at once. Mixing the two up is the most common B+tree bug there is.

### Ascending inserts — a known inefficiency, left in

Primary keys are often inserted in increasing order, and every one lands in the rightmost leaf.
An even split then leaves each left half at 50% and never touches it again, so a table loaded in
key order ends with every leaf half-empty. Postgres and SQLite detect an insert at the right edge
of the rightmost leaf and split unevenly, leaving the left side full. v1 splits evenly; the fix is
local to the split-point choice and changes no format, so it is recorded here rather than built.

## Delete (phase 2)

Built after insert, search and split are complete and tested. It is additive — nothing above
depends on it — and debugging two unfinished halves of a tree at once is how tree corruption
becomes unfindable.

1. **Descend**, holding the path. If the key is absent, return `kNotFound`.
2. **Remove the entry** from the leaf: shift the tail left, decrement `count`. Leave every
   separator above it alone — see [the corollary](#the-rule-everything-follows).
3. **If the node is the root, or `count` is still at least the minimum,** done.
4. **Otherwise it underflowed. Pick one sibling:** the left sibling under the same parent if there
   is one, else the right. Never a cousin under a different parent — the parent holds the
   separator between two siblings, and that separator is what redistribution and merge rewrite.
5. **If the sibling has more than the minimum, redistribute** one entry (below). Done — the parent
   loses nothing.
6. **Otherwise merge** (below). The parent loses one entry and may underflow in turn; if so,
   repeat from step 3 one level up.
7. **Root collapse.** If the root is an internal node left with one child, write that child's id
   into the header, decrement `height`, and free the old root. If the root is a leaf, it may shrink
   to zero entries and stays: an empty tree is an empty leaf.

**Always merge right into left.** Whichever of the two nodes underflowed, the survivor is the left
one and the right one is freed. One code path, one direction, and the leaf chain only ever loses
a link it can repair from the node it already holds.

### Redistribute

Let `S` be the parent separator between the two siblings.

**Leaves, borrowing from the left sibling `L`:** move `L`'s last entry to the front of the node.
The new separator is the node's new first key.

**Leaves, borrowing from the right sibling `R`:** move `R`'s first entry to the end of the node.
The new separator is `R`'s new first key.

**Internal, borrowing from the left sibling `L` — a rotation through the parent:** the node's
current `entry[0]` child needs a real separator in front of it now, and that separator is `S`
(which bounded it from the left). So: shift the node right by one; set the new `entry[1].key = S`;
set `entry[0].child` to `L`'s last child; the new parent separator is `L`'s last key; drop `L`'s
last entry.

**Internal, borrowing from the right sibling `R`:** append `(S, R.entry[0].child)` to the node; the
new parent separator is `R.entry[1].key`; remove `R.entry[0]`, so `R`'s old `entry[1]` becomes its
`entry[0]`, and reset that key to `INT64_MIN`.

The internal cases are rotations, not moves: a key goes **down** from the parent and a different
key comes **up** from the sibling. Writing either as "move one entry across" puts a separator on
the wrong side of its child.

### Merge

Let `L` be the left node, `R` the right, `S` the separator between them in the parent.

- **Leaves:** append `R`'s entries to `L`; `L.SetNextLeaf(R.NextLeaf())`.
- **Internal:** append `(S, R.entry[0].child)` to `L`, then `R.entry[1 …]`. `S` comes **down**,
  because it bounds `R`'s first child and is about to live inside `L`.
- **Remove `R`'s entry from the parent.**
- **Free `R`**: drop its guard, then `DeletePage`. `DeletePage` rejects a pinned page, so the guard
  must be gone first.

**Unlink first, then free.** By the time `DeletePage` runs, nothing in the tree points at `R`, so
the tree is correct whether or not the free succeeds. That matters because `DeletePage` can fail
spuriously against a reclaimer's transient pin (DD-002, "Known behaviour"). A failed free is a
leaked page — allocated and unreachable — never a dangling pointer.

### A delete that fails partway

Unlike insert, delete cannot allocate everything first: whether level 2 needs a sibling depends on
what happened at level 1. So a sibling fetch at an upper level can fail after a lower level has
already merged. The rule that makes this safe: **each level is fully consistent before the next
one is touched.** A delete that stops partway leaves an underfull node above the level it
finished — and occupancy is the only invariant it can break. The tree remains ordered, bounded,
searchable and correctly chained; it is less full than it should be, which costs space and never
correctness.

## Range scan

`Scan(lo, hi)` descends to the leaf that would hold `lo`, positions at `lower_bound(lo)`, and
returns an `IndexIterator`.

- **One leaf guard at a time**, following each leaf's `NextLeaf()` — the same fixed-pool reasoning as
  `TableIterator`.
- **Entries are copied out.** A `(key, RID)` pair is 16 bytes of value, so there is no lifetime
  question to get wrong the way there is with a tuple span.
- **A position past the last entry of a leaf is not a result.** `lower_bound(lo)` can land one past
  the end of the leaf the descent reached — every key in that leaf is below `lo`, and the first
  key `≥ lo` is the first entry of the next leaf. Both the initial positioning and every advance
  must step to the next leaf before reporting anything. Stale separators make this case routine
  after deletes, not exotic.
- **Stops at the first key `> hi`,** or at the end of the chain.
- **Same shape as `TableIterator`:** a move-only cursor with a fallible `Result<bool> Next()` plus
  `Key()` and `Rid()`, for the same two reasons (DD-004, "API shape").

> **Never mutate the tree through a live iterator.** `Next()` returns holding a read guard on the
> current leaf; a `Delete` on the same thread asks for a write guard on that frame and blocks
> forever on the non-recursive `shared_mutex`. This is DD-004's rule unchanged, and the executor
> satisfies both with the same discipline: collect, let the iterator die, then mutate.

## Invariants and `CheckInvariants()`

An O(n) walk of the whole tree, run after every operation in tests and never in production code
paths. It checks, from the header down:

1. **Page types:** the header is `INDEX_HEADER`, internal nodes `INDEX_INTERNAL`, leaves
   `INDEX_LEAF`.
2. **Levels:** `root.level == height − 1`; every child's level is its parent's minus one; leaves
   are level 0. Together these give "every leaf at the same depth".
3. **Order:** keys strictly increasing within every node — `entry[0 …]` in a leaf,
   `entry[1 …]` in an internal node — and `entry[0].key == INT64_MIN` in every internal node.
4. **Separator bounds — [the rule](#the-rule-everything-follows):** carry a `[lo, hi)` interval
   down the recursion and assert every key in every node falls inside it. This is the check that
   catches the silent bug.
5. **Occupancy:** every non-root node between its minimum and maximum; an internal root has at
   least two children.
6. **The leaf chain:** walking `NextLeaf()` from the leftmost leaf visits exactly the leaves the
   recursive walk found, in the same order, and ends at `INVALID_PAGE`. The concatenated keys are
   strictly increasing across leaf boundaries.
7. **No sharing:** no page is reached twice.

Every item on this list maps to a real bug class. 2 catches a node hung at the wrong depth; 4
catches a separator on the wrong side; 6 catches a split that linked the chain in the wrong order
or a merge that dropped a link; 7 catches a child pointer written into two parents.

## Testing

**Node views are pure.** `LeafNode` and `InternalNode` are views over a
`span<byte, PAGE_BODY_SIZE>`, like `HeapPage`, validated by `AsLeaf` / `AsInternal` against
`page_type` in the same way `AsHeapPage` is. Their search, insert-at, remove-at, split and merge
helpers are tested against a bare `std::array<std::byte, 4064>` in microseconds, which is where
the index arithmetic gets debugged.

**Tree tests run against a real file and a real pool**, as every suite in this repo does.

**The randomised differential test is the centrepiece.** A seeded PRNG drives a long sequence of
inserts, deletes, point lookups and range scans against the tree and against a `std::map` in
lockstep; every result must match, and `CheckInvariants()` runs after every operation. At fanout
4, a few thousand operations exercise every split, redistribution, merge and root change many
times over. The seed goes into a `SCOPED_TRACE`, so any failure reproduces exactly from its
output. This one test finds more tree bugs than every hand-written case combined.

Around it, the targeted cases:

- ascending, descending and random insertion orders, each to several levels;
- delete everything, in each order, until the tree is back to one empty root leaf — and then
  assert on the **file**: `PageCount()` never shrinks, but every page the tree freed must be back
  on the free list, so `PageCount()` minus the free-list length must equal the reserved pages plus
  the header and the root. The same leak-detection idea as the heap's chain-extension test; it
  needs a test-visible free-list length on `DiskManager`;
- a reopen test: build, `Shutdown`, reopen from the header page id, and replay the differential
  comparison against the saved `std::map`;
- boundary keys: `INT64_MIN`, `INT64_MAX`, and ranges that start or end there;
- the frame budget: a height-4 tree still works in a pool of exactly `2H + 2` frames.

## Concurrency

The tree defines no locking. Its contract is **one writer or many readers, never both**, and the
table's reader-writer lock enforces it: an index belongs to its table, and a statement holding the
table exclusively holds the index exclusively. Page guards are still taken on every access —
the pool requires a pin to keep a frame resident, and the latch costs nothing uncontended.

**No latch crabbing, and no B-link tree.** Crabbing exists so concurrent writers can restructure
one tree at the same time; under the table lock that state is unreachable, so crabbing code would
guard against nothing and could never be exercised by an end-to-end test. Two rules above would
be the first to break if concurrent writers ever returned, and are called out so nobody relies on
them silently: readers drop the header guard before descending, trusting that no writer can move
the root in between; and writers hold the whole path pessimistically, which crabbing would replace
with early release above the deepest safe node.

## Durability

Inherited from DD-001: no WAL, durability on clean `Shutdown()`. A split writes three or more
pages; a crash between their writebacks can leave a separator pointing at a page that was never
written, or a leaf chain that skips a node. The tree does not detect or repair this.

One mitigation comes for free: the index is **redundant with the heap**. Every entry is a
`(primary key, RID)` pair recoverable by scanning the table. Rebuilding an index from its heap is
a complete recovery procedure for any index damage, and is worth exposing as an admin command.

## API shape

- **`Create(bpm, leaf_max = N, internal_max = N)` and `Open(bpm, header_page_id)`**, factories
  returning `unique_ptr`, for the same reasons as `TableHeap`: both can fail, and the object is
  not meant to move. `HeaderPageId()` is what the catalog persists. **Unlike `TableHeap::Open`,
  `Open` reads a page:** the fanout is fixed for the tree's life and every operation needs it, so
  it is read once, cached, and validated (`INDEX_HEADER`, fanout in range). The root is not
  cached — it moves.
- **`Insert(key, rid) -> Status`**: `kDuplicateKey`, or whatever allocation reported.
- **`Delete(key) -> Status`**: `kNotFound` if absent. Named `Delete` to match `TableHeap`.
- **`Get(key) -> Result<RID>`**: `kNotFound` if absent.
- **`Scan(lo, hi) -> Result<IndexIterator>`**: a closed interval. Fallible, unlike
  `TableHeap::Scan`, because positioning descends the tree.
- **`CheckInvariants() -> bool`**, public so tests can call it on a tree they drove through the
  public API alone.

Errors from below propagate unchanged. `kNotFound` means the key, `kDuplicateKey` means the key,
`kBufferPoolFull` means the pool, `kCorruption` means a page failed `AsLeaf`/`AsInternal` — and
flattening any of them into `kInternal` throws away exactly the information the caller needs.

## Keeping the index and the heap in sync

Not this component's code, but its correctness depends on it, so the obligations are recorded
here for the executor:

- **`INSERT`:** probe the index first and fail with `kDuplicateKey` before the heap is touched.
  Under the exclusive table lock, nothing can insert the same key in between.
- **`TableHeap::Update` can return a new `RID`.** When it does, the row's index entry must be
  rewritten to the new `RID`. Most updates keep their RID, which is exactly what makes a missing
  rewrite easy to ship: the index is right until a row outgrows its page.
- **`DELETE`:** remove the index entry and the heap row. With no undo, a multi-row statement that
  fails partway leaves some rows deleted and some not — a documented limitation of a no-WAL
  engine, not something either component can fix alone.

## Amendments to DD-001

- **Root location.** "Each index's current root page id is stored as mutable data in the catalog"
  becomes: the catalog stores the index's **header page id**, which is immutable; the header page
  stores the root id.
- **New page type** `INDEX_HEADER`, appended to `PageType`.
- **Open question closed — index key storage:** fixed-width `int64`, no slotted format.
- **Leaf siblings leave the page header.** `INDEX_LEAF → right sibling` is removed from DD-001's
  chain invariant; the link lives in the node sub-header ([The leaf chain](#the-leaf-chain)), and
  `PageHeader::next_page_id` is `INVALID_PAGE` on every index page. DD-001 and `page_header.hpp`
  are updated to match.
- **Open question closed — internal sibling links:** none. Leaves only, right link only.

## Non-goals

- **Duplicate keys.** The upgrade path is to make `(key, RID)` the real key, so every entry is
  unique by construction and the algorithms above run unchanged; only the comparison and the
  separator width change. Postgres's nbtree has worked this way since version 12.
- **Variable-length keys, prefix and suffix compression.** Fixed-width `int64` only.
- **Concurrent writers:** no crabbing, no B-link tree, no optimistic descent. See
  [Concurrency](#concurrency).
- **Reverse scans.** No left links, no backward iterator.
- **Bulk loading.** Building an index bottom-up from sorted input is much faster than repeated
  inserts; at this scale repeated inserts are fine, and they reuse the code the tests already
  cover.
- **Uneven splits for ascending inserts.** Designed above, deferred.
- **Crash consistency.** No WAL; rebuild from the heap instead.

## Open questions

- **Minimum pool size.** The frame budget gives a floor of 10 frames at height 4. Whether the pool
  enforces a minimum at construction, or the tree checks `height` against the pool's capacity and
  refuses an insert that could exhaust it, is undecided.
- **Where the heap/index sync obligations are enforced** — a thin table-level wrapper that owns
  both a `TableHeap` and its `BPlusTree`, or the executor directly. The first puts the rules in one
  place; it is probably the right answer, and it settles with the catalog.

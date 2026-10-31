# Catalog & Tables

**Status:** Draft — decisions 1–3 agreed 2026-10-31; awaiting review of the whole.
**Component:** `src/catalog` (`Schema`, `Value`, `Row` in `schema.hpp`; `Table` in `table.hpp`;
`Catalog` in `catalog.hpp`; `Database` in `database.hpp`; `table_id_t` in `common/types.hpp`)

Turns "a heap and a B+tree" into **a named table with a schema**. The last layer below SQL: the
executor will reach every row through it. Sits on `TableHeap`
([DD-004](./DD-004-heap-pages-and-table-heap.md)) and `BPlusTree`
([DD-005](./DD-005-b-plus-tree.md)), and finally gives page 1
([DD-001](./DD-001-storage-file-layout.md)) a format.

Deliberately short. The storage layers needed design-doc rigour because they invent on-disk
structures and latching rules; this layer invents neither. The only section with a real
trade-off is [Keeping the heap and the index in sync](#keeping-the-heap-and-the-index-in-sync).
Everything else is a format and a call order.

## Scope

In:

- **Row format** — column types, encoding a row to bytes and back, the size cap.
- **Catalog** — the list of tables, persisted from page 1, loaded at open.
- **Table** — one heap plus its primary-key index, kept in sync.
- **Database** — opens the file and wires `DiskManager` → `BufferPoolManager` → `Catalog`.

Out, deliberately: `DROP TABLE` / `ALTER TABLE`, secondary indexes, `UPDATE`, NULLs, executor
operators (`SeqScan` / `IndexScan` are the executor's), table locks (stage 5), and file
compaction (see [Space reclamation](#space-reclamation)).

## Row format

### Types

Two: **`INT64`** and **`VARCHAR(n)`**. `INT64` covers keys, numbers and join columns; `VARCHAR`
covers text. Every further type (`BOOL`, `DOUBLE`, `DATE`) adds encoding, comparison and parsing
code to every later stage and teaches nothing new.

`VARCHAR(n)` counts **bytes**, not characters, and compares with `memcmp` order — no collation,
no UTF-8 awareness. This is SQLite's default `BINARY` collation.

### No NULLs

Every column is `NOT NULL`, and `INSERT` must supply every column.

Storing NULL is cheap — a bitmap per row. What it costs is semantics, everywhere above this
layer: `x = NULL` is *unknown* rather than true or false, so `WHERE` needs three-valued logic;
NULL never equals NULL, so every join needs a NULL rule; hash and sort-merge join each need
their own. Leaving NULL out keeps that tax off the executor and all four join algorithms. If it
is added later, the format change is a bitmap in front of the row; the real work is in the
executor.

### Encoding: sequential

Columns in schema order. `INT64` is 8 bytes; `VARCHAR` is a `u16` byte length followed by the
bytes. No row header. Host-endian `memcpy`, the convention `page_header.hpp` already documents
(the file is not portable across architectures).

The alternative is Postgres's shape: fixed-width columns at fixed offsets, each variable-length
column as an (offset, length) pair, data after — any column in O(1) without decoding the ones
before it. That only pays off for an engine that decodes just the columns a query touches. This
executor decodes every row whole into a `Row` and passes that between operators, so the O(1)
access would never be used, and a 2000-byte row walks in nanoseconds. SQLite's record format is
sequential for the same reason.

In C++: `Value = std::variant<int64_t, std::string>`, `Row = std::vector<Value>`, and a `Schema`
holding the columns (name, type, max length) and the primary-key column index, with
`MaxRowSize()`, `Encode(Row) -> bytes` and `Decode(bytes) -> Row`.

### Primary key

Exactly one primary-key column, and it must be `INT64`, declared `PRIMARY KEY` in `CREATE TABLE`.
The index takes unique `int64` keys only (DD-005), so this is what lets `Table` read the index
key straight out of the `Row`.

### The size cap

`MAX_TUPLE_SIZE` (2000 bytes, DD-004) is enforced at `CREATE TABLE` against the largest row the
schema can produce — 8 per `INT64`, `2 + n` per `VARCHAR(n)` — and at `INSERT` by rejecting any
string longer than its column's `n`. Together those make an oversized row impossible to build;
`HeapPage` still checks, as a backstop.

## Catalog storage

### Format

One serialized stream for the whole catalog, cut into page-body-sized pieces along the
`CATALOG` chain that starts at page 1 (DD-001, "Multi-page objects").

```
header      format_version u32 | stream_length u32 | table_count u32 | next_table_id u32
per table   table_id u32 | name | heap_first_page_id | index_header_page_id
            | pk_column u16 | column_count u16
per column  name | type u8 | max_len u16
name        u8 length + bytes
```

`DiskManager` creates page 1 typed `CATALOG` with a zeroed body, so **an all-zero header must
read as an empty catalog**: `format_version` 0 means "never written". The first `CREATE TABLE`
writes version 1.

A **system table** — Postgres's `pg_class`, a catalog stored as ordinary rows and queryable with
SQL — was rejected in the August scoping and still is: it would make the catalog depend on the
row format, the heap and the executor it is supposed to sit beneath.

### Writing: whole stream, every `CREATE TABLE`

With no `DROP TABLE` the stream only grows, and at roughly 30 bytes per table plus 20 per
column, page 1 alone holds 25–50 tables. Rewriting the whole stream is simpler than any
incremental scheme and costs one page write.

`CREATE TABLE` follows the order Insert uses in DD-005 — **every fallible step first, then
mutate**:

1. **Validate** — name legal and free (the schema was validated when it was built), and size
   the new stream. Page ids are fixed-width, so the size is known before the table exists.
2. **Fetch** every catalog page that stream needs, write-latched, allocating one more — not yet
   linked — if the stream has outgrown the chain.
3. **Create** the table: `Table::Create`, which makes the heap and the index and is itself all
   or nothing (it frees the heap's page if the index cannot be created).
4. **Write**: add the table to the map, serialize, write the stream across the pages, linking
   the new one. Nothing here can fail.

Catalog pages are fetched *before* the table is created so that the only thing a late failure
has to undo is at most one unlinked catalog page — freed best effort, the rule `BPlusTree::Create`
already follows (a failed free is a leak). Either way the catalog is unchanged in memory and on
disk. Durability is `Shutdown()`, as for every other page (DD-002); a crash mid-`CREATE TABLE`
is no worse than a crash mid-`INSERT`.

### Names

Table and column names are **case-insensitive, stored lowercase** — standard SQL for unquoted
identifiers. The parser lowercases; the catalog only ever compares exactly. A legal name is 1–64
bytes of `[a-z_][a-z0-9_]*` (`IsValidName`). Table names are unique; column names are unique
within a table. `Schema::Create` and `Catalog::CreateTable` both re-check, so a name built by
hand — a test, a future API — never reaches disk in a shape a later lookup could not match.

### Table ids

A `u32`, assigned from `next_table_id`, monotonic, never reused. Nothing reads it yet: it is
for stage 5, where a statement that touches several tables locks them in table-id order. Four
bytes now instead of a format change later.

### Loading and validation

`Catalog::Open` reads the stream once and builds everything: a name → table map and one `Table`
per entry, each opening its heap and its index. After that the catalog pages are only touched
again by `CREATE TABLE`. Opening every index reads one header page per table — nothing at this
scale.

Nothing on disk is trusted. Any of these is **`kCorruption`**: an unknown `format_version` or
column type; `stream_length` longer than the chain; a primary-key column out of range or not
`INT64`; a reserved page id (0 or 1) as a heap or index page; an empty, over-long or duplicate
name. The same stance as `BPlusTree::Open` rejecting an out-of-range fanout.

## Keeping the heap and the index in sync

A row lives in two places — its bytes in the heap, its `key -> RID` in the index — every write
changes both, and there is no WAL. The question is which order leaves the least damage when one
step fails.

The lever: **`BPlusTree::Insert` and `Delete` are all-or-nothing** (DD-005). A failed tree step
leaves the tree untouched, so the tree step goes wherever its failure costs nothing.

### Insert

1. Validate the row against the schema — arity, types, string lengths. `kInvalidArgument`.
2. `tree.Get(key)`; found → `kDuplicateKey` before the heap is touched (DD-005, "Keeping the
   index and the heap in sync"). Safe because there is one writer.
3. `heap.Insert(bytes)` → `RID`. A failure here has changed nothing.
4. `tree.Insert(key, rid)`. On failure, **undo 3 with `heap.Delete(rid)`** and return the
   original error.

The tree cannot go first: it needs the `RID`, which only the heap insert produces. So step 4
carries an undo, and the question is whether the undo can fail. **Not for lack of frames, while
the pool has one user:** a failed `tree.Insert` has already released everything it pinned, and
`heap.Delete` needs one frame — and the one `heap.Insert` released in step 3 is still free. Only
an I/O error can fail it.

That argument ends at stage 5, when readers of *other* tables can hold frames concurrently. A
failed undo leaves an **orphan heap row**: a full scan returns it, the index does not know it.
It is logged and returned as the original error, and it is a documented no-WAL limitation of the
same kind as DD-005's partially-applied multi-row `DELETE`.

### Delete: tree first

1. `tree.Get(key)` → `RID`; absent → `kNotFound`.
2. `tree.Delete(key)`. On failure **nothing has changed** — return the error.
3. `heap.Delete(rid)`. One frame, and `tree.Delete` just released up to 2H, so again only I/O
   can fail it; if it does, undo with `tree.Insert(key, rid)`, best effort.

Heap first would be the wrong way round: a failed tree step would leave an index entry pointing
at a deleted row, and every later insert of that key would be refused as a duplicate of a row
that does not exist.

### Get

`tree.Get` → `heap.Get` → decode. An index hit whose heap row is gone is **`kCorruption`**, not
`kNotFound`: it means the two have drifted apart, and hiding it would hide the bug.

### All writes go through `Table`

The heap and the index are private. The reads the executor needs are forwarded instead: `Scan()`
(heap order, raw bytes + `RID`, for `SeqScan`), `ScanIndex(lo, hi)` (key order, for
`IndexScan`) and `RowAt(rid)` (the row an index entry names, without a second tree search).
Nothing outside `Table` can change one structure without the other.

Exposing them `const` was the first plan; it does not work, because `TableHeap::Scan/Get` and
`BPlusTree::Get/Scan` are non-`const` — reading fetches pages.

## Database

The object tests, the executor and the shell start from:

- `Database::Open(path, pool_frames)` opens the `DiskManager`, builds the `BufferPoolManager`,
  then opens the `Catalog`. Members are declared in that order, so they are destroyed in reverse.
- `Database::Shutdown()` calls `BufferPoolManager::Shutdown()` — the durability point, as in
  DD-002. A `Database` destroyed without it is a caller bug, handled exactly as the pool's
  destructor already handles it (log, flush best effort, abort): `Database` does not invent a
  second policy.
- Holds no pins of its own: `Table`, `TableHeap` and `BPlusTree` all hold page ids, never guards.

## Testing

As for every layer: real file, real pool, byte-state assertions.

- **Row format (byte arrays):** round trip for every type; `INT64_MIN` / `INT64_MAX`; empty
  string and a string of exactly `n`; `MaxRowSize` arithmetic; schemas just under and just over
  2000 bytes; rows with the wrong arity, type or length rejected.
- **Catalog:** a fresh file is an empty catalog; create → reopen → identical schemas, ids and
  page ids; enough tables (long names, many columns) to spill onto a second catalog page, then
  reopen; duplicate and over-long names rejected; a `CREATE TABLE` that cannot allocate leaves
  the catalog unchanged and leaks nothing; hand-corrupted streams (bad version, bad type,
  `stream_length` past the chain, reserved page ids) are `kCorruption`.
- **Table — `CheckConsistency()`:** a test-only O(n) check, the `CheckInvariants` of this layer.
  Scan the heap; every row's key must be in the index with exactly that row's `RID`, the counts
  must match, and `tree.CheckInvariants()` must pass. Run after every operation.
- **Table — fanout:** `Table::Create` and `Catalog::CreateTable` take an optional index fanout
  (default `NODE_CAPACITY`; the tree persists it, so reopening needs nothing). At the default a
  table reaches height 2 only after ~250 rows; at fanout 3 the sync tests split and merge within
  a handful — DD-005's "most important testing decision", reused.
- **Table — differential:** seeded inserts, deletes and gets against a `std::map<int64_t, Row>`,
  `CheckConsistency()` throughout, then reopen and compare again.
- **Table — fault injection:** pin the pool so `tree.Insert` cannot allocate — the heap row must
  be undone and the table consistent; pin it so `tree.Delete` cannot fetch a sibling — the row
  must still be there in both structures.
- **Not tested:** the failed-undo paths. They need an I/O error mid-operation, and the repo has
  no I/O fault injection. Documented here instead.

## Space reclamation

Considered and deliberately not built.

What exists already: within a page, `HeapPage::Compact()` reclaims deleted rows' bytes; across
pages, freed index pages go on the `DiskManager` free list and are handed out again before the
file grows, and the heap's rotating insert cursor (DD-004 v1) revisits pages that freed up. So
the file does not grow forever. It stays at its **high-water mark** — the most the data ever
needed — and a table at steady churn stabilises.

What is not done is shrinking the file. Moving a page changes its page id, and page ids are
stored everywhere: heap chain links, B+tree child pointers, leaf links, the catalog, and every
`RID` in the index — moving one heap page means rewriting the index entry of every row on it.
Without a WAL a crash halfway through leaves pointers to the wrong pages that nothing can
repair. Real engines avoid in-place shrinking for the same reason: Postgres's plain `VACUUM`
only truncates empty pages off the end of the file, and `VACUUM FULL`, SQLite's `VACUUM` and
InnoDB's `OPTIMIZE TABLE` all **rebuild into a fresh copy**.

If it is ever built, that is the shape: copy the live rows into a new heap and index, point the
catalog entry at them in one stream rewrite, then free the old pages. The remaining gap is a
heap page that empties completely: it stays on its table's chain, reused by inserts but still
read by every full scan (DD-004 prices this). Irrelevant at portfolio data sizes.

## Concurrency

None in this stage: the engine is single-threaded until stage 5. Then the catalog gets a
reader-writer lock — `CREATE TABLE` exclusive, lookups shared — and `Table` relies on the
per-table statement lock, exactly as `BPlusTree` does.

## Non-goals

- **NULL, further types, collations** — see [Row format](#row-format).
- **`DROP TABLE`, `ALTER TABLE`** — not in the SQL scope; dropping would also need to free every
  page of a heap and a tree.
- **Secondary indexes** — one index per table, on the primary key.
- **`UPDATE`** — not in the SQL scope, which also removes DD-005's "an update can move the row
  and change its `RID`" obligation entirely.
- **File compaction** — see [Space reclamation](#space-reclamation).

## Amendments to DD-001

- **Page 1 gets a format:** the catalog stream above. DD-001 reserved the page and left its
  contents to this layer.

## Amendments to DD-004

- **The catalog stores a heap's first page only, not its last.** DD-004 says "the catalog holds
  each table's `first_page_id` and `last_page_id`", but the last page moves every time the chain
  grows — far too often to rewrite the catalog for. `Table::Open` passes the first page as both.
  That is safe as it stands: `TableHeap::Insert` already treats the last page as a hint and
  follows `next_page_id` forward from it under the extension latch, so the first append after an
  open walks the chain once and caches the real end. DD-004's own rule — anything that spans pages
  is a hint, never a truth — applied to one more field.

## Open questions

- **Default `pool_frames` for `Database::Open`.** Tests pass their own; the shell needs a
  default. Proposed, and in `database.hpp` as `DEFAULT_POOL_FRAMES`: 256 frames (1 MiB).

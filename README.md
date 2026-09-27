<p align="center">
  <img src="assets/kernsql-lockup.svg" alt="kernSQL" width="380"/>
</p>

<p align="center">
  A minimal SQL database built from scratch in C++23 on Linux —
  storage engine, indexing, and query engine, no shortcuts.
</p>

---

## What is this?

**KernSQL** is a single-node relational database written from first principles: no storage
libraries, no parser generators, no query framework. Every layer — from the bytes on disk to
the SQL shell — is hand-built, tested, and documented.

It is a learning-in-public project. The goal is not to compete with SQLite; it is to deeply
understand how real databases (Postgres and SQLite in particular) work by building one, and to
leave behind a codebase clean enough that others can learn from it.

The emphasis is **correctness over surface area**. The SQL dialect is deliberately small, and
every layer is tested against real files and real byte state — no mocks — under AddressSanitizer
and ThreadSanitizer. Every design decision is recorded in [`design-docs/`](design-docs/) with the
alternatives that were rejected and why — those documents are the most useful thing in this
repository.

## Status

| Component | State |
|---|---|
| `DiskManager` — single file, 4 KB pages, persistent free list, thread-safe | done, tested |
| `PageHeader` — 32-byte self-describing header (self page id, format version) | done |
| `Replacer` — CLOCK sweep with capped usage counts | done, tested |
| `BufferPoolManager` — sharded page table, frame state machine, RAII page guards | done, tested (incl. concurrency) |
| `HeapPage` — slotted page, stable RIDs across compaction | done, tested |
| `TableHeap` — page chain, free-space reuse, concurrent inserts, forward scan | done, tested (incl. concurrency) |
| Shell — REPL over the buffer pool and table heap, one command per operation | done |
| B+tree — unique `int64` keys, split, merge/redistribute, range scan | **in design** ([DD-005](design-docs/)) |
| Catalog | planned |
| Parser, binder, executor | planned |
| Join algorithms — nested-loop, index nested-loop, hash, sort-merge | planned |
| Table-level reader-writer locking | planned |

144 tests, green under ASan and TSan. The concurrency suites have caught real bugs — a stale
page-table mapping after delete and a lost `SetEvictable` in the buffer pool — both written up
in [DD-002](design-docs/DD-002-buffer-pool-manager.md).

## Architecture

| Layer | Design |
|---|---|
| Storage | Single-file, page-based (4 KB), self-describing page headers, slotted pages, heap files |
| Caching | Buffer pool: 16-way sharded page table, CLOCK-sweep eviction, pin/latch separation, RAII guards |
| Indexing | B+tree: fixed-width `int64` keys, unique, per-index header page holding the root id |
| Concurrency control | SQLite-style: one reader-writer lock per table, held for the whole statement |
| Durability | Explicit `Shutdown()` — flush, then `fsync`. No write-ahead log (see non-goals) |
| SQL front-end | Hand-written lexer and recursive-descent parser, binder with type checking |
| Execution | Volcano (iterator) model; nested-loop, index nested-loop, hash and sort-merge joins |
| Interface | Interactive shell, thread-per-session concurrency |

Three decisions shape everything else:

**Thread-per-session.** One thread carries a statement down the entire stack and back; layers
are a code decomposition, never a scheduling one. Every layer below is therefore synchronous
and blocking by construction ([DD-003](design-docs/DD-003-threading-model.md)).

**Latches are not locks.** Latches protect physical structures — a frame, a page — for
nanoseconds, and are ordered so they cannot deadlock. Locks protect logical state — a table —
for the length of a statement. Conflating them is the classic error, and the distinction is
load-bearing throughout the codebase: the buffer pool and the heap are safe under concurrent
access on their own latches, before any lock exists.

**Coarse locks, so no aborts.** A statement takes its table locks up front, in a fixed order,
and holds them until it finishes. Readers share a table; a writer has it to itself. A fixed
order means no deadlock can form, so nothing ever has to be aborted and rolled back — which is
what lets the engine go without an undo log. It is the model SQLite uses, and the trade is
deliberate: less concurrency between writers, in exchange for a system whose correctness can be
tested end to end.

## Scope (v1)

**SQL surface**

- `CREATE TABLE` with a primary key (backed by the B+tree)
- `INSERT`, `UPDATE`, `DELETE`
- `SELECT` with `WHERE`, `ORDER BY`, and a single `INNER JOIN`
- Types: `INT` (64-bit), `VARCHAR(n)`, with `NULL` support
- Every statement autocommits

**Engine guarantees**

- Statement-level isolation: a statement never observes another's partial writes
- Concurrent sessions with correct latching throughout — the test suite runs under
  ThreadSanitizer and AddressSanitizer in CI
- Every join algorithm returns exactly the rows the nested-loop join does — the nested-loop
  join is the test oracle for the others
- Durability on clean shutdown

## Non-goals

Deliberately out of scope. These are decisions, not omissions — each is recorded with its
reasoning in the relevant design doc.

**No write-ahead log, and therefore no crash recovery.** Durability comes from an explicit
`Shutdown()` that flushes and `fsync`s. A `kill -9` loses every dirty page in the buffer pool,
and because a statement's pages are not flushed atomically, a crash can leave the database
*structurally* inconsistent — a half-applied B+tree split, not merely missing recent writes.
This is the single largest simplification in the project. It is deferred rather than unexamined:
adding a WAL would change the buffer pool's flush sequence (a page could no longer be written
before the log record describing it) and would demote the shutdown flush from the durability
mechanism to a restart-time optimization.

**No multi-statement transactions.** No `BEGIN` / `COMMIT` / `ROLLBACK`, no row-level locking,
no two-phase locking. An earlier plan had strict 2PL with deadlock detection; it was cut
because a deadlock victim must be rolled back, and rollback needs an undo path the heap format
does not have — a deleted slot is zeroed, and re-inserting the row would give it a new RID and
invalidate its index entry. Table-level locks remove the need for aborts entirely.

**No MVCC.** Readers and writers of the same table block each other. MVCC is a tuple-format
decision (version chains or an undo log) plus a mandatory garbage collector, and it reaches
into every layer below the executor.

**No concurrent writers inside the B+tree.** No latch crabbing, no B-link tree. The table lock
already guarantees one writer or many readers per tree, so crabbing would guard against a state
the engine can never reach — and code that cannot be exercised end to end cannot be tested.

**No query optimizer.** No cost model, no statistics, no join ordering. The join algorithm is
chosen by fixed rules, and an index is used only for predicates on the primary key.

**No variable-length index keys**, no duplicate keys in an index, no prefix compression, no
out-of-memory hash join or external sort, no overflow pages — tuples larger than a page are
rejected rather than split.

**No page checksums.** Space is reserved in the page header; the self page id catches
misdirected reads, but not bit rot.

**Not portable across architectures.** Page headers are `memcpy`'d, so the file format is
host-endian and host-ABI.

Also out of scope: distributed anything · subqueries, CTEs, views, triggers, foreign keys ·
`ALTER TABLE`, `DROP TABLE` · aggregates, `GROUP BY` · floating-point types · authentication ·
a network wire protocol.

## Design docs

The reasoning behind each component, including rejected alternatives:

- [DD-001 — Storage file layout](design-docs/DD-001-storage-file-layout.md)
- [DD-002 — Buffer pool: concurrency and latching](design-docs/DD-002-buffer-pool-manager.md)
- [DD-003 — Threading and execution model](design-docs/DD-003-threading-model.md)
- [DD-004 — Heap pages and table heap](design-docs/DD-004-heap-pages-and-table-heap.md)
- DD-005 — B+tree *(in progress)*

## Project layout

```
src/            engine source, one directory per component
test/           GoogleTest suites, mirrors src/
design-docs/    decision records (DD-NNN) — written as each component is built
assets/         logo, branding, diagrams
```

## Building

Requires Clang 17+ with libc++ (the sanitizer presets pin it), CMake ≥ 3.25, Ninja.

```bash
cmake --preset debug && cmake --build --preset debug   # fast development build
ctest --preset debug                                   # run tests

cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # memory checks
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan   # race checks
```

CI runs the ASan and TSan suites plus a clang-format check on every pull request; `main`
only moves through green pipelines.

Then drive the engine by hand — raw pages, or a table on the heap layer:

```bash
./build/debug/kernsql mydb.db
kernsql> tcreate
tcreate: first=2 last=2
kernsql> tinsert hello
tinsert: 5 bytes -> {2, 0} (last page now 2)
kernsql> tinsert world
tinsert: 5 bytes -> {2, 1} (last page now 2)
kernsql> tdelete 2 0
tdelete: ok
kernsql> tscan
  {2, 1} "world"
tscan: 1 rows
kernsql> tdump 2
tdump: page 2 next=-1
       slots=2 live=1 dead_bytes=5 tuple_data_start=4054
       contiguous=4038 reclaimable=4043
       invariants: ok
kernsql> quit
shutdown ok
```

`help` lists every command, including the page-level ones (`new`, `write`, `read`, `flush`).

## References

Standing on the shoulders of: *Database Internals* (Petrov) · *Designing Data-Intensive
Applications* (Kleppmann) · CMU 15-445 · the PostgreSQL documentation and internals guides ·
the SQLite file format and architecture documents.

## License

[MIT](LICENSE) © 2026 Gaurav Kumar

<p align="center">
  <img src="assets/kernsql-lockup.svg" alt="kernSQL" width="380"/>
</p>

<p align="center">
  A minimal SQL database built from scratch in C++23 on Linux —
  storage engine, transactions, and query engine, no shortcuts.
</p>

---

## What is this?

**KernSQL** is a single-node relational database written from first principles: no storage
libraries, no parser generators, no query framework. Every layer — from the bytes on disk to
the SQL shell — is hand-built, tested, and documented.

It is a learning-in-public project. The goal is not to compete with SQLite; it is to deeply
understand how real databases (Postgres in particular) work by building one, and to leave
behind a codebase clean enough that others can learn from it.

The emphasis is **depth over surface area**. The SQL dialect is deliberately tiny; the
concurrency underneath it is not. Every design decision is recorded in
[`design-docs/`](design-docs/) with the alternatives that were rejected and why — those
documents are the most useful thing in this repository.

## Status

| Component | State |
|---|---|
| `DiskManager` — single file, 4 KB pages, persistent free list, thread-safe | done, tested |
| `PageHeader` — 32-byte self-describing header (self page id, format version) | done |
| `Replacer` — CLOCK sweep with capped usage counts | done, tested |
| `BufferPoolManager` — sharded page table, frame state machine, RAII page guards | code complete, **tests pending** |
| Shell — REPL over the buffer pool, one command per operation | done |
| Slotted pages + heap file | next |
| B+tree with latch crabbing | planned |
| Catalog | planned |
| Parser, binder, executor | planned |
| Two-phase locking + deadlock detection | planned |

## Architecture

| Layer | Design |
|---|---|
| Storage | Single-file, page-based (4 KB), self-describing page headers, slotted pages, heap files |
| Caching | Buffer pool: 16-way sharded page table, CLOCK-sweep eviction, pin/latch separation, RAII guards |
| Indexing | B+tree with latch crabbing, fixed-width integer keys |
| Concurrency control | Strict two-phase locking — row-level shared/exclusive locks, wait-for-graph deadlock detection |
| Durability | Explicit `Shutdown()` — flush, then `fsync`. No write-ahead log (see non-goals) |
| SQL front-end | Hand-written lexer and recursive-descent parser, binder with type checking |
| Execution | Volcano (iterator) model, fixed execution strategy — no cost model |
| Interface | Interactive shell, thread-per-session concurrency |

Two decisions shape everything else:

**Thread-per-session.** One thread carries a statement down the entire stack and back; layers
are a code decomposition, never a scheduling one. Every layer below is therefore synchronous
and blocking by construction ([DD-003](design-docs/DD-003-threading-model.md)).

**Latches are not locks.** Latches protect physical structures for nanoseconds and are ordered
to prevent deadlock; locks protect logical database state for the length of a transaction and
deadlock is detected and resolved. Conflating them is the classic error, and the distinction is
load-bearing throughout the codebase.

## Scope (v1)

**SQL surface**

- `CREATE TABLE`
- `INSERT`, `UPDATE`, `DELETE`
- `SELECT` with `WHERE` and a single `INNER JOIN`
- `BEGIN` / `COMMIT` / `ROLLBACK`
- Types: `INT`, `VARCHAR(n)`, with `NULL` support

**Engine guarantees**

- Serializable isolation via strict 2PL; deadlocks are detected and one transaction is aborted
- Concurrent sessions with correct latching throughout — the test suite runs under
  ThreadSanitizer and AddressSanitizer in CI
- Durability on clean shutdown

## Non-goals

Deliberately out of scope. These are decisions, not omissions — each is recorded with its
reasoning in the relevant design doc.

**No write-ahead log, and therefore no crash recovery.** Durability comes from an explicit
`Shutdown()` that flushes and `fsync`s. A `kill -9` loses every dirty page in the buffer pool,
and because a transaction's pages are not flushed atomically, a crash can leave the database
*structurally* inconsistent — a half-applied B+tree split, not merely missing recent writes.
This is the single largest simplification in the project. It is deferred rather than unexamined:
adding a WAL would change the buffer pool's flush sequence (a page could no longer be written
before the log record describing it) and would demote the shutdown flush from the durability
mechanism to a restart-time optimization.

**No MVCC.** Concurrency control is 2PL, so readers block writers and writers block readers.
MVCC was rejected on scope: it is a tuple-format decision (version chains or an undo log) plus
a mandatory garbage collector, and it reaches into layers this project builds later. 2PL is
additive to what already exists.

**No query optimizer.** No cost model, no statistics, no join ordering. An index is used if and
only if the predicate is on the primary key.

**No variable-length index keys**, no prefix compression, no hash join, no overflow pages —
tuples larger than a page are rejected rather than split.

**No page checksums.** Space is reserved in the page header; the self page id catches
misdirected reads, but not bit rot.

**Not portable across architectures.** Page headers are `memcpy`'d, so the file format is
host-endian and host-ABI.

Also out of scope: distributed anything · subqueries, CTEs, views, triggers, foreign keys ·
`ALTER TABLE`, `DROP TABLE` · aggregates, `GROUP BY`, `ORDER BY` · floating-point types ·
authentication · a network wire protocol.

## Design docs

The reasoning behind each component, including rejected alternatives:

- [DD-001 — Storage file layout](design-docs/DD-001-storage-file-layout.md)
- [DD-002 — Buffer pool: concurrency and latching](design-docs/DD-002-buffer-pool-manager.md)
- [DD-003 — Threading and execution model](design-docs/DD-003-threading-model.md)

## Project layout

```
src/            engine source, one directory per component
test/           GoogleTest suites, mirrors src/
design-docs/    decision records (DD-NNN) — written as each component is built
assets/         logo and branding
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

Then drive the engine by hand:

```bash
./build/debug/kernsql mydb.db
kernsql> new
new: allocated page 2
kernsql> write 2 hello
kernsql> read 2
read: page 2 -> "hello"
kernsql> quit
```

## References

Standing on the shoulders of: *Database Internals* (Petrov) · *Designing Data-Intensive
Applications* (Kleppmann) · CMU 15-445 · the PostgreSQL documentation and internals guides.

## License

[MIT](LICENSE) © 2026 Gaurav Kumar

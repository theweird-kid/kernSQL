<p align="center">
  <img src="assets/kernsql-lockup.svg" alt="kernSQL" width="380"/>
</p>

<p align="center">
  A minimal SQL database built from scratch in C++20 on Linux —
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

## Architecture at a glance

KernSQL follows a Postgres-flavored design:

| Layer | Design |
|---|---|
| Storage | Single-file, page-based (4 KB), slotted pages, heap files |
| Caching | Buffer pool with LRU-K replacement, pin/latch semantics |
| Indexing | B+tree with latch crabbing for concurrent access |
| Concurrency control | MVCC with snapshot isolation — in-heap version chains (`xmin`/`xmax`), first-updater-wins |
| Durability | Redo-only write-ahead log, checkpoints, crash recovery, vacuum |
| SQL front-end | Hand-written lexer and recursive-descent parser, binder with type checking |
| Execution | Volcano (iterator) model, rule-based planner with predicate pushdown and index selection |
| Interface | Interactive shell, thread-per-session concurrency |

A key consequence of this combination: **rollback writes nothing to the heap.** Aborted
transactions simply become invisible — the same property that makes redo-only logging
sufficient in Postgres.

## Features (v1 scope)

**SQL surface**

- `CREATE TABLE` / `DROP TABLE`
- `INSERT`, `UPDATE`, `DELETE`
- `SELECT` with `WHERE`, `ORDER BY`, `LIMIT`, `INNER JOIN`
- `GROUP BY` with `COUNT` / `SUM` / `AVG` (stretch)
- `BEGIN` / `COMMIT` / `ROLLBACK`
- Types: `BIGINT`, `VARCHAR(n)`, `BOOLEAN` — with `NULL` support

**Engine guarantees**

- Snapshot isolation for all transactions; readers never block writers
- Crash safety: committed data survives `kill -9` (WAL replay on restart)
- Concurrent sessions with correct latching throughout — the full test suite runs under
  ThreadSanitizer and AddressSanitizer in CI
- System catalog stored in the database itself, as regular tables

## Non-goals (v1)

Deliberately out of scope, so the core stays finishable and understandable:

distributed anything · cost-based optimization · subqueries, CTEs, views, triggers,
foreign keys · `ALTER TABLE` · floating-point types · authentication · network wire
protocol.

## Project layout

```
src/            engine source, one directory per component
test/           GoogleTest suites, mirrors src/
design-docs/    decision records (DD-NNN) — written as each component is built
assets/         logo and branding
```

## Building

Requires GCC 13+ (or Clang 17+), CMake ≥ 3.25, Ninja.

```bash
cmake --preset debug && cmake --build --preset debug   # fast development build
ctest --preset debug                                   # run tests

cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # memory checks
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan   # race checks
```

CI runs the ASan and TSan suites plus a clang-format check on every pull request; `main`
only moves through green pipelines.

## Status

🚧 Early days — foundation phase. Progress, design decisions, and write-ups are tracked in
[`design-docs/`](design-docs/) as each component lands, feature by feature, branch by branch.

## References

Standing on the shoulders of: *Database Internals* (Petrov) · *Designing Data-Intensive
Applications* (Kleppmann) · CMU 15-445 · the PostgreSQL documentation and internals guides.

## License

[MIT](LICENSE) © 2026 Gaurav Kumar

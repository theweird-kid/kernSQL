# Executor

**Status:** Accepted 2026-11-15 — implemented on `feature/executor`.
**Component:** `src/sql/executor` (`Bind`, `Evaluate` in `binder.hpp`; the operators in
`operators.hpp`; `PlanScan`, `PlanSelect` in `planner.hpp`; `Executor`, `QueryResult`,
`FormatResult`, `SplitStatements` in `executor.hpp`), and the SQL shell in `src/shell/main.cpp`.

Runs SQL. Takes the parser's AST ([DD-007](./DD-007-sql-parser.md)), checks it against the
catalog ([DD-006](./DD-006-catalog-and-tables.md)), picks an access path, and pulls rows through
a tree of operators. After this stage kernSQL is a database you can type at.

## Scope

In: `CREATE TABLE`, `INSERT ... VALUES`, `SELECT` with a projection and a `WHERE` of `AND`ed
comparisons, `DELETE ... WHERE`, `EXPLAIN` on `SELECT` and `DELETE`, a rule-based choice between
a sequential scan and a primary-key index range scan, and a SQL REPL.

Out until the joins stage: `JOIN` and `ORDER BY`. They parse (DD-007 decision 1), and the binder
rejects them with "not supported yet". Out entirely: transactions, `UPDATE`, aggregates, a cost
model, and concurrency (stage 5 adds per-table locks).

## Pipeline

```
"SELECT name FROM people WHERE id >= 2 AND age > 26"
  │  Parser::Parse                      DD-007: form
  ▼
SelectStmt{ columns=[name], table=people, where=[id >= 2, age > 26] }
  │  Bind                               names → positions, types checked
  ▼
BoundSelect{ table=Table*, projection=[0], where=[#1 >= 2, #2 > 26] }
  │  PlanSelect                         access path + filter + projection
  ▼
Projection(name) ← Filter(age > 26) ← IndexScan(people, id in [2, MAX])
  │  Next() until false
  ▼
QueryResult{ columns=[name], rows=[...], tag="SELECT 2" }
```

Each stage only reads what the one before produced: the planner never sees a name, and the
operators never see the AST.

## Binder

The parser checked **form**. The binder checks **meaning**, and leaves nothing for later:

- the table exists (`kNotFound`, "no such table: x");
- every column exists in it (`kNotFound`), and a qualified `t.col` names the `FROM` table — there
  are no aliases and, until joins, no second table;
- both sides of every comparison have the same type. **No implicit casts**: `id = '5'` is an
  error, as in Postgres, rather than SQLite's type-affinity conversion;
- `CREATE TABLE` goes through `Schema::Create`, which owns every schema rule (PK must be INT64,
  `VARCHAR(n)` bounds, duplicate column names, the 2000-byte row cap). The binder restates none
  of them;
- `INSERT` is checked **as a whole** before any row goes in (see [Atomicity](#atomicity)).

The output names columns by **position** in the schema (`BoundColumn{index}`), so evaluating a
predicate is an array index, not a name lookup per row.

## Planner

Rule-based, one decision: how to reach the rows.

1. **Access path.** Every `WHERE` term that compares the primary key with an integer literal,
   either way round (`id = 5`, `3 < id`), narrows one inclusive key range `[lo, hi]`. If there is
   at least one such term the scan is an `IndexScan` over that range, and those terms are
   **dropped**, because the range already enforces them. Otherwise it is a `SeqScan`.
2. **Filter** over the terms left, if any.
3. **Projection**, unless the query is `SELECT *`.

Range rules: `=` sets both ends, `<=`/`>=` one end, and strict `<`/`>` step the bound by one key
first. Stepping is impossible past the type's end, so `id < INT64_MIN` and `id > INT64_MAX` mark
the range **empty** instead of overflowing. Contradictions (`id > 5 AND id < 3`, `id = 1 AND
id = 2`) also give an empty range, and the IndexScan returns nothing without touching a page.
`!=` never narrows: it is a hole in the range, not a range, so it stays in the filter.

No statistics and no costs. For this engine an index range is never worse than a full scan,
because the heap has no order a scan could exploit. A cost model starts to matter with joins,
where the choice is between algorithms.

## Operators

The Volcano model (Graefe, 1994): every operator answers `Next(row)`, which returns true with the
next row or false at the end. A parent pulls from its child one row at a time. Postgres's
`ExecProcNode` works this way.

| Operator | Next() |
|---|---|
| `SeqScan` | next heap row (`Table::Scan`), decoded |
| `IndexScan` | next `(key, RID)` in range (`Table::ScanIndex`), row fetched with `RowAt(rid)` |
| `Filter` | child rows until one passes every predicate |
| `Projection` | child row, cut down to the chosen columns |

Scans open their iterator on the first `Next()`, so building a plan touches no page. That is what
lets `EXPLAIN` build the real plan and print it without running anything.

`IndexScan` reports an index entry whose RID names an empty slot as `kCorruption`, not as a
missing row, matching `Table::Get`.

## Collect, then mutate

A live scan keeps its current page **read-latched** between `Next()` calls. `Table::Delete`
write-latches pages, so deleting while the scan is alive would wait on the scan's own latch
forever. `DELETE` therefore:

1. runs the scan to the end, collecting primary keys;
2. **destroys the operator tree**, which releases its guard;
3. deletes each key through `Table::Delete`.

This is also the standard fix for the Halloween problem (System R, 1976): a statement that changes
rows while scanning them can meet its own changes. Collecting first fixes the set of rows before
the first one changes.

## Atomicity

There is no WAL (DD-006), so a statement cannot be rolled back. Within that limit:

- **INSERT** is all-or-nothing for every error the binder can foresee. Every row passes
  `Schema::Validate`, no key appears twice in the `VALUES` list, and no key is already in the
  table — all checked before the first `Table::Insert`. What remains is an I/O or pool error
  midway, which leaves the rows done so far.
- **DELETE** has only that storage-layer failure mode once its keys are collected.

## Results

`QueryResult{columns, rows, tag}`. `tag` is the Postgres command tag: `CREATE TABLE`, `INSERT n`,
`DELETE n`, `SELECT n`, `EXPLAIN`. `EXPLAIN` returns one `plan` column, one row per plan line,
with each child indented two spaces:

```
 plan
--------------------------------
 Projection: name
   Filter: age > 26
     IndexScan: people (id >= 2)
```

`FormatResult` prints psql-style tables: integers right-aligned, strings left-aligned, no trailing
spaces, and an `(n rows)` footer.

## Shell

`src/shell/main.cpp` is now a SQL REPL; the old page-poking REPL is gone. Input is buffered until
it contains a `;` **token** (`SplitStatements`), so a `;` inside a string or comment never ends a
statement. Several statements can share a line, and one can span many. Lines starting with `.`
are shell commands, as in sqlite3: `.tables`, `.schema t`, `.check t`, `.stats`, `.help`, `.quit`.

**Autocommit.** The engine is durable only at `Shutdown` (DD-002), so a shell killed by Ctrl-C
used to lose every statement since it started, even ones whose results it had printed. Now every
statement except `SELECT` and `EXPLAIN` is followed by `Database::Checkpoint()` (flush every
dirty page, then fsync) **before** its result prints: what the screen says happened is on disk,
and survives `kill -9`. This is sqlite3's per-statement commit, without the journal that would
make it atomic. With no WAL, a crash *during* the flush can still leave a torn state. Ctrl-C
cancels the statement being typed, as in psql, instead of ending the process.

The executor holds no pin between statements, so DD-003's rule that a pin never spans a client
round-trip holds by construction. That is also why `Checkpoint` needs no quiescence check.

## Testing

`test/sql/executor_test.cpp`, against a real file and pool, index fanout `MIN_FANOUT` so a few
dozen rows make a deep tree. Every statement in the tests is followed by a check that **no frame
is pinned**, and mutating tests also run `CheckConsistency()`.

- Basics: `SELECT *` column order, projection and reordering, case folding, strings with quotes
  and newlines, INT64 edges as keys, empty tables.
- Binder errors with their codes, and that nothing changed afterwards.
- `INSERT` all-or-nothing: a bad row 2 never leaves row 1 behind.
- `EXPLAIN` plans for every planner rule: equality, ranges, mirrored literals, empty ranges, `!=`,
  column-vs-column, residual filters, `DELETE`.
- `DELETE` by key, by range and by filter, including all 300 rows of a multi-level table. That
  test would deadlock, not fail, if the delete did not collect first.
- Persistence across a reopen, and across a real `_exit` without `Shutdown` after a
  `Checkpoint` (a forked child, since in-process a missing `Shutdown` aborts by design).
- **Differential:** 400 random `WHERE` clauses (key ranges with INT64 edges, non-key columns,
  strings, column-vs-column, either side) against a `std::map` evaluated by hand, with random
  `DELETE`s mixed in. Planting bugs in the planner was caught every time: `<` treated as `<=`,
  a literal on the left not mirrored, and a missing `> INT64_MAX` guard.
- `FormatResult` and `SplitStatements`, each with exact expected output.

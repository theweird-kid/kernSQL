# SQL Parser

**Status:** Accepted 2026-11-14 — all seven [decisions](#decisions-to-confirm) confirmed.
**Component:** `src/sql/parser` (`Token`, `Lexer` in `token.hpp`; the AST and `ToSql` in
`ast.hpp`; `Parser` in `parser.hpp`)

Turns one SQL statement — a string — into an **AST**: plain structs that say what the statement
asks for, with whitespace, case, quoting and comments gone. The executor stage's binder takes the
AST, resolves names against the catalog and builds a plan.

Deliberately short. A parser invents no on-disk structure, no latching rule and no failure
ordering; the grammar below is the whole design.

## Scope

In: `CREATE TABLE`, `INSERT ... VALUES`, `SELECT`, `DELETE`, `EXPLAIN`. `WHERE` as a list of
comparisons joined by `AND`. One `JOIN ... ON`, and `ORDER BY` — parsed now, executed in the
joins stage (see [Decisions to confirm](#decisions-to-confirm)).

Out, deliberately: `OR`, `NOT`, parentheses in expressions, arithmetic, `UPDATE`, `DROP`,
`ALTER`, aggregates, `GROUP BY`, `LIMIT`, subqueries, table aliases (`FROM t AS a`), `t.*`,
quoted identifiers (`"Foo"`), block comments, decimal numbers, several statements in one input.

## Pipeline

```
"SELECT id FROM t WHERE id = 5"
  │  Lexer::Tokenize — characters → tokens
  ▼
[SELECT] [IDENT id] [FROM] [IDENT t] [WHERE] [IDENT id] [=] [INTEGER 5] [END]
  │  Parser::Parse — tokens → AST, by recursive descent
  ▼
ParsedStatement{ explain=false, SelectStmt{ columns=[id], table=t, where=[id = 5] } }
```

Two passes, not one: the lexer handles characters (whitespace, quotes, digits, case), the parser
handles structure, and the parser never sees a space or a quote. The lexer runs to completion
before the parser starts; one statement is a few dozen tokens, so a token vector costs nothing
and gives the parser free lookahead.

## Grammar

`[x]` optional, `{x}` zero or more, `|` alternatives, UPPERCASE a keyword.

```
statement   := [EXPLAIN] (create | insert | select | delete) [';'] END
create      := CREATE TABLE ident '(' coldef { ',' coldef } ')'
coldef      := ident type [PRIMARY KEY]
type        := INT64 | VARCHAR '(' integer ')'
insert      := INSERT INTO ident VALUES row { ',' row }
row         := '(' literal { ',' literal } ')'
select      := SELECT projection FROM ident [join] [where] [orderby]
projection  := '*' | colref { ',' colref }
join        := JOIN ident ON comparison
where       := WHERE comparison { AND comparison }
orderby     := ORDER BY sortkey { ',' sortkey }
sortkey     := colref [ASC | DESC]
delete      := DELETE FROM ident [where]
comparison  := operand op operand
operand     := colref | literal
colref      := ident [ '.' ident ]
literal     := ['-'] integer | string
op          := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>='
```

**LL(1):** at every choice, the next single token decides which way to go — `*` or not after
`SELECT`, identifier or literal for an operand, `.` or not after an identifier. No backtracking.

**No rule refers back to itself** (no parentheses, no nested expressions), so the call depth is
bounded by the grammar itself (about 6 calls) whatever the input. Adding parenthesised
expressions later would need a depth limit. Postgres calls `check_stack_depth()` in its
expression code for this reason, and SQLite caps expression depth with `SQLITE_MAX_EXPR_DEPTH`.

## Lexer rules

- **Words** are `[A-Za-z_][A-Za-z0-9_]*`, read to the longest match (`selectx` is one
  identifier), lowercased, then looked up in the keyword table. In the table → that keyword's
  token; otherwise → `kIdentifier` with the lowercased text. This is what makes names
  case-insensitive (DD-006, "Names"); the identifier rule after lowercasing is exactly
  `IsValidName`'s character set, length aside.
- **Keywords are reserved**: `CREATE TABLE order (...)` is a syntax error ("expected table name,
  found ORDER"). Postgres splits keywords into reserved and unreserved classes to avoid this; the
  extra grammar is not worth it here.
- **`INT` and `INT64`** both lex to `kInt64`. `ToSql` prints `INT64`.
- **Integers** are decimal digits only, kept as text. The lexer never sees a sign: `-5` is
  `[MINUS] [INTEGER 5]`. A digit run followed directly by a letter or `_` (`123abc`) is an error.
- **Strings** are single-quoted; `''` inside is one quote; no other escapes; any byte (newline
  included) is allowed inside. The token's text is the unescaped content. End of input inside a
  string is an error at the opening quote.
- **Comments**: `--` to end of line. No block comments.
- **Operators**: longest match first — `<=` `<>` before `<`, `>=` before `>`, `!=`; a lone `!`
  is an error. `!=` and `<>` both lex to `kNotEqual`.
- Any other byte outside a string, including every non-ASCII byte, is an error.
- The token stream always ends with exactly one `kEnd`, at offset `sql.size()`.

## Literals and the INT64_MIN trap

The parser builds an integer from `['-'] INTEGER`: convert the digits to a `uint64_t` magnitude
(`std::from_chars`, error on overflow), then apply the sign. Positive must be ≤ `INT64_MAX`;
negative must be ≤ 2^63, and exactly 2^63 is `INT64_MIN`. Negating a parsed `int64_t` instead
cannot accept `-9223372036854775808`, because `9223372036854775808` alone does not fit an int64.

`VARCHAR(n)` takes an unsigned integer that must fit `uint16_t`; whether n is in
`1 .. MAX_VARCHAR_LENGTH` is `Schema::Create`'s rule, not the parser's.

## What the parser checks, and what it leaves to the binder

The parser checks **form**: the token sequence matches the grammar, integers fit their type, and
a `CREATE TABLE` has **exactly one** `PRIMARY KEY` column. That last one is structural (it counts
clauses), and the parser can point at the second `PRIMARY KEY` or the closing `)`.

It does **not** check **meaning**: whether a table or column exists, whether a qualified name's
table is in `FROM`, types of values against columns, `INSERT` arity, the primary key being INT64,
`ON` being an equality between two columns, name length, `VARCHAR` limits. Those need the catalog
or belong to `Schema::Create`. Postgres splits the work the same way (`gram.y` builds a raw parse
tree; `parse_analyze` resolves names), and so does SQLite (`parse.y`, then `sqlite3Resolve*`).

`EXPLAIN` is accepted only before `SELECT` and `DELETE`: `CREATE` and `INSERT ... VALUES` have no
plan worth showing.

## AST

Plain structs and `std::variant` (`ast.hpp`), compared with `operator== = default`. No class
hierarchy, no visitors: a `std::visit` over five statement kinds is the whole dispatch the
executor needs.

- Literals are `Value` and column definitions are `Column`, both from `schema.hpp`, so the binder
  and the catalog read exactly what the parser wrote, with no conversion step.
- `ColumnRef{table, column}`, with `table` empty when unqualified.
- `SelectStmt::columns` empty means `SELECT *`. The grammar can produce an empty list no other way.
- `WHERE` is `std::vector<Comparison>`, an implicit `AND` of every element; empty means no `WHERE`.

## Errors

The parser stops at the first error. `kInvalidArgument`, message:

```
syntax error at line 1, column 15: expected FROM, found identifier 'form'
```

Line and column are 1-based, column in bytes, both computed from the offending token's byte
offset (`FormatSyntaxError`). Error recovery, which compilers use to report many errors in one
run, is pointless for one-line statements.

The final `expect(kEnd)` is part of the grammar. Without it, `SELECT * FROM t garbage` would
parse successfully and the trailing `garbage` would be dropped without any error.

## Canonical SQL: `ToSql`

`ToSql(const ParsedStatement&)` prints the AST back as SQL in one fixed form: uppercase keywords,
`, ` between list items, one space around operators, strings re-quoted with `''`, `INT64` for
both spellings, no trailing `;`. It serves three uses:

1. **Golden tests** compare `ToSql(Parse(input))` against an expected string.
2. **Round trip**: `Parse(ToSql(ast)) == ast` for every AST the parser produced.
3. **`EXPLAIN`** (executor stage) prints predicates with `ToSql(const Comparison&)`.

## Testing

Pure functions over strings, so no files and no pool are involved: `test/sql/lexer_test.cpp`,
`test/sql/parser_test.cpp`.

- **Lexer:** every token type; keyword case folding; `selectx` as an identifier; `''` inside
  strings, the empty string, a newline inside a string; unterminated string; `123abc`; `<` `<=`
  `<>` `>` `>=` `!=` and a lone `!`; comments, including one at end of input; non-ASCII outside a
  string; offsets of every token.
- **Golden:** a table of `{input, expected ToSql}` pairs, one per grammar feature, written in
  messy case and spacing so normalisation is tested too.
- **Round trip** over every golden case.
- **Errors:** a table of `{input, message substring, line, column}`: a missing `FROM`, trailing
  garbage, empty input, a keyword used as a name, zero or two `PRIMARY KEY`s, `EXPLAIN INSERT`,
  `VARCHAR(70000)`, `-` before a string.
- **Integer edges:** `INT64_MAX`, `INT64_MIN`, one beyond each, `-0`.
- **Every prefix:** cut each golden input before each of its tokens. A prefix may legitimately
  parse (`SELECT * FROM t` is a prefix of `... WHERE ...`), and then must round-trip; otherwise it
  must fail *at its end*, never earlier. A separate list of cut-short statements (`INSERT INTO t
  VALUES (1`) must be rejected, which catches rules that accept a statement cut short.
- **Garbage:** seeded random sequences drawn from every token's spelling, ~10k iterations under
  ASan: each must return `ok` or `kInvalidArgument` and never crash.

## Non-goals

A parser generator (Bison, Lemon). Postgres (`gram.y`, roughly 18k lines) and SQLite (`parse.y`)
need one at their size. At 18 rules, a hand-written parser is shorter than the generator setup,
gives readable error messages for free, and is the approach ClickHouse, GCC and Clang take. A
Pratt or precedence-climbing expression parser is not needed either: the flat `AND` list has no
precedence to resolve.

## Decisions to confirm

Confirmed by the user 2026-11-14. Each is cheap to reverse now and expensive after the executor exists.

1. **Parse `JOIN` and `ORDER BY` now**, though they run only in the joins stage. The binder
   rejects them until then (`kInvalidArgument`, "not supported yet"). The reason is that the AST
   shape stays frozen through mid-December.
2. **`WHERE` is a flat `AND` list, not an expression tree.** `ON` uses the same `Comparison`, and
   the planner's "is there `pk = literal`?" is a loop over the list. Adding `OR` later means
   replacing the list with a tree.
3. **At most one `JOIN`.** Two-table joins are what the join-algorithm work needs. A chain would
   need join-order planning as well.
4. **Keywords fully reserved**, and `INT` accepted as a synonym of `INT64`.
5. **Exactly one `PRIMARY KEY` is a parse error otherwise**; its type is left to `Schema::Create`.
6. **`EXPLAIN` only before `SELECT` / `DELETE`.**
7. **One statement per input**, optional trailing `;`. Splitting a multi-line REPL input on `;`
   is the shell's job (executor stage).

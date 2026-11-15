#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/schema.hpp"
#include "catalog/table.hpp"
#include "common/status.hpp"
#include "sql/parser/ast.hpp"

namespace kernsql {

/*
 * The binder (DD-008, "Binder"): the parser's AST checked against the catalog.
 *
 * The parser checked FORM: the tokens make a statement. The binder checks MEANING: the table
 * exists, every column exists in it, both sides of every comparison have the same type, and the
 * statement uses nothing the executor cannot run yet. What comes out names columns by POSITION in
 * the schema, so nothing after this point ever looks a name up again.
 *
 * Postgres splits the same way (gram.y builds a raw parse tree, parse_analyze resolves it into a
 * Query); so does SQLite (parse.y, then sqlite3ResolveExprNames).
 *
 * Errors, all before anything is touched:
 *   kNotFound         a table or column that does not exist
 *   kInvalidArgument  everything else — a type mismatch, JOIN / ORDER BY (until the joins stage),
 *                     a qualified name whose table is not the FROM table, an INSERT row that fails
 *                     Schema::Validate, an invalid CREATE TABLE schema
 *   kDuplicateKey     CREATE TABLE of an existing name, or an INSERT whose key is already present
 *                     or appears twice in its own VALUES list
 */

// One side of a bound comparison: a column of the row, by position, or a literal.
struct BoundColumn {
	uint16_t index{0};
	bool operator==(const BoundColumn&) const = default;
};
using BoundOperand = std::variant<BoundColumn, Value>;

struct BoundComparison {
	BoundOperand lhs;
	CompareOp op{CompareOp::kEqual};
	BoundOperand rhs;

	// The comparison as written, for EXPLAIN (ToSql). Never consulted to evaluate.
	Comparison source;
};

struct BoundCreateTable {
	std::string name;
	Schema schema;  // already through Schema::Create
};

struct BoundInsert {
	Table* table{nullptr};
	std::vector<Row> rows;  // every row has passed Schema::Validate; keys are distinct and new
};

struct BoundSelect {
	Table* table{nullptr};
	// Positions to output, in order. EMPTY MEANS EVERY COLUMN, in schema order (SELECT *).
	std::vector<uint16_t> projection{};
	std::vector<BoundComparison> where{};  // implicit AND, as in the AST
};

struct BoundDelete {
	Table* table{nullptr};
	std::vector<BoundComparison> where{};
};

using BoundStatement = std::variant<BoundCreateTable, BoundInsert, BoundSelect, BoundDelete>;

/*
 * Bind `statement` against `catalog`. Reads only: it never creates a table or inserts a row.
 *
 * INSERT is checked as a whole before any row goes in — every row validated, every key checked
 * against the table and against the other rows — so the only way an INSERT can stop halfway is
 * an I/O or pool error from the storage layers (no WAL, DD-006).
 */
[[nodiscard]] Result<BoundStatement> Bind(const Statement& statement, Catalog& catalog);

/*
 * Evaluate a bound comparison against a row of the table it was bound to. Both sides have the
 * same type — the binder guarantees it — so this is a plain comparison: integers numerically,
 * strings in memcmp order (DD-006's collation).
 */
[[nodiscard]] bool Evaluate(const BoundComparison& comparison, const Row& row);

}  // namespace kernsql

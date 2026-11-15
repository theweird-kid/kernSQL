#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "catalog/schema.hpp"

namespace kernsql {

/*
 * The parser's output (DD-007, "AST"): what a statement asks for, with spelling gone. Plain
 * structs, compared with defaulted ==, so a test can build the expected tree by hand.
 *
 * Nothing here has been checked against the catalog. A ColumnRef may name a column that does not
 * exist; that is the binder's to find (DD-007, "What the parser checks").
 */

// `col` or `table.col`. Names are lowercase (the lexer folds them).
struct ColumnRef {
	std::string table;  // empty when unqualified
	std::string column;

	bool operator==(const ColumnRef&) const = default;
};

enum class CompareOp : uint8_t {
	kEqual,
	kNotEqual,
	kLess,
	kLessEqual,
	kGreater,
	kGreaterEqual,
};

// One side of a comparison: a column or a literal. Literals are schema.hpp's Value, so the
// binder compares them against decoded rows with no conversion.
using Operand = std::variant<ColumnRef, Value>;

// `lhs op rhs`. Both sides may be columns (`a.x = b.y`) or literals (`1 = 1`); the parser accepts
// either, and the binder decides what it can execute.
struct Comparison {
	Operand lhs;
	CompareOp op{CompareOp::kEqual};
	Operand rhs;

	bool operator==(const Comparison&) const = default;
};

// CREATE TABLE name (coldef, ...). Maps straight onto Schema::Create(columns, primary_key).
struct CreateTableStmt {
	std::string table;
	std::vector<Column> columns;  // INT64: max_length 0. VARCHAR(n): max_length n, unvalidated.

	// Index into `columns` of the one PRIMARY KEY column. The parser guarantees there is exactly
	// one; whether it is INT64 is Schema::Create's rule.
	uint16_t primary_key{0};

	bool operator==(const CreateTableStmt&) const = default;
};

// INSERT INTO table VALUES (...), (...). Arity and types unchecked: the binder runs
// Schema::Validate on each row.
struct InsertStmt {
	std::string table;
	std::vector<Row> rows;  // at least one, each with at least one value

	bool operator==(const InsertStmt&) const = default;
};

// JOIN table ON comparison. Parsed now, executed in the joins stage (DD-007, decision 1).
struct JoinClause {
	std::string table;
	Comparison on;

	bool operator==(const JoinClause&) const = default;
};

struct SortKey {
	ColumnRef column;
	bool descending{false};  // ASC, the default, is false

	bool operator==(const SortKey&) const = default;
};

struct SelectStmt {
	// The projection. EMPTY MEANS `SELECT *`; the grammar cannot produce an empty list otherwise.
	std::vector<ColumnRef> columns;

	std::string table;
	std::optional<JoinClause> join;

	// Implicit AND of every element; empty means no WHERE (DD-007, decision 2).
	std::vector<Comparison> where;

	std::vector<SortKey> order_by;  // empty means no ORDER BY

	bool operator==(const SelectStmt&) const = default;
};

struct DeleteStmt {
	std::string table;
	std::vector<Comparison> where;  // as SelectStmt::where; empty deletes every row

	bool operator==(const DeleteStmt&) const = default;
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, DeleteStmt>;

// What Parser::Parse returns. `explain` is set only on a SelectStmt or DeleteStmt body.
struct ParsedStatement {
	bool explain{false};
	Statement body;

	bool operator==(const ParsedStatement&) const = default;
};

/*
 * Canonical SQL (DD-007, "Canonical SQL"): uppercase keywords, ", " between list items, one space
 * around operators, strings re-quoted with '' escaping, INT64 for both type spellings, no
 * trailing ';'.
 *
 * The property the tests hold it to: Parse(ToSql(s)) == s for every s the parser produced.
 */
[[nodiscard]] std::string ToSql(const ParsedStatement& statement);
[[nodiscard]] std::string ToSql(const Comparison& comparison);  // EXPLAIN prints predicates
[[nodiscard]] std::string ToSql(const ColumnRef& column);
[[nodiscard]] std::string ToSql(const Value& value);  // 42, -7, 'it''s'
[[nodiscard]] std::string_view ToSql(CompareOp op);   // "=", "!=", "<", ...

}  // namespace kernsql

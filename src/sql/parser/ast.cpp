#include "sql/parser/ast.hpp"

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace kernsql;

/*
 * The canonical printer (DD-007, "Canonical SQL"). Every choice here is one fixed spelling the
 * parser reads back to the same AST: uppercase keywords, ", " separators, single spaces, INT64
 * for both type spellings, `!=` for both not-equal spellings, ASC never printed (it is the
 * default), no trailing ';'.
 */

namespace {

std::string OperandSql(const Operand& operand) {
	return std::visit([](const auto& side) { return kernsql::ToSql(side); }, operand);
}

// `items` printed with `print`, joined by ", ".
template <typename T, typename Print>
std::string Join(const std::vector<T>& items, Print print) {
	std::string out;
	for (std::size_t i = 0; i < items.size(); ++i) {
		if (i > 0) {
			out += ", ";
		}
		out += print(items[i]);
	}
	return out;
}

// ` WHERE a AND b ...`, or nothing for an empty list.
std::string WhereClause(const std::vector<Comparison>& where) {
	std::string out;
	for (std::size_t i = 0; i < where.size(); ++i) {
		out += (i == 0) ? " WHERE " : " AND ";
		out += ToSql(where[i]);
	}
	return out;
}

std::string ColumnDefSql(const Column& column, bool primary_key) {
	std::string out = column.name;
	switch (column.type) {
		case ColumnType::kInt64:
			out += " INT64";
			break;
		case ColumnType::kVarchar:
			out += " VARCHAR(" + std::to_string(column.max_length) + ")";
			break;
	}
	if (primary_key) {
		out += " PRIMARY KEY";
	}
	return out;
}

std::string StatementSql(const CreateTableStmt& create) {
	std::string out = "CREATE TABLE " + create.table + " (";
	for (std::size_t i = 0; i < create.columns.size(); ++i) {
		if (i > 0) {
			out += ", ";
		}
		out += ColumnDefSql(create.columns[i], i == create.primary_key);
	}
	return out + ")";
}

std::string StatementSql(const InsertStmt& insert) {
	const auto row_sql = [](const Row& row) {
		return "(" + Join(row, [](const Value& v) { return kernsql::ToSql(v); }) + ")";
	};
	return "INSERT INTO " + insert.table + " VALUES " + Join(insert.rows, row_sql);
}

std::string StatementSql(const SelectStmt& select) {
	std::string out = "SELECT ";
	out += select.columns.empty()
	           ? "*"
	           : Join(select.columns, [](const ColumnRef& c) { return kernsql::ToSql(c); });
	out += " FROM " + select.table;
	if (select.join.has_value()) {
		out += " JOIN " + select.join->table + " ON " + ToSql(select.join->on);
	}
	out += WhereClause(select.where);
	if (!select.order_by.empty()) {
		out += " ORDER BY " + Join(select.order_by, [](const SortKey& key) {
			       return ToSql(key.column) + (key.descending ? " DESC" : "");
		       });
	}
	return out;
}

std::string StatementSql(const DeleteStmt& del) {
	return "DELETE FROM " + del.table + WhereClause(del.where);
}

}  // namespace

std::string kernsql::ToSql(const ParsedStatement& statement) {
	std::string body = std::visit([](const auto& s) { return StatementSql(s); }, statement.body);
	return statement.explain ? "EXPLAIN " + body : body;
}

std::string kernsql::ToSql(const Comparison& comparison) {
	return OperandSql(comparison.lhs) + " " + std::string(ToSql(comparison.op)) + " " +
	       OperandSql(comparison.rhs);
}

std::string kernsql::ToSql(const ColumnRef& column) {
	return column.table.empty() ? column.column : column.table + "." + column.column;
}

std::string kernsql::ToSql(const Value& value) {
	if (const int64_t* integer = std::get_if<int64_t>(&value)) {
		// INT64_MIN prints as -9223372036854775808, which ParseLiteral reads back exactly.
		return std::to_string(*integer);
	}
	const std::string& text = std::get<std::string>(value);
	std::string out = "'";
	for (char c : text) {
		if (c == '\'') {
			out += "''";  // the only escape the lexer knows
		} else {
			out += c;
		}
	}
	return out + "'";
}

std::string_view kernsql::ToSql(CompareOp op) {
	switch (op) {
		case CompareOp::kEqual:
			return "=";
		case CompareOp::kNotEqual:
			return "!=";
		case CompareOp::kLess:
			return "<";
		case CompareOp::kLessEqual:
			return "<=";
		case CompareOp::kGreater:
			return ">";
		case CompareOp::kGreaterEqual:
			return ">=";
	}
	std::unreachable();
}

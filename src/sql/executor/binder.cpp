#include "sql/executor/binder.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/types.hpp"
#include "indices/b_plus_tree_page.hpp"

using namespace kernsql;

namespace {

std::string_view TypeName(ColumnType type) {
	return type == ColumnType::kInt64 ? "INT64" : "VARCHAR";
}

ColumnType TypeOf(const Value& value) {
	return std::holds_alternative<int64_t>(value) ? ColumnType::kInt64 : ColumnType::kVarchar;
}

Result<Table*> LookUpTable(Catalog& catalog, const std::string& name) {
	Result<Table*> table = catalog.GetTable(name);
	if (!table.has_value()) {
		if (table.error().code() == ErrorCode::kNotFound) {
			return std::unexpected(Status::NotFound(std::format("no such table: {}", name)));
		}
		return std::unexpected(std::move(table.error()));
	}
	return table;
}

// `t.col` or `col` → its position in `table`. A qualifier must name the FROM table: there are no
// aliases and, until the joins stage, no second table.
Result<uint16_t> BindColumn(const ColumnRef& ref, const Table& table) {
	if (!ref.table.empty() && ref.table != table.Name()) {
		return std::unexpected(Status::InvalidArgument(std::format(
		    "column {} names table '{}', which is not in FROM", ToSql(ref), ref.table)));
	}
	const std::optional<uint16_t> index = table.GetSchema().ColumnIndex(ref.column);
	if (!index.has_value()) {
		return std::unexpected(Status::NotFound(
		    std::format("no such column: {} (table {})", ref.column, table.Name())));
	}
	return *index;
}

struct BoundSide {
	BoundOperand operand;
	ColumnType type;
};

Result<BoundSide> BindOperand(const Operand& operand, const Table& table) {
	if (const auto* ref = std::get_if<ColumnRef>(&operand)) {
		Result<uint16_t> index = BindColumn(*ref, table);
		if (!index.has_value()) return std::unexpected(std::move(index.error()));
		return BoundSide{BoundColumn{*index}, table.GetSchema().Columns()[*index].type};
	}
	const Value& literal = std::get<Value>(operand);
	return BoundSide{literal, TypeOf(literal)};
}

Result<std::vector<BoundComparison>> BindWhere(const std::vector<Comparison>& where,
                                               const Table& table) {
	std::vector<BoundComparison> bound;
	bound.reserve(where.size());
	for (const Comparison& comparison : where) {
		Result<BoundSide> lhs = BindOperand(comparison.lhs, table);
		if (!lhs.has_value()) return std::unexpected(std::move(lhs.error()));
		Result<BoundSide> rhs = BindOperand(comparison.rhs, table);
		if (!rhs.has_value()) return std::unexpected(std::move(rhs.error()));
		// No implicit casts: `id = '5'` is an error, not a string-to-integer conversion. SQLite's
		// type affinity would accept it; Postgres rejects it. One rule, no surprises.
		if (lhs->type != rhs->type) {
			return std::unexpected(Status::InvalidArgument(
			    std::format("type mismatch in {}: {} compared with {}", ToSql(comparison),
			                TypeName(lhs->type), TypeName(rhs->type))));
		}
		bound.push_back(BoundComparison{.lhs = std::move(lhs->operand),
		                                .op = comparison.op,
		                                .rhs = std::move(rhs->operand),
		                                .source = comparison});
	}
	return bound;
}

Result<BoundStatement> BindCreate(const CreateTableStmt& create, Catalog& catalog) {
	if (catalog.GetTable(create.table).has_value()) {
		return std::unexpected(
		    Status::DuplicateKey(std::format("table {} already exists", create.table)));
	}
	// Every schema rule (PK must be INT64, VARCHAR(n) bounds, duplicate column names, the row
	// size cap) lives in Schema::Create. The binder does not restate any of them.
	Result<Schema> schema = Schema::Create(create.columns, create.primary_key);
	if (!schema.has_value()) return std::unexpected(std::move(schema.error()));
	return BoundCreateTable{.name = create.table, .schema = std::move(*schema)};
}

Result<BoundStatement> BindInsert(const InsertStmt& insert, Catalog& catalog) {
	Result<Table*> table = LookUpTable(catalog, insert.table);
	if (!table.has_value()) return std::unexpected(std::move(table.error()));
	const Schema& schema = (*table)->GetSchema();

	// The whole statement is checked before any row goes in, so a bad row 3 never leaves rows 1
	// and 2 behind. Order: shape of every row, then keys against each other, then the table.
	for (std::size_t i = 0; i < insert.rows.size(); ++i) {
		if (Status st = schema.Validate(insert.rows[i]); !st.ok()) {
			return std::unexpected(
			    Status::InvalidArgument(std::format("row {}: {}", i + 1, st.message())));
		}
	}
	std::set<index_key_t> keys;
	for (std::size_t i = 0; i < insert.rows.size(); ++i) {
		const index_key_t key = schema.KeyOf(insert.rows[i]);
		if (!keys.insert(key).second) {
			return std::unexpected(Status::DuplicateKey(
			    std::format("row {}: primary key {} appears twice in this INSERT", i + 1, key)));
		}
	}
	for (std::size_t i = 0; i < insert.rows.size(); ++i) {
		const index_key_t key = schema.KeyOf(insert.rows[i]);
		Result<Row> existing = (*table)->Get(key);
		if (existing.has_value()) {
			return std::unexpected(Status::DuplicateKey(
			    std::format("row {}: primary key {} already exists", i + 1, key)));
		}
		if (existing.error().code() != ErrorCode::kNotFound) {
			return std::unexpected(std::move(existing.error()));
		}
	}
	return BoundInsert{.table = *table, .rows = insert.rows};
}

Result<BoundStatement> BindSelect(const SelectStmt& select, Catalog& catalog) {
	// Parsed since DD-007 so the AST shape is frozen; executed from the joins stage on.
	if (select.join.has_value()) {
		return std::unexpected(Status::InvalidArgument("JOIN is not supported yet"));
	}
	if (!select.order_by.empty()) {
		return std::unexpected(Status::InvalidArgument("ORDER BY is not supported yet"));
	}

	Result<Table*> table = LookUpTable(catalog, select.table);
	if (!table.has_value()) return std::unexpected(std::move(table.error()));

	BoundSelect bound{.table = *table};
	for (const ColumnRef& ref : select.columns) {
		Result<uint16_t> index = BindColumn(ref, **table);
		if (!index.has_value()) return std::unexpected(std::move(index.error()));
		bound.projection.push_back(*index);
	}
	Result<std::vector<BoundComparison>> where = BindWhere(select.where, **table);
	if (!where.has_value()) return std::unexpected(std::move(where.error()));
	bound.where = std::move(*where);
	return bound;
}

Result<BoundStatement> BindDelete(const DeleteStmt& del, Catalog& catalog) {
	Result<Table*> table = LookUpTable(catalog, del.table);
	if (!table.has_value()) return std::unexpected(std::move(table.error()));
	Result<std::vector<BoundComparison>> where = BindWhere(del.where, **table);
	if (!where.has_value()) return std::unexpected(std::move(where.error()));
	return BoundDelete{.table = *table, .where = std::move(*where)};
}

const Value& Resolve(const BoundOperand& operand, const Row& row) {
	if (const auto* column = std::get_if<BoundColumn>(&operand)) {
		return row[column->index];
	}
	return std::get<Value>(operand);
}

}  // namespace

Result<BoundStatement> kernsql::Bind(const Statement& statement, Catalog& catalog) {
	return std::visit(
	    [&](const auto& s) -> Result<BoundStatement> {
		    using S = std::decay_t<decltype(s)>;
		    if constexpr (std::is_same_v<S, CreateTableStmt>) {
			    return BindCreate(s, catalog);
		    } else if constexpr (std::is_same_v<S, InsertStmt>) {
			    return BindInsert(s, catalog);
		    } else if constexpr (std::is_same_v<S, SelectStmt>) {
			    return BindSelect(s, catalog);
		    } else {
			    return BindDelete(s, catalog);
		    }
	    },
	    statement);
}

bool kernsql::Evaluate(const BoundComparison& comparison, const Row& row) {
	const Value& lhs = Resolve(comparison.lhs, row);
	const Value& rhs = Resolve(comparison.rhs, row);
	// Same alternative on both sides (the binder's type check), so variant's <=> compares the
	// values themselves: int64_t numerically, std::string by memcmp order.
	const std::strong_ordering order = lhs <=> rhs;
	switch (comparison.op) {
		case CompareOp::kEqual:
			return order == 0;
		case CompareOp::kNotEqual:
			return order != 0;
		case CompareOp::kLess:
			return order < 0;
		case CompareOp::kLessEqual:
			return order <= 0;
		case CompareOp::kGreater:
			return order > 0;
		case CompareOp::kGreaterEqual:
			return order >= 0;
	}
	std::unreachable();
}

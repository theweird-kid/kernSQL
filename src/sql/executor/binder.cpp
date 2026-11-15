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

/*
 * The tables a statement can name, and how their columns line up in the combined row (binder.hpp):
 * `left`'s columns first, then `right`'s, if there is one.
 */
struct Scope {
	const Table* left{nullptr};
	const Table* right{nullptr};  // the JOIN table, or nullptr

	[[nodiscard]] std::size_t LeftWidth() const { return left->GetSchema().ColumnCount(); }

	[[nodiscard]] const Column& ColumnAt(uint16_t index) const {
		const std::size_t width = LeftWidth();
		return index < width ? left->GetSchema().Columns()[index]
		                     : right->GetSchema().Columns()[index - width];
	}
};

// `t.col` or `col` → its position in the combined row.
Result<uint16_t> BindColumn(const ColumnRef& ref, const Scope& scope) {
	const auto in = [&](const Table* table) -> std::optional<uint16_t> {
		if (table == nullptr) return std::nullopt;
		std::optional<uint16_t> index = table->GetSchema().ColumnIndex(ref.column);
		if (index.has_value() && table == scope.right) {
			*index = static_cast<uint16_t>(*index + scope.LeftWidth());
		}
		return index;
	};

	if (!ref.table.empty()) {
		const Table* table = nullptr;
		if (ref.table == scope.left->Name()) {
			table = scope.left;
		} else if (scope.right != nullptr && ref.table == scope.right->Name()) {
			table = scope.right;
		} else {
			// No aliases: a qualifier must be one of the tables in FROM, by name.
			return std::unexpected(Status::InvalidArgument(std::format(
			    "column {} names table '{}', which is not in FROM", ToSql(ref), ref.table)));
		}
		if (const std::optional<uint16_t> index = in(table)) return *index;
		return std::unexpected(Status::NotFound(std::format("no such column: {}", ToSql(ref))));
	}

	const std::optional<uint16_t> left = in(scope.left);
	const std::optional<uint16_t> right = in(scope.right);
	if (left.has_value() && right.has_value()) {
		return std::unexpected(Status::InvalidArgument(
		    std::format("ambiguous column name: {} (in both {} and {}; qualify it)", ref.column,
		                scope.left->Name(), scope.right->Name())));
	}
	if (left.has_value()) return *left;
	if (right.has_value()) return *right;
	return std::unexpected(Status::NotFound(std::format("no such column: {}", ref.column)));
}

struct BoundSide {
	BoundOperand operand;
	ColumnType type;
};

Result<BoundSide> BindOperand(const Operand& operand, const Scope& scope) {
	if (const auto* ref = std::get_if<ColumnRef>(&operand)) {
		Result<uint16_t> index = BindColumn(*ref, scope);
		if (!index.has_value()) return std::unexpected(std::move(index.error()));
		return BoundSide{BoundColumn{*index}, scope.ColumnAt(*index).type};
	}
	const Value& literal = std::get<Value>(operand);
	return BoundSide{literal, TypeOf(literal)};
}

Result<BoundComparison> BindComparison(const Comparison& comparison, const Scope& scope) {
	Result<BoundSide> lhs = BindOperand(comparison.lhs, scope);
	if (!lhs.has_value()) return std::unexpected(std::move(lhs.error()));
	Result<BoundSide> rhs = BindOperand(comparison.rhs, scope);
	if (!rhs.has_value()) return std::unexpected(std::move(rhs.error()));
	// No implicit casts: `id = '5'` is an error, not a string-to-integer conversion. SQLite's
	// type affinity would accept it; Postgres rejects it. One rule, no surprises.
	if (lhs->type != rhs->type) {
		return std::unexpected(Status::InvalidArgument(
		    std::format("type mismatch in {}: {} compared with {}", ToSql(comparison),
		                TypeName(lhs->type), TypeName(rhs->type))));
	}
	return BoundComparison{.lhs = std::move(lhs->operand),
	                       .op = comparison.op,
	                       .rhs = std::move(rhs->operand),
	                       .source = comparison};
}

Result<std::vector<BoundComparison>> BindWhere(const std::vector<Comparison>& where,
                                               const Scope& scope) {
	std::vector<BoundComparison> bound;
	bound.reserve(where.size());
	for (const Comparison& comparison : where) {
		Result<BoundComparison> b = BindComparison(comparison, scope);
		if (!b.has_value()) return std::unexpected(std::move(b.error()));
		bound.push_back(std::move(*b));
	}
	return bound;
}

// ON must be `x = y` with one column from each table: the only shape a hash join can run.
Result<BoundJoin> BindJoin(const JoinClause& join, Table& right, const Scope& scope) {
	Result<BoundComparison> on = BindComparison(join.on, scope);
	if (!on.has_value()) return std::unexpected(std::move(on.error()));

	const auto* a = std::get_if<BoundColumn>(&on->lhs);
	const auto* b = std::get_if<BoundColumn>(&on->rhs);
	const auto width = scope.LeftWidth();
	const auto is_left = [width](const BoundColumn* c) { return c->index < width; };
	if (on->op != CompareOp::kEqual || a == nullptr || b == nullptr || is_left(a) == is_left(b)) {
		return std::unexpected(Status::InvalidArgument(std::format(
		    "JOIN ... ON must be an equality between a column of {} and a column of {}, got {}",
		    scope.left->Name(), right.Name(), ToSql(join.on))));
	}
	const BoundColumn* left = is_left(a) ? a : b;
	const BoundColumn* right_col = is_left(a) ? b : a;
	return BoundJoin{.table = &right,
	                 .left_column = left->index,
	                 .right_column = static_cast<uint16_t>(right_col->index - width),
	                 .source = join.on};
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
	Result<Table*> table = LookUpTable(catalog, select.table);
	if (!table.has_value()) return std::unexpected(std::move(table.error()));

	BoundSelect bound{.table = *table};
	Scope scope{.left = *table};
	if (select.join.has_value()) {
		Result<Table*> right = LookUpTable(catalog, select.join->table);
		if (!right.has_value()) return std::unexpected(std::move(right.error()));
		if (*right == *table) {
			// `t JOIN t` needs aliases to tell the two sides apart, and there are none.
			return std::unexpected(Status::InvalidArgument(std::format(
			    "cannot join {} with itself (table aliases are not supported)", (*table)->Name())));
		}
		scope.right = *right;
		Result<BoundJoin> join = BindJoin(*select.join, **right, scope);
		if (!join.has_value()) return std::unexpected(std::move(join.error()));
		bound.join = std::move(*join);
	}

	for (const Table* t : {scope.left, scope.right}) {
		if (t == nullptr) continue;
		for (const Column& column : t->GetSchema().Columns()) {
			bound.column_names.push_back(column.name);
		}
	}

	for (const ColumnRef& ref : select.columns) {
		Result<uint16_t> index = BindColumn(ref, scope);
		if (!index.has_value()) return std::unexpected(std::move(index.error()));
		bound.projection.push_back(*index);
	}
	Result<std::vector<BoundComparison>> where = BindWhere(select.where, scope);
	if (!where.has_value()) return std::unexpected(std::move(where.error()));
	bound.where = std::move(*where);

	// ORDER BY may name any column of the combined row, projected or not.
	for (const SortKey& key : select.order_by) {
		Result<uint16_t> index = BindColumn(key.column, scope);
		if (!index.has_value()) return std::unexpected(std::move(index.error()));
		bound.order_by.push_back(BoundSortKey{.column = *index, .descending = key.descending});
	}
	return bound;
}

Result<BoundStatement> BindDelete(const DeleteStmt& del, Catalog& catalog) {
	Result<Table*> table = LookUpTable(catalog, del.table);
	if (!table.has_value()) return std::unexpected(std::move(table.error()));
	Result<std::vector<BoundComparison>> where = BindWhere(del.where, Scope{.left = *table});
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

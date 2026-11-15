#include "sql/executor/planner.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "indices/b_plus_tree_page.hpp"

using namespace kernsql;

namespace {

constexpr index_key_t kMinKey = std::numeric_limits<index_key_t>::min();
constexpr index_key_t kMaxKey = std::numeric_limits<index_key_t>::max();

// `5 < id` is `id > 5`: the operator mirrored, for a literal written on the left.
CompareOp Mirror(CompareOp op) {
	switch (op) {
		case CompareOp::kLess:
			return CompareOp::kGreater;
		case CompareOp::kLessEqual:
			return CompareOp::kGreaterEqual;
		case CompareOp::kGreater:
			return CompareOp::kLess;
		case CompareOp::kGreaterEqual:
			return CompareOp::kLessEqual;
		case CompareOp::kEqual:
		case CompareOp::kNotEqual:
			return op;
	}
	std::unreachable();
}

// A term of the shape `pk op integer` (either way round), normalised to the column on the left.
struct KeyBound {
	CompareOp op;
	index_key_t value;
};

std::optional<KeyBound> AsKeyBound(const BoundComparison& term, uint16_t pk) {
	const auto is_pk = [pk](const BoundOperand& side) {
		const auto* column = std::get_if<BoundColumn>(&side);
		return column != nullptr && column->index == pk;
	};
	const auto as_int = [](const BoundOperand& side) -> std::optional<int64_t> {
		const auto* value = std::get_if<Value>(&side);
		if (value == nullptr) return std::nullopt;
		const auto* integer = std::get_if<int64_t>(value);
		return integer ? std::optional<int64_t>(*integer) : std::nullopt;
	};

	if (term.op == CompareOp::kNotEqual) {
		return std::nullopt;  // a hole in the range, not a range
	}
	if (is_pk(term.lhs)) {
		if (const std::optional<int64_t> v = as_int(term.rhs)) return KeyBound{term.op, *v};
	}
	if (is_pk(term.rhs)) {
		if (const std::optional<int64_t> v = as_int(term.lhs)) return KeyBound{Mirror(term.op), *v};
	}
	return std::nullopt;
}

// Intersect `range` with `pk op value`. Strict bounds become inclusive ones by stepping one key,
// which cannot be done past the type's end: `id < INT64_MIN` and `id > INT64_MAX` hold nothing.
void Narrow(KeyRange& range, KeyBound bound) {
	switch (bound.op) {
		case CompareOp::kEqual:
			range.lo = std::max(range.lo, bound.value);
			range.hi = std::min(range.hi, bound.value);
			break;
		case CompareOp::kLess:
			if (bound.value == kMinKey) {
				range.empty = true;
			} else {
				range.hi = std::min(range.hi, bound.value - 1);
			}
			break;
		case CompareOp::kLessEqual:
			range.hi = std::min(range.hi, bound.value);
			break;
		case CompareOp::kGreater:
			if (bound.value == kMaxKey) {
				range.empty = true;
			} else {
				range.lo = std::max(range.lo, bound.value + 1);
			}
			break;
		case CompareOp::kGreaterEqual:
			range.lo = std::max(range.lo, bound.value);
			break;
		case CompareOp::kNotEqual:
			std::unreachable();  // AsKeyBound never returns one
	}
	if (range.lo > range.hi) {
		range.empty = true;
	}
}

}  // namespace

std::unique_ptr<Operator> kernsql::PlanScan(Table& table, std::vector<BoundComparison> where) {
	const uint16_t pk = table.GetSchema().PrimaryKeyColumn();

	KeyRange range{.lo = kMinKey, .hi = kMaxKey};
	bool use_index = false;
	std::vector<BoundComparison> residual;
	for (BoundComparison& term : where) {
		if (const std::optional<KeyBound> bound = AsKeyBound(term, pk)) {
			Narrow(range, *bound);
			use_index = true;  // the range now enforces this term: drop it
		} else {
			residual.push_back(std::move(term));
		}
	}

	std::unique_ptr<Operator> plan;
	if (use_index) {
		plan = std::make_unique<IndexScan>(table, range);
	} else {
		plan = std::make_unique<SeqScan>(table);
	}
	if (!residual.empty()) {
		plan = std::make_unique<Filter>(std::move(plan), std::move(residual));
	}
	return plan;
}

std::unique_ptr<Operator> kernsql::PlanSelect(const BoundSelect& select) {
	std::unique_ptr<Operator> plan = PlanScan(*select.table, select.where);
	if (select.projection.empty()) {
		return plan;  // SELECT *: the scan's rows are already in schema order
	}
	std::vector<std::string> names;
	for (uint16_t column : select.projection) {
		names.push_back(select.table->GetSchema().Columns()[column].name);
	}
	return std::make_unique<Projection>(std::move(plan), select.projection, std::move(names));
}

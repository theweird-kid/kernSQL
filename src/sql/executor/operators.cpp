#include "sql/executor/operators.hpp"

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "sql/parser/ast.hpp"

using namespace kernsql;

// ---- SeqScan ----

Result<bool> SeqScan::Next(Row& row) {
	if (!it_.has_value()) {
		it_.emplace(table_.Scan());
	}
	Result<bool> more = it_->Next();
	if (!more.has_value()) return std::unexpected(std::move(more.error()));
	if (!*more) {
		return false;
	}
	// Tuple() points into the iterator and dies on the next Next(): decode (copy) it now.
	Result<Row> decoded = table_.GetSchema().Decode(it_->Tuple());
	if (!decoded.has_value()) return std::unexpected(std::move(decoded.error()));
	row = std::move(*decoded);
	return true;
}

std::string SeqScan::Describe() const {
	return std::format("SeqScan: {}", table_.Name());
}

// ---- IndexScan ----

Result<bool> IndexScan::Next(Row& row) {
	if (range_.empty) {
		return false;
	}
	if (!it_.has_value()) {
		assert(range_.lo <= range_.hi);
		Result<IndexIterator> it = table_.ScanIndex(range_.lo, range_.hi);
		if (!it.has_value()) return std::unexpected(std::move(it.error()));
		it_.emplace(std::move(*it));
	}
	Result<bool> more = it_->Next();
	if (!more.has_value()) return std::unexpected(std::move(more.error()));
	if (!*more) {
		return false;
	}
	// RowAt, not Get(key): the RID is already in hand, so the tree is not searched twice.
	Result<Row> fetched = table_.RowAt(it_->Rid());
	if (!fetched.has_value()) {
		if (fetched.error().code() == ErrorCode::kNotFound) {
			// The index names a slot with no row: heap and index have drifted (Table::Get).
			return std::unexpected(Status::Corruption(std::format(
			    "table {}: index key {} points at an empty heap slot", table_.Name(), it_->Key())));
		}
		return std::unexpected(std::move(fetched.error()));
	}
	row = std::move(*fetched);
	return true;
}

std::string IndexScan::Describe() const {
	const Column& key = table_.GetSchema().Columns()[table_.GetSchema().PrimaryKeyColumn()];
	if (range_.empty) {
		return std::format("IndexScan: {} ({} in empty range)", table_.Name(), key.name);
	}
	if (range_.lo == range_.hi) {
		return std::format("IndexScan: {} ({} = {})", table_.Name(), key.name, range_.lo);
	}
	// Print only the bounds a predicate set: an open end is the type's limit.
	constexpr index_key_t kMin = std::numeric_limits<index_key_t>::min();
	constexpr index_key_t kMax = std::numeric_limits<index_key_t>::max();
	std::string bounds;
	if (range_.lo != kMin) bounds += std::format("{} >= {}", key.name, range_.lo);
	if (range_.lo != kMin && range_.hi != kMax) bounds += " AND ";
	if (range_.hi != kMax) bounds += std::format("{} <= {}", key.name, range_.hi);
	if (bounds.empty()) bounds = "full range";
	return std::format("IndexScan: {} ({})", table_.Name(), bounds);
}

// ---- Filter ----

Result<bool> Filter::Next(Row& row) {
	while (true) {
		Result<bool> more = child_->Next(row);
		if (!more.has_value() || !*more) {
			return more;
		}
		bool pass = true;
		for (const BoundComparison& predicate : predicates_) {
			if (!Evaluate(predicate, row)) {
				pass = false;
				break;
			}
		}
		if (pass) {
			return true;
		}
	}
}

std::string Filter::Describe() const {
	std::string out = "Filter: ";
	for (std::size_t i = 0; i < predicates_.size(); ++i) {
		if (i > 0) out += " AND ";
		out += ToSql(predicates_[i].source);
	}
	return out;
}

// ---- Projection ----

Result<bool> Projection::Next(Row& row) {
	Result<bool> more = child_->Next(input_);
	if (!more.has_value() || !*more) {
		return more;
	}
	row.clear();
	row.reserve(columns_.size());
	for (uint16_t column : columns_) {
		row.push_back(input_[column]);
	}
	return true;
}

std::string Projection::Describe() const {
	std::string out = "Projection: ";
	for (std::size_t i = 0; i < names_.size(); ++i) {
		if (i > 0) out += ", ";
		out += names_[i];
	}
	return out;
}

// ---- Sort ----

Result<bool> Sort::Next(Row& row) {
	if (!sorted_) {
		Row input;
		while (true) {
			Result<bool> more = child_->Next(input);
			if (!more.has_value()) return std::unexpected(std::move(more.error()));
			if (!*more) break;
			rows_.push_back(input);
		}
		std::ranges::stable_sort(rows_, [this](const Row& a, const Row& b) {
			for (const BoundSortKey& key : keys_) {
				const std::strong_ordering order = a[key.column] <=> b[key.column];
				if (order != 0) {
					return key.descending ? order > 0 : order < 0;
				}
			}
			return false;  // equal on every key: keep the child's order (stable)
		});
		sorted_ = true;
	}
	if (next_ == rows_.size()) {
		return false;
	}
	row = std::move(rows_[next_++]);
	return true;
}

// ---- Joins ----

Result<bool> JoinBase::Next(Row& row) {
	if (!built_) {
		std::vector<Row> right_rows;
		Row input;
		while (true) {
			Result<bool> more = right_->Next(input);
			if (!more.has_value()) return std::unexpected(std::move(more.error()));
			if (!*more) break;
			right_rows.push_back(input);
		}
		Build(std::move(right_rows));
		built_ = true;
	}

	// Emit the current left row's remaining matches; when they run out, pull the next left row.
	while (next_ == matches_.size()) {
		Result<bool> more = left_->Next(left_row_);
		if (!more.has_value() || !*more) {
			return more;
		}
		matches_.clear();
		next_ = 0;
		Matches(left_row_[left_column_], matches_);
	}
	const Row& right_row = *matches_[next_++];
	row = left_row_;
	row.insert(row.end(), right_row.begin(), right_row.end());
	return true;
}

void NestedLoopJoin::Matches(const Value& key, std::vector<const Row*>& out) const {
	for (const Row& right_row : rows_) {
		if (right_row[right_column_] == key) {
			out.push_back(&right_row);
		}
	}
}

void HashJoin::Build(std::vector<Row> right_rows) {
	rows_ = std::move(right_rows);
	table_.reserve(rows_.size());
	for (const Row& right_row : rows_) {
		table_.emplace(right_row[right_column_], &right_row);
	}
}

void HashJoin::Matches(const Value& key, std::vector<const Row*>& out) const {
	const auto [begin, end] = table_.equal_range(key);
	for (auto it = begin; it != end; ++it) {
		out.push_back(it->second);
	}
	// equal_range has no defined order; restore the right side's scan order, so a hash join and
	// a nested-loop join over the same input emit identical sequences, not just identical sets.
	std::ranges::sort(out);
}

// ---- EXPLAIN ----

namespace {

void Explain(const Operator& op, std::size_t depth, std::vector<std::string>& lines) {
	lines.push_back(std::string(2 * depth, ' ') + op.Describe());
	for (const Operator* child : op.Children()) {
		Explain(*child, depth + 1, lines);
	}
}

}  // namespace

std::vector<std::string> kernsql::ExplainLines(const Operator& root) {
	std::vector<std::string> lines;
	Explain(root, 0, lines);
	return lines;
}

#include "sql/executor/operators.hpp"

#include <cassert>
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

// ---- EXPLAIN ----

std::vector<std::string> kernsql::ExplainLines(const Operator& root) {
	std::vector<std::string> lines;
	std::size_t depth = 0;
	for (const Operator* op = &root; op != nullptr; op = op->Child()) {
		lines.push_back(std::string(2 * depth, ' ') + op->Describe());
		++depth;
	}
	return lines;
}

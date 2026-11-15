#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/schema.hpp"
#include "catalog/table.hpp"
#include "common/status.hpp"
#include "heap/table_heap.hpp"
#include "indices/b_plus_tree.hpp"
#include "indices/b_plus_tree_page.hpp"
#include "sql/executor/binder.hpp"

namespace kernsql {

/*
 * The Volcano model (Graefe, 1994), DD-008 "Operators": a plan is a tree of operators, and every
 * operator answers one call — Next(): "give me your next row, or tell me you are done". A parent
 * pulls from its child one row at a time, so a SELECT never holds more than one row per operator
 * in memory, and LIMIT-style early stops cost nothing.
 *
 *   Projection(a, b)          Next(): child.Next(), keep columns a and b
 *     Filter(x > 3)           Next(): child.Next() until a row passes
 *       SeqScan(t)            Next(): the next heap row, decoded
 *
 * Postgres (ExecProcNode), SQLite's VDBE loops, and most textbook engines are built this way;
 * vectorised engines (DuckDB, ClickHouse) pass batches instead, for CPU efficiency at a scale this
 * engine does not have.
 *
 * > A LIVE SCAN PINS A PAGE AND HOLDS ITS READ LATCH.
 *
 * TableIterator and IndexIterator keep the current page read-latched between Next() calls. So
 * NEVER write to a table while an operator tree that scans it is alive: the write would wait on
 * that latch forever (Table::Scan's comment). DELETE therefore collects keys first, destroys the
 * tree, and only then deletes (Executor, "collect then mutate").
 *
 * Scans open their iterator lazily, on the first Next(), so building a tree (and EXPLAIN, which
 * only builds one) touches no page.
 */
class Operator {
  public:
	virtual ~Operator() = default;

	/*
	 * The next row into `row`. True if there was one; false at the end, and on every call after.
	 * An error (a failed fetch, a row that does not decode) ends the query.
	 */
	[[nodiscard]] virtual Result<bool> Next(Row& row) = 0;

	// One line for EXPLAIN, without indentation: "SeqScan: t", "Filter: a > 3".
	[[nodiscard]] virtual std::string Describe() const = 0;

	// The operators this one pulls from, for EXPLAIN: none for a scan, two for a join.
	[[nodiscard]] virtual std::vector<const Operator*> Children() const { return {}; }
};

// Every row of the table, in heap order.
class SeqScan final : public Operator {
  public:
	explicit SeqScan(Table& table) : table_(table) {}
	[[nodiscard]] Result<bool> Next(Row& row) override;
	[[nodiscard]] std::string Describe() const override;

  private:
	Table& table_;
	std::optional<TableIterator> it_;  // opened on the first Next()
};

/*
 * The inclusive primary-key range [lo, hi], in key order, each row fetched from the heap by the
 * RID the index holds. `empty` is a range the planner proved holds nothing (`id > 5 AND id < 3`,
 * or `id < INT64_MIN`), which no [lo, hi] pair can say without lo > hi.
 */
struct KeyRange {
	index_key_t lo;
	index_key_t hi;
	bool empty{false};
};

class IndexScan final : public Operator {
  public:
	IndexScan(Table& table, KeyRange range) : table_(table), range_(range) {}
	[[nodiscard]] Result<bool> Next(Row& row) override;
	[[nodiscard]] std::string Describe() const override;

  private:
	Table& table_;
	KeyRange range_;
	std::optional<IndexIterator> it_;  // opened on the first Next()
};

// The child's rows that pass every predicate (an implicit AND).
class Filter final : public Operator {
  public:
	Filter(std::unique_ptr<Operator> child, std::vector<BoundComparison> predicates)
	    : child_(std::move(child)), predicates_(std::move(predicates)) {}
	[[nodiscard]] Result<bool> Next(Row& row) override;
	[[nodiscard]] std::string Describe() const override;
	[[nodiscard]] std::vector<const Operator*> Children() const override { return {child_.get()}; }

  private:
	std::unique_ptr<Operator> child_;
	std::vector<BoundComparison> predicates_;
};

// The child's rows cut down to `columns`, in that order. `names` are for EXPLAIN only.
class Projection final : public Operator {
  public:
	Projection(std::unique_ptr<Operator> child, std::vector<uint16_t> columns,
	           std::vector<std::string> names)
	    : child_(std::move(child)), columns_(std::move(columns)), names_(std::move(names)) {}
	[[nodiscard]] Result<bool> Next(Row& row) override;
	[[nodiscard]] std::string Describe() const override;
	[[nodiscard]] std::vector<const Operator*> Children() const override { return {child_.get()}; }

  private:
	std::unique_ptr<Operator> child_;
	std::vector<uint16_t> columns_;
	std::vector<std::string> names_;
	Row input_;  // reused between calls, so the child's row is not reallocated each time
};

/*
 * The child's rows ordered by `keys` (positions in its row), ascending unless descending. A
 * PIPELINE BREAKER: the first Next() pulls every row from the child into memory and sorts them;
 * only then does a row come out. Stable, so rows equal on every key keep the child's order.
 *
 * In memory only — no external merge sort spilling to disk. A table that does not fit in RAM is
 * not a case this engine meets: the buffer pool itself is a few MiB.
 */
class Sort final : public Operator {
  public:
	Sort(std::unique_ptr<Operator> child, std::vector<BoundSortKey> keys, std::string description)
	    : child_(std::move(child)), keys_(std::move(keys)), description_(std::move(description)) {}
	[[nodiscard]] Result<bool> Next(Row& row) override;
	[[nodiscard]] std::string Describe() const override { return "Sort: " + description_; }
	[[nodiscard]] std::vector<const Operator*> Children() const override { return {child_.get()}; }

  private:
	std::unique_ptr<Operator> child_;
	std::vector<BoundSortKey> keys_;
	std::string description_;  // "name DESC, id", for EXPLAIN
	std::vector<Row> rows_;
	std::size_t next_{0};
	bool sorted_{false};
};

/*
 * left JOIN right ON left[left_column] = right[right_column]. Each output row is the left row's
 * values followed by the right row's (binder.hpp, "the row a SELECT works on"), in left-row order.
 *
 * Both joins below read the RIGHT side into memory once, on the first Next(), and then stream the
 * left side past it. They differ only in how they find a left row's matches:
 *
 *   NestedLoopJoin  compares the left row with every right row         O(|L| * |R|)
 *   HashJoin        looks the left row's key up in a hash table         O(|L| + |R|)
 *
 * Nested loop is kept as the ORACLE: it is too simple to be wrong, so the tests run every join
 * both ways and require the same rows. That is how Postgres and SQLite test their join methods.
 */
class JoinBase : public Operator {
  public:
	JoinBase(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, uint16_t left_column,
	         uint16_t right_column, std::string on)
	    : right_column_(right_column),
	      on_(std::move(on)),
	      left_(std::move(left)),
	      right_(std::move(right)),
	      left_column_(left_column) {}
	[[nodiscard]] Result<bool> Next(Row& row) final;
	[[nodiscard]] std::vector<const Operator*> Children() const final {
		return {left_.get(), right_.get()};
	}

  protected:
	// Called once, with every right row. Keep them however matching wants them.
	virtual void Build(std::vector<Row> right_rows) = 0;
	// The right rows that match `key`, the current left row's join value.
	virtual void Matches(const Value& key, std::vector<const Row*>& out) const = 0;

	uint16_t right_column_;
	std::string on_;  // "people.id = pets.owner", for EXPLAIN

  private:
	std::unique_ptr<Operator> left_;
	std::unique_ptr<Operator> right_;
	uint16_t left_column_;

	bool built_{false};
	Row left_row_;
	std::vector<const Row*> matches_;  // of left_row_, not yet emitted from matches_[next_] on
	std::size_t next_{0};
};

class NestedLoopJoin final : public JoinBase {
  public:
	using JoinBase::JoinBase;
	[[nodiscard]] std::string Describe() const override { return "NestedLoopJoin: " + on_; }

  private:
	void Build(std::vector<Row> right_rows) override { rows_ = std::move(right_rows); }
	void Matches(const Value& key, std::vector<const Row*>& out) const override;
	std::vector<Row> rows_;
};

/*
 * The classic in-memory hash join: BUILD a hash table on the right side's join column, then PROBE
 * it with each left row. The build side is the JOIN table, not chosen by size — with no
 * statistics there is nothing to choose by, and the user decides by writing the query.
 */
class HashJoin final : public JoinBase {
  public:
	using JoinBase::JoinBase;
	[[nodiscard]] std::string Describe() const override { return "HashJoin: " + on_; }

  private:
	void Build(std::vector<Row> right_rows) override;
	void Matches(const Value& key, std::vector<const Row*>& out) const override;
	std::vector<Row> rows_;
	std::unordered_multimap<Value, const Row*> table_;  // points into rows_, which never moves
};

// The plan as EXPLAIN prints it: one operator per line, each child indented two more spaces.
[[nodiscard]] std::vector<std::string> ExplainLines(const Operator& root);

}  // namespace kernsql

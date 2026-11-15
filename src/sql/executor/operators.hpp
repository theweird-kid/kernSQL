#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

	// The operator this one pulls from, for EXPLAIN. nullptr for a scan.
	[[nodiscard]] virtual const Operator* Child() const { return nullptr; }
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
	[[nodiscard]] const Operator* Child() const override { return child_.get(); }

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
	[[nodiscard]] const Operator* Child() const override { return child_.get(); }

  private:
	std::unique_ptr<Operator> child_;
	std::vector<uint16_t> columns_;
	std::vector<std::string> names_;
	Row input_;  // reused between calls, so the child's row is not reallocated each time
};

// The plan as EXPLAIN prints it: one operator per line, each child indented two more spaces.
[[nodiscard]] std::vector<std::string> ExplainLines(const Operator& root);

}  // namespace kernsql

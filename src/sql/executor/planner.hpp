#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "catalog/table.hpp"
#include "sql/executor/binder.hpp"
#include "sql/executor/operators.hpp"

namespace kernsql {

/*
 * The rule-based planner (DD-008, "Planner"). One decision: how to reach the rows.
 *
 *   1. ACCESS PATH. Every WHERE term that compares the primary key with an integer literal
 *      (`id = 5`, `id >= 10`, `3 < id`) narrows one key range. If there is at least one, the scan
 *      is an IndexScan over that range and those terms are dropped — the range already enforces
 *      them. Otherwise a SeqScan. `!=` never narrows a range; it stays a filter.
 *   2. FILTER over whatever terms are left, if any.
 *   3. PROJECTION, for a SELECT that is not `*`.
 *
 * With a JOIN, each WHERE term that names only one table is PUSHED DOWN to that table's scan
 * before the join (so `pets.id = 3` still becomes an IndexScan on pets), and only terms naming
 * both tables, or neither, filter above it:
 *
 *   Projection → Sort → Filter(cross-table terms) → HashJoin ┬ PlanScan(left,  left-only terms)
 *                                                           └ PlanScan(right, right-only terms)
 *
 * Sort sits below Projection because ORDER BY may name a column the SELECT does not output.
 *
 * No statistics, no costs: an index range is always at least as good as a full scan for this
 * engine, because the heap has no ordering a scan could exploit. A cost model starts to matter
 * with joins, where the choice is between algorithms (stage 4).
 */

// The scan plus any Filter for `where`, which is what both SELECT and DELETE need.
[[nodiscard]] std::unique_ptr<Operator> PlanScan(Table& table, std::vector<BoundComparison> where);

// Which join operator to build. kHash always, except in tests, which run kNestedLoop as the
// oracle for it (operators.hpp, JoinBase).
enum class JoinAlgorithm : uint8_t { kHash, kNestedLoop };

// The whole SELECT: scans (pushed-down terms), join, filter, sort, projection.
[[nodiscard]] std::unique_ptr<Operator> PlanSelect(const BoundSelect& select,
                                                   JoinAlgorithm join = JoinAlgorithm::kHash);

}  // namespace kernsql

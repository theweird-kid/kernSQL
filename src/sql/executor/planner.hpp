#pragma once

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
 * No statistics, no costs: an index range is always at least as good as a full scan for this
 * engine, because the heap has no ordering a scan could exploit. A cost model starts to matter
 * with joins, where the choice is between algorithms (stage 4).
 */

// The scan plus any Filter for `where`, which is what both SELECT and DELETE need.
[[nodiscard]] std::unique_ptr<Operator> PlanScan(Table& table, std::vector<BoundComparison> where);

// PlanScan, then a Projection unless the projection is `*`.
[[nodiscard]] std::unique_ptr<Operator> PlanSelect(const BoundSelect& select);

}  // namespace kernsql

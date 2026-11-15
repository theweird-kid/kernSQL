#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/schema.hpp"
#include "common/status.hpp"
#include "indices/b_plus_tree_page.hpp"

namespace kernsql {

/*
 * What a statement returns (DD-008, "Results").
 *
 *   SELECT    columns + rows
 *   EXPLAIN   one column, "plan", one row per line of the plan
 *   others    no columns; `tag` says what happened
 *
 * `tag` is the Postgres command tag: "CREATE TABLE", "INSERT 3", "DELETE 2", "SELECT 5". A client
 * can print it as-is, and tests can check a row count without counting.
 */
struct QueryResult {
	std::vector<std::string> columns{};
	std::vector<Row> rows{};
	std::string tag{};
};

/*
 * SQL in, QueryResult out: parse → bind → plan → run (DD-008, "Pipeline").
 *
 * Holds only a Catalog&, and never a pin or a latch between calls: every operator tree, iterator
 * and guard a statement creates is destroyed before Execute returns.
 *
 * THREADING: none. One statement at a time, until the table locks of stage 5.
 */
class Executor {
  public:
	// `index_fanout` is passed to every CREATE TABLE — tests only, so a handful of rows already
	// makes a multi-level tree and range scans cross leaves (as Catalog::CreateTable's).
	explicit Executor(Catalog& catalog, uint16_t index_fanout = NODE_CAPACITY)
	    : catalog_(catalog), index_fanout_(index_fanout) {}

	/*
	 * Run one statement. Errors: the parser's and the binder's (see binder.hpp), then whatever
	 * the storage layers return while running.
	 *
	 * NOT ATOMIC in general — there is no WAL (DD-006). The binder makes INSERT all-or-nothing for
	 * every error it can foresee (types, sizes, duplicate keys); what is left is an I/O or pool
	 * error midway, which leaves the rows done so far. DELETE likewise.
	 */
	[[nodiscard]] Result<QueryResult> Execute(std::string_view sql);

  private:
	Catalog& catalog_;
	uint16_t index_fanout_;
};

/*
 * A result as a psql-style text table, or the tag alone for a result with no columns:
 *
 *    id | name
 *   ----+-------
 *     1 | alice
 *   (1 row)
 *
 * Integers are right-aligned, strings left-aligned; strings are printed raw, not quoted.
 */
[[nodiscard]] std::string FormatResult(const QueryResult& result);

/*
 * The shell's input splitter (DD-007 decision 7: one statement per Parse, splitting is the
 * shell's job). Cuts `buffer` after every `;` token — so a `;` inside a string or a comment never
 * splits — into complete statements, and returns what follows the last `;` as `remainder`, still
 * being typed.
 *
 * If the buffer ends inside a string, nothing is complete yet: all of it is remainder. Any other
 * lexer error makes the whole buffer one "statement", so that running it reports the error
 * instead of the shell waiting for a `;` that cannot fix it.
 */
struct SplitInput {
	std::vector<std::string> statements{};
	std::string remainder{};
};
[[nodiscard]] SplitInput SplitStatements(std::string_view buffer);

}  // namespace kernsql

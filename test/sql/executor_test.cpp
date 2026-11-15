#include "sql/executor/executor.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "buffer/pool_stats.hpp"
#include "catalog/database.hpp"
#include "catalog/schema.hpp"
#include "common/status.hpp"
#include "indices/b_plus_tree_page.hpp"
#include "sql/parser/ast.hpp"

namespace kernsql {

/*
 * The executor against a real file, a real pool and real tables (DD-008, "Testing"): SQL in,
 * rows out. Three assertions run after every statement, through Exec():
 *
 *   - the statement succeeded (or failed with the expected code),
 *   - NOTHING IS PINNED afterwards — the executor's promise that no operator, iterator or guard
 *     outlives Execute,
 *
 * and, in the tests that mutate, CheckConsistency() on the table: the heap and the index agree.
 *
 * The index fanout is MIN_FANOUT, so a few dozen rows already make a deep tree and every range
 * scan crosses leaves. The centrepiece is the differential test: random WHERE clauses through the
 * planner (IndexScan with ranges, SeqScan, Filter) against a std::map evaluated by hand.
 */
class ExecutorTest : public ::testing::Test {
  protected:
	static constexpr std::size_t kFrames = 32;
	static constexpr index_key_t kMin = std::numeric_limits<index_key_t>::min();
	static constexpr index_key_t kMax = std::numeric_limits<index_key_t>::max();

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_executor_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
		         std::to_string(getpid()));
		std::filesystem::remove(path_);
		Open();
	}

	void TearDown() override {
		executor_.reset();
		if (db_) {
			EXPECT_TRUE(db_->Shutdown().ok());
		}
		db_.reset();
		std::filesystem::remove(path_);
	}

	void Open() {
		auto db = Database::Open(path_, kFrames);
		ASSERT_TRUE(db.has_value()) << db.error().message();
		db_ = std::move(*db);
		executor_ = std::make_unique<Executor>(db_->GetCatalog(), MIN_FANOUT);
	}

	void Reopen() {
		executor_.reset();
		ASSERT_TRUE(db_->Shutdown().ok());
		db_.reset();
		Open();
	}

	void ExpectNothingPinned(std::string_view sql) {
		EXPECT_EQ(db_->Pool().GetStats().pinned_frames, 0u) << "pin leaked by: " << sql;
	}

	QueryResult Exec(std::string_view sql) {
		Result<QueryResult> result = executor_->Execute(sql);
		EXPECT_TRUE(result.has_value()) << sql << "\n  → " << result.error().message();
		ExpectNothingPinned(sql);
		return result.has_value() ? *result : QueryResult{};
	}

	void ExpectError(std::string_view sql, ErrorCode code, std::string_view fragment) {
		Result<QueryResult> result = executor_->Execute(sql);
		ASSERT_FALSE(result.has_value()) << "succeeded: " << sql;
		EXPECT_EQ(result.error().code(), code) << sql << "\n  → " << result.error().message();
		EXPECT_NE(std::string(result.error().message()).find(fragment), std::string::npos)
		    << sql << "\n  → " << result.error().message();
		ExpectNothingPinned(sql);
	}

	std::vector<Row> Rows(std::string_view sql) { return Exec(sql).rows; }

	std::vector<std::string> Plan(std::string_view sql) {
		std::vector<std::string> lines;
		for (const Row& row : Exec(sql).rows) lines.push_back(std::get<std::string>(row[0]));
		return lines;
	}

	bool Consistent(std::string_view table) {
		Result<Table*> t = db_->GetCatalog().GetTable(table);
		return t.has_value() && (*t)->CheckConsistency();
	}

	// The standard table: the key in the MIDDLE, so code assuming column 0 is the key fails.
	void CreatePeople() {
		Exec("CREATE TABLE people (name VARCHAR(16), id INT64 PRIMARY KEY, age INT64)");
		Exec(
		    "INSERT INTO people VALUES ('alice', 1, 30), ('bob', 2, 25), ('carol', 3, 41), "
		    "('dave', 4, 25), ('eve', 5, 35)");
	}

	static Row R(std::string name, int64_t id, int64_t age) {
		return Row{Value{std::move(name)}, Value{id}, Value{age}};
	}

	// Rows sorted by their id (column 1 of people), for comparisons that ignore scan order.
	static std::vector<Row> ById(std::vector<Row> rows, std::size_t key_column = 1) {
		std::ranges::sort(rows, [key_column](const Row& a, const Row& b) {
			return std::get<int64_t>(a[key_column]) < std::get<int64_t>(b[key_column]);
		});
		return rows;
	}

	std::filesystem::path path_;
	std::unique_ptr<Database> db_;
	std::unique_ptr<Executor> executor_;
};

// ---- CREATE / INSERT / SELECT basics ----

TEST_F(ExecutorTest, CreateInsertSelectStar) {
	EXPECT_EQ(Exec("CREATE TABLE people (name VARCHAR(16), id INT64 PRIMARY KEY, age INT64)").tag,
	          "CREATE TABLE");
	EXPECT_EQ(Exec("INSERT INTO people VALUES ('alice', 1, 30), ('bob', 2, 25)").tag, "INSERT 2");

	const QueryResult result = Exec("SELECT * FROM people");
	EXPECT_EQ(result.columns, (std::vector<std::string>{"name", "id", "age"}));
	EXPECT_EQ(ById(result.rows), (std::vector<Row>{R("alice", 1, 30), R("bob", 2, 25)}));
	EXPECT_EQ(result.tag, "SELECT 2");
}

TEST_F(ExecutorTest, SelectFromAnEmptyTable) {
	Exec("CREATE TABLE t (id INT64 PRIMARY KEY)");
	const QueryResult result = Exec("SELECT * FROM t");
	EXPECT_TRUE(result.rows.empty());
	EXPECT_EQ(result.tag, "SELECT 0");
	EXPECT_TRUE(Rows("SELECT * FROM t WHERE id = 1").empty());
}

TEST_F(ExecutorTest, ProjectionPicksAndReordersColumns) {
	CreatePeople();
	const QueryResult result = Exec("SELECT age, name, people.age FROM people WHERE id = 3");
	EXPECT_EQ(result.columns, (std::vector<std::string>{"age", "name", "age"}));
	EXPECT_EQ(
	    result.rows,
	    (std::vector<Row>{{Value{int64_t{41}}, Value{std::string("carol")}, Value{int64_t{41}}}}));
}

TEST_F(ExecutorTest, CaseInsensitiveNames) {
	Exec("CREATE TABLE Users (ID INT PRIMARY KEY, Name VARCHAR(8))");
	Exec("insert into USERS values (1, 'Keep')");
	const QueryResult result = Exec("SELECT NAME FROM users WHERE Id = 1");
	EXPECT_EQ(result.columns, (std::vector<std::string>{"name"}));
	// Identifiers fold; string contents do not.
	EXPECT_EQ(result.rows, (std::vector<Row>{{Value{std::string("Keep")}}}));
}

TEST_F(ExecutorTest, WhereOnNonKeyColumns) {
	CreatePeople();
	EXPECT_EQ(ById(Rows("SELECT * FROM people WHERE age = 25")),
	          (std::vector<Row>{R("bob", 2, 25), R("dave", 4, 25)}));
	EXPECT_EQ(ById(Rows("SELECT * FROM people WHERE age > 30 AND name != 'eve'")),
	          (std::vector<Row>{R("carol", 3, 41)}));
	// Strings compare in memcmp order.
	EXPECT_EQ(ById(Rows("SELECT * FROM people WHERE name < 'c'")),
	          (std::vector<Row>{R("alice", 1, 30), R("bob", 2, 25)}));
	EXPECT_EQ(ById(Rows("SELECT * FROM people WHERE 'dave' <= name")),
	          (std::vector<Row>{R("dave", 4, 25), R("eve", 5, 35)}));
	// Column against column, and literal against literal.
	EXPECT_EQ(Rows("SELECT * FROM people WHERE id = age").size(), 0u);
	EXPECT_EQ(Rows("SELECT * FROM people WHERE 1 = 1").size(), 5u);
	EXPECT_EQ(Rows("SELECT * FROM people WHERE 1 = 2").size(), 0u);
}

TEST_F(ExecutorTest, StringsWithQuotesAndNewlinesRoundTrip) {
	Exec("CREATE TABLE t (id INT64 PRIMARY KEY, s VARCHAR(32))");
	Exec("INSERT INTO t VALUES (1, 'it''s'), (2, ''), (3, 'a\nb'), (4, 'semi; colon')");
	EXPECT_EQ(ById(Rows("SELECT * FROM t"), 0),
	          (std::vector<Row>{{Value{int64_t{1}}, Value{std::string("it's")}},
	                            {Value{int64_t{2}}, Value{std::string("")}},
	                            {Value{int64_t{3}}, Value{std::string("a\nb")}},
	                            {Value{int64_t{4}}, Value{std::string("semi; colon")}}}));
	EXPECT_EQ(Rows("SELECT id FROM t WHERE s = 'it''s'"), (std::vector<Row>{{Value{int64_t{1}}}}));
}

TEST_F(ExecutorTest, Int64EdgesAsKeys) {
	Exec("CREATE TABLE t (id INT64 PRIMARY KEY)");
	Exec("INSERT INTO t VALUES (-9223372036854775808), (9223372036854775807), (0)");
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id = -9223372036854775808"),
	          (std::vector<Row>{{Value{kMin}}}));
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id = 9223372036854775807"),
	          (std::vector<Row>{{Value{kMax}}}));
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id > 9223372036854775807").size(), 0u);
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id < -9223372036854775808").size(), 0u);
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id >= -9223372036854775808").size(), 3u);
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id <= 9223372036854775807").size(), 3u);
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id > -9223372036854775808").size(), 2u);
	EXPECT_EQ(Rows("SELECT * FROM t WHERE id < 9223372036854775807").size(), 2u);
}

// ---- Binder errors: all before anything is touched ----

TEST_F(ExecutorTest, BinderErrors) {
	CreatePeople();
	ExpectError("SELECT * FROM nope", ErrorCode::kNotFound, "no such table: nope");
	ExpectError("INSERT INTO nope VALUES (1)", ErrorCode::kNotFound, "no such table");
	ExpectError("DELETE FROM nope", ErrorCode::kNotFound, "no such table");
	ExpectError("SELECT salary FROM people", ErrorCode::kNotFound, "no such column: salary");
	ExpectError("SELECT * FROM people WHERE salary = 1", ErrorCode::kNotFound, "no such column");
	ExpectError("DELETE FROM people WHERE x = 1", ErrorCode::kNotFound, "no such column");
	ExpectError("SELECT other.id FROM people", ErrorCode::kInvalidArgument, "not in FROM");
	ExpectError("SELECT * FROM people WHERE id = 'x'", ErrorCode::kInvalidArgument,
	            "type mismatch in id = 'x': INT64 compared with VARCHAR");
	ExpectError("SELECT * FROM people WHERE name = age", ErrorCode::kInvalidArgument,
	            "type mismatch");
	ExpectError("SELECT * FROM people WHERE 1 = 'a'", ErrorCode::kInvalidArgument, "type mismatch");
	ExpectError("SELECT * FROM people JOIN people ON id = id", ErrorCode::kInvalidArgument,
	            "JOIN is not supported yet");
	ExpectError("SELECT * FROM people ORDER BY id", ErrorCode::kInvalidArgument,
	            "ORDER BY is not supported yet");
	// Parser errors come through unchanged.
	ExpectError("SELECT * FORM people", ErrorCode::kInvalidArgument, "expected FROM");
	EXPECT_TRUE(Consistent("people"));
	EXPECT_EQ(Rows("SELECT * FROM people").size(), 5u);
}

TEST_F(ExecutorTest, CreateTableErrors) {
	CreatePeople();
	ExpectError("CREATE TABLE people (id INT64 PRIMARY KEY)", ErrorCode::kDuplicateKey,
	            "already exists");
	// Schema::Create's rules, surfaced as they come.
	ExpectError("CREATE TABLE t (name VARCHAR(8) PRIMARY KEY)", ErrorCode::kInvalidArgument, "");
	ExpectError("CREATE TABLE t (id INT64 PRIMARY KEY, id INT64)", ErrorCode::kInvalidArgument, "");
	ExpectError("CREATE TABLE t (id INT64 PRIMARY KEY, s VARCHAR(0))", ErrorCode::kInvalidArgument,
	            "");
	ExpectError("CREATE TABLE t (id INT64 PRIMARY KEY, s VARCHAR(65535))",
	            ErrorCode::kInvalidArgument, "");
	EXPECT_EQ(db_->GetCatalog().TableCount(), 1u);
}

TEST_F(ExecutorTest, InsertIsAllOrNothingForEveryForeseeableError) {
	CreatePeople();
	// Each of these has a good row BEFORE the bad one; none of the good rows may land.
	ExpectError("INSERT INTO people VALUES ('zed', 10, 1), ('bad', 11)",
	            ErrorCode::kInvalidArgument, "row 2");
	ExpectError("INSERT INTO people VALUES ('zed', 10, 1), (11, 'bad', 1)",
	            ErrorCode::kInvalidArgument, "row 2");
	ExpectError("INSERT INTO people VALUES ('zed', 10, 1), ('this name is far too long', 11, 1)",
	            ErrorCode::kInvalidArgument, "row 2");
	ExpectError("INSERT INTO people VALUES ('zed', 10, 1), ('dup', 3, 1)", ErrorCode::kDuplicateKey,
	            "row 2: primary key 3 already exists");
	ExpectError("INSERT INTO people VALUES ('zed', 10, 1), ('twin', 10, 2)",
	            ErrorCode::kDuplicateKey, "appears twice");
	EXPECT_TRUE(Rows("SELECT * FROM people WHERE id = 10").empty());
	EXPECT_EQ(Rows("SELECT * FROM people").size(), 5u);
	EXPECT_TRUE(Consistent("people"));
}

// ---- Planner: EXPLAIN shows the access path ----

TEST_F(ExecutorTest, ExplainChoosesTheAccessPath) {
	CreatePeople();
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people"), (std::vector<std::string>{"SeqScan: people"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id = 3"),
	          (std::vector<std::string>{"IndexScan: people (id = 3)"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE 3 = id"),
	          (std::vector<std::string>{"IndexScan: people (id = 3)"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id > 2 AND id <= 4"),
	          (std::vector<std::string>{"IndexScan: people (id >= 3 AND id <= 4)"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE 10 > id"),
	          (std::vector<std::string>{"IndexScan: people (id <= 9)"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id >= -5"),
	          (std::vector<std::string>{"IndexScan: people (id >= -5)"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id > 5 AND id < 3"),
	          (std::vector<std::string>{"IndexScan: people (id in empty range)"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id = 1 AND id = 2"),
	          (std::vector<std::string>{"IndexScan: people (id in empty range)"}));
	// The key compared with something other than an integer literal is not a range.
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id != 3"),
	          (std::vector<std::string>{"Filter: id != 3", "  SeqScan: people"}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id = age"),
	          (std::vector<std::string>{"Filter: id = age", "  SeqScan: people"}));
	// Range terms are consumed; the rest become the filter; projection on top.
	EXPECT_EQ(Plan("EXPLAIN SELECT name FROM people WHERE age > 26 AND id >= 2 AND name != 'eve'"),
	          (std::vector<std::string>{"Projection: name", "  Filter: age > 26 AND name != 'eve'",
	                                    "    IndexScan: people (id >= 2)"}));
	EXPECT_EQ(
	    Plan("EXPLAIN DELETE FROM people WHERE age = 25"),
	    (std::vector<std::string>{"Delete: people", "  Filter: age = 25", "    SeqScan: people"}));
	EXPECT_EQ(Plan("EXPLAIN DELETE FROM people WHERE id = 1"),
	          (std::vector<std::string>{"Delete: people", "  IndexScan: people (id = 1)"}));
	// EXPLAIN runs nothing.
	EXPECT_EQ(Rows("SELECT * FROM people").size(), 5u);
	EXPECT_EQ(Exec("EXPLAIN SELECT * FROM people").columns, (std::vector<std::string>{"plan"}));
}

TEST_F(ExecutorTest, IndexScanReturnsRowsInKeyOrder) {
	Exec("CREATE TABLE t (id INT64 PRIMARY KEY, v INT64)");
	Exec("INSERT INTO t VALUES (5, 50), (1, 10), (9, 90), (3, 30), (7, 70), (2, 20)");
	EXPECT_EQ(
	    Rows("SELECT id FROM t WHERE id >= 2 AND id <= 7"),
	    (std::vector<Row>{
	        {Value{int64_t{2}}}, {Value{int64_t{3}}}, {Value{int64_t{5}}}, {Value{int64_t{7}}}}));
}

// ---- DELETE ----

TEST_F(ExecutorTest, DeleteByKeyRangeAndFilter) {
	CreatePeople();
	EXPECT_EQ(Exec("DELETE FROM people WHERE id = 1").tag, "DELETE 1");
	EXPECT_EQ(Exec("DELETE FROM people WHERE id = 1").tag, "DELETE 0");  // already gone
	EXPECT_EQ(Exec("DELETE FROM people WHERE age = 25").tag, "DELETE 2");
	EXPECT_EQ(ById(Rows("SELECT * FROM people")),
	          (std::vector<Row>{R("carol", 3, 41), R("eve", 5, 35)}));
	EXPECT_TRUE(Consistent("people"));
	EXPECT_EQ(Exec("DELETE FROM people").tag, "DELETE 2");
	EXPECT_TRUE(Rows("SELECT * FROM people").empty());
	EXPECT_TRUE(Consistent("people"));
	// And the table is usable again.
	Exec("INSERT INTO people VALUES ('back', 1, 1)");
	EXPECT_EQ(Rows("SELECT * FROM people"), (std::vector<Row>{R("back", 1, 1)}));
}

TEST_F(ExecutorTest, DeleteEverythingFromAMultiLevelTable) {
	// Collect-then-mutate: if DELETE deleted while its scan was alive, this would deadlock on the
	// scan's own read latch (and the test would hit its timeout), not fail an assertion.
	Exec("CREATE TABLE t (id INT64 PRIMARY KEY, pad VARCHAR(200))");
	for (int batch = 0; batch < 10; ++batch) {
		std::string sql = "INSERT INTO t VALUES ";
		for (int i = 0; i < 30; ++i) {
			if (i > 0) sql += ", ";
			sql += std::format("({}, '{}')", batch * 30 + i, std::string(150, 'x'));
		}
		Exec(sql);
	}
	EXPECT_EQ(Rows("SELECT id FROM t").size(), 300u);
	EXPECT_EQ(Exec("DELETE FROM t WHERE id >= 100 AND id < 200").tag, "DELETE 100");
	EXPECT_EQ(Exec("DELETE FROM t WHERE pad != ''").tag, "DELETE 200");
	EXPECT_TRUE(Rows("SELECT * FROM t").empty());
	EXPECT_TRUE(Consistent("t"));
}

// ---- Persistence ----

TEST_F(ExecutorTest, TablesAndRowsSurviveAReopen) {
	CreatePeople();
	Exec("DELETE FROM people WHERE id = 2");
	Reopen();
	EXPECT_EQ(ById(Rows("SELECT * FROM people")),
	          (std::vector<Row>{R("alice", 1, 30), R("carol", 3, 41), R("dave", 4, 25),
	                            R("eve", 5, 35)}));
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people WHERE id = 4"),
	          (std::vector<std::string>{"IndexScan: people (id = 4)"}));
	EXPECT_EQ(Rows("SELECT name FROM people WHERE id = 4"),
	          (std::vector<Row>{{Value{std::string("dave")}}}));
	EXPECT_TRUE(Consistent("people"));
}

TEST_F(ExecutorTest, CheckpointMakesStatementsSurviveAKill) {
	// The shell's autocommit (DD-008, "Shell"): statements followed by Checkpoint() survive a
	// process that dies without Shutdown. A real kill, in a child process: in-process, dropping a
	// Database without Shutdown is the pool's caller-bug path and aborts by design.
	executor_.reset();
	ASSERT_TRUE(db_->Shutdown().ok());
	db_.reset();

	const pid_t child = fork();
	ASSERT_NE(child, -1);
	if (child == 0) {
		auto db = Database::Open(path_, kFrames);
		if (!db.has_value()) _exit(2);
		Executor executor((*db)->GetCatalog());
		if (!executor.Execute("CREATE TABLE t (id INT64 PRIMARY KEY, s VARCHAR(8))").has_value())
			_exit(3);
		if (!executor.Execute("INSERT INTO t VALUES (1, 'kept'), (2, 'also')").has_value())
			_exit(4);
		if (!(*db)->Checkpoint().ok()) _exit(5);
		// Written after the checkpoint and never flushed: may or may not survive.
		(void)executor.Execute("INSERT INTO t VALUES (3, 'maybe')");
		_exit(0);  // no Shutdown, no destructors: as close to kill -9 as a test gets
	}
	int status = 0;
	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 0);

	Open();
	const std::vector<Row> rows = Rows("SELECT * FROM t WHERE id <= 2");
	EXPECT_EQ(ById(rows, 0), (std::vector<Row>{{Value{int64_t{1}}, Value{std::string("kept")}},
	                                           {Value{int64_t{2}}, Value{std::string("also")}}}));
	EXPECT_TRUE(Consistent("t"));
}

// ---- Differential: the planner's every path against a hand-evaluated model ----

TEST_F(ExecutorTest, RandomWhereClausesMatchAModel) {
	Exec("CREATE TABLE t (v INT64, id INT64 PRIMARY KEY, s VARCHAR(4))");

	std::mt19937_64 rng(20261115);
	std::map<int64_t, Row> model;  // id -> row
	const auto rand_in = [&](int64_t lo, int64_t hi) {
		return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
	};
	const std::vector<std::string> strings = {"", "a", "ab", "b", "ba", "zz"};

	// Keys clustered in [-40, 200] so ranges hit and miss, plus both INT64 edges.
	std::vector<int64_t> ids = {kMin, kMax, kMin + 1, kMax - 1};
	while (ids.size() < 160) {
		const int64_t id = rand_in(-40, 200);
		if (std::ranges::find(ids, id) == ids.end()) ids.push_back(id);
	}
	for (std::size_t i = 0; i < ids.size(); i += 20) {
		std::string sql = "INSERT INTO t VALUES ";
		for (std::size_t j = i; j < std::min(ids.size(), i + 20); ++j) {
			const int64_t v = rand_in(-5, 5);
			const std::string& s = strings[static_cast<std::size_t>(rand_in(0, 5))];
			if (j > i) sql += ", ";
			sql += std::format("({}, {}, '{}')", v, ids[j], s);
			model[ids[j]] = Row{Value{v}, Value{ids[j]}, Value{s}};
		}
		Exec(sql);
	}

	const std::vector<std::string_view> ops = {"=", "!=", "<", "<=", ">", ">="};
	const auto key_literal = [&]() -> int64_t {
		switch (rand_in(0, 9)) {
			case 0:
				return kMin;
			case 1:
				return kMax;
			case 2:
				return kMin + 1;
			case 3:
				return kMax - 1;
			default:
				return rand_in(-50, 210);
		}
	};
	const auto holds = [](std::string_view op, const Value& a, const Value& b) {
		if (op == "=") return a == b;
		if (op == "!=") return a != b;
		if (op == "<") return a < b;
		if (op == "<=") return a <= b;
		if (op == ">") return a > b;
		return a >= b;
	};

	for (int query = 0; query < 400; ++query) {
		// 1–3 terms; each picks a column (key, v, s, or v-vs-id), an operator and a side.
		std::string where;
		std::vector<std::function<bool(const Row&)>> terms;
		const int64_t n = rand_in(1, 3);
		for (int64_t k = 0; k < n; ++k) {
			const std::string_view op = ops[static_cast<std::size_t>(rand_in(0, 5))];
			const bool flip = rand_in(0, 1) == 1;
			std::string lhs;
			std::string rhs;
			std::size_t column = 0;
			Value literal;
			bool column_vs_column = false;
			switch (rand_in(0, 4)) {
				case 0:
				case 1:  // the key, twice as often: ranges are the interesting part
					column = 1;
					literal = Value{key_literal()};
					break;
				case 2:
					column = 0;
					literal = Value{rand_in(-6, 6)};
					break;
				case 3:
					column = 2;
					literal = Value{strings[static_cast<std::size_t>(rand_in(0, 5))]};
					break;
				default:
					column_vs_column = true;
					break;
			}
			if (column_vs_column) {
				lhs = flip ? "id" : "v";
				rhs = flip ? "v" : "id";
				terms.push_back([=](const Row& r) {
					return flip ? holds(op, r[1], r[0]) : holds(op, r[0], r[1]);
				});
			} else {
				const std::string name = column == 0 ? "v" : column == 1 ? "id" : "s";
				std::string lit = ToSql(literal);
				lhs = flip ? lit : name;
				rhs = flip ? name : lit;
				terms.push_back([=](const Row& r) {
					return flip ? holds(op, literal, r[column]) : holds(op, r[column], literal);
				});
			}
			where += std::format("{}{} {} {}", k == 0 ? "" : " AND ", lhs, op, rhs);
		}

		const std::string sql = "SELECT * FROM t WHERE " + where;
		std::vector<Row> expected;
		for (const auto& [id, row] : model) {
			if (std::ranges::all_of(terms, [&](const auto& term) { return term(row); })) {
				expected.push_back(row);
			}
		}
		ASSERT_EQ(ById(Rows(sql)), expected) << sql;

		// Every 20th query, delete what it matched instead, and check the table still agrees.
		if (query % 20 == 19) {
			EXPECT_EQ(Exec("DELETE FROM t WHERE " + where).tag,
			          std::format("DELETE {}", expected.size()))
			    << where;
			for (const Row& row : expected) model.erase(std::get<int64_t>(row[1]));
			ASSERT_TRUE(Consistent("t")) << where;
		}
	}
	std::vector<Row> all;
	for (const auto& [id, row] : model) all.push_back(row);
	EXPECT_EQ(ById(Rows("SELECT * FROM t")), all);
}

// ---- FormatResult ----

TEST(FormatResultTest, PsqlStyleTable) {
	const QueryResult result{.columns = {"id", "name"},
	                         .rows = {{Value{int64_t{1}}, Value{std::string("alice")}},
	                                  {Value{int64_t{-20}}, Value{std::string("bo")}}},
	                         .tag = "SELECT 2"};
	EXPECT_EQ(FormatResult(result),
	          " id  | name\n"
	          "-----+-------\n"
	          "   1 | alice\n"
	          " -20 | bo\n"
	          "(2 rows)");
}

TEST(FormatResultTest, OneRowZeroRowsAndTagOnly) {
	EXPECT_EQ(FormatResult(QueryResult{.columns = {"x"}, .rows = {{Value{int64_t{7}}}}}),
	          " x\n"
	          "---\n"
	          " 7\n"
	          "(1 row)");
	EXPECT_EQ(FormatResult(QueryResult{.columns = {"x"}}),
	          " x\n"
	          "---\n"
	          "(0 rows)");
	EXPECT_EQ(FormatResult(QueryResult{.tag = "INSERT 3"}), "INSERT 3");
}

// ---- SplitStatements ----

TEST(SplitStatementsTest, CutsAfterEverySemicolonToken) {
	const SplitInput split = SplitStatements("SELECT 1; SELECT 2;\nSELECT");
	EXPECT_EQ(split.statements, (std::vector<std::string>{"SELECT 1;", " SELECT 2;"}));
	EXPECT_EQ(split.remainder, "\nSELECT");
}

TEST(SplitStatementsTest, NoSemicolonYetIsAllRemainder) {
	const SplitInput split = SplitStatements("SELECT *\nFROM t\n");
	EXPECT_TRUE(split.statements.empty());
	EXPECT_EQ(split.remainder, "SELECT *\nFROM t\n");
}

TEST(SplitStatementsTest, SemicolonsInStringsAndCommentsDoNotSplit) {
	const SplitInput split = SplitStatements("INSERT INTO t VALUES ('a;b'); -- c; d\n");
	EXPECT_EQ(split.statements, (std::vector<std::string>{"INSERT INTO t VALUES ('a;b');"}));
	EXPECT_EQ(split.remainder, "");  // only a comment after the last ';'
}

TEST(SplitStatementsTest, AnOpenStringWaitsForMoreInput) {
	const SplitInput split = SplitStatements("INSERT INTO t VALUES ('line one;\n");
	EXPECT_TRUE(split.statements.empty());
	EXPECT_EQ(split.remainder, "INSERT INTO t VALUES ('line one;\n");
}

TEST(SplitStatementsTest, OtherLexerErrorsAreHandedOnToReportThem) {
	const SplitInput split = SplitStatements("SELECT @ FROM t\n");
	EXPECT_EQ(split.statements, (std::vector<std::string>{"SELECT @ FROM t\n"}));
	EXPECT_EQ(split.remainder, "");
}

TEST(SplitStatementsTest, BlankInputIsNothing) {
	for (std::string_view input : {"", "\n", "   -- just a comment\n"}) {
		const SplitInput split = SplitStatements(input);
		EXPECT_TRUE(split.statements.empty()) << input;
		EXPECT_EQ(split.remainder, "") << input;
	}
}

}  // namespace kernsql

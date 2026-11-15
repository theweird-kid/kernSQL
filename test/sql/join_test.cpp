#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
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
#include "sql/executor/executor.hpp"
#include "sql/executor/planner.hpp"
#include "sql/parser/ast.hpp"

namespace kernsql {

/*
 * JOIN and ORDER BY (DD-008, "Joins"). Every query here runs TWICE, through an executor that
 * hash-joins and one that nested-loop-joins, and the two must return the same rows in the same
 * order: nested loop is the oracle for hash join. Run() does both and returns one result.
 *
 * On top of that, the differential test checks both against a model joined by hand, over tables
 * with many duplicate join values — the case where a hash join that keeps one match per key, or
 * drops a bucket, goes wrong.
 */
class JoinTest : public ::testing::Test {
  protected:
	static constexpr std::size_t kFrames = 32;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_join_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
		         std::to_string(getpid()));
		std::filesystem::remove(path_);
		auto db = Database::Open(path_, kFrames);
		ASSERT_TRUE(db.has_value()) << db.error().message();
		db_ = std::move(*db);
		hash_ = std::make_unique<Executor>(db_->GetCatalog(), MIN_FANOUT, JoinAlgorithm::kHash);
		nested_ =
		    std::make_unique<Executor>(db_->GetCatalog(), MIN_FANOUT, JoinAlgorithm::kNestedLoop);
	}

	void TearDown() override {
		hash_.reset();
		nested_.reset();
		if (db_) {
			EXPECT_TRUE(db_->Shutdown().ok());
		}
		db_.reset();
		std::filesystem::remove(path_);
	}

	// Through both join algorithms; they must agree exactly. Nothing may be left pinned.
	QueryResult Run(std::string_view sql) {
		Result<QueryResult> a = hash_->Execute(sql);
		EXPECT_TRUE(a.has_value()) << sql << "\n  → " << a.error().message();
		EXPECT_EQ(db_->Pool().GetStats().pinned_frames, 0u) << "pin leaked by: " << sql;
		Result<QueryResult> b = nested_->Execute(sql);
		EXPECT_TRUE(b.has_value()) << sql << "\n  → " << b.error().message();
		if (!a.has_value() || !b.has_value()) return QueryResult{};
		EXPECT_EQ(a->columns, b->columns) << sql;
		EXPECT_EQ(a->rows, b->rows) << "hash join and nested loop disagree: " << sql;
		return *a;
	}

	std::vector<Row> Rows(std::string_view sql) { return Run(sql).rows; }

	// For statements that write: run once.
	void Exec(std::string_view sql) {
		Result<QueryResult> r = hash_->Execute(sql);
		ASSERT_TRUE(r.has_value()) << sql << "\n  → " << r.error().message();
	}

	void ExpectError(std::string_view sql, ErrorCode code, std::string_view fragment) {
		Result<QueryResult> r = hash_->Execute(sql);
		ASSERT_FALSE(r.has_value()) << "succeeded: " << sql;
		EXPECT_EQ(r.error().code(), code) << sql << "\n  → " << r.error().message();
		EXPECT_NE(std::string(r.error().message()).find(fragment), std::string::npos)
		    << sql << "\n  → " << r.error().message();
	}

	std::vector<std::string> Plan(std::string_view sql) {
		std::vector<std::string> lines;
		// The hash executor only: EXPLAIN names the join algorithm, so the two differ by design.
		Result<QueryResult> r = hash_->Execute(sql);
		EXPECT_TRUE(r.has_value()) << sql << "\n  → " << r.error().message();
		if (!r.has_value()) return lines;
		for (const Row& row : r->rows) lines.push_back(std::get<std::string>(row[0]));
		return lines;
	}

	static std::vector<Row> Sorted(std::vector<Row> rows) {
		std::ranges::sort(rows);
		return rows;
	}

	static Value I(int64_t v) { return Value{v}; }
	static Value S(std::string v) { return Value{std::move(v)}; }

	// people: 1..4; pets: owner 1 twice, 2 once, 9 (nobody), and person 3 and 4 have none.
	void CreatePeopleAndPets() {
		Exec("CREATE TABLE people (id INT64 PRIMARY KEY, name VARCHAR(16), city VARCHAR(8))");
		Exec(
		    "INSERT INTO people VALUES (1, 'alice', 'oslo'), (2, 'bob', 'rome'), "
		    "(3, 'carol', 'oslo'), (4, 'dave', 'lima')");
		Exec("CREATE TABLE pets (id INT64 PRIMARY KEY, owner INT64, name VARCHAR(16))");
		Exec(
		    "INSERT INTO pets VALUES (10, 1, 'rex'), (11, 1, 'tom'), (12, 2, 'kit'), "
		    "(13, 9, 'stray')");
	}

	std::filesystem::path path_;
	std::unique_ptr<Database> db_;
	std::unique_ptr<Executor> hash_;
	std::unique_ptr<Executor> nested_;
};

// ---- Results ----

TEST_F(JoinTest, InnerJoinKeepsOnlyMatchingPairs) {
	CreatePeopleAndPets();
	EXPECT_EQ(Sorted(Rows("SELECT people.name, pets.name FROM people JOIN pets "
	                      "ON people.id = pets.owner")),
	          Sorted({{S("alice"), S("rex")}, {S("alice"), S("tom")}, {S("bob"), S("kit")}}));
}

TEST_F(JoinTest, SelectStarIsLeftColumnsThenRight) {
	CreatePeopleAndPets();
	const QueryResult result =
	    Run("SELECT * FROM people JOIN pets ON people.id = pets.owner WHERE pets.id = 12");
	EXPECT_EQ(result.columns,
	          (std::vector<std::string>{"id", "name", "city", "id", "owner", "name"}));
	EXPECT_EQ(result.rows, (std::vector<Row>{{I(2), S("bob"), S("rome"), I(12), I(2), S("kit")}}));
}

TEST_F(JoinTest, OnSidesCanBeWrittenEitherWay) {
	CreatePeopleAndPets();
	EXPECT_EQ(Sorted(Rows("SELECT pets.id FROM people JOIN pets ON pets.owner = people.id")),
	          Sorted(Rows("SELECT pets.id FROM people JOIN pets ON people.id = pets.owner")));
}

TEST_F(JoinTest, UnqualifiedNamesResolveWhenUnambiguous) {
	CreatePeopleAndPets();
	// `owner` and `city` exist in one table only.
	EXPECT_EQ(Sorted(Rows("SELECT city, owner FROM people JOIN pets ON people.id = owner")),
	          Sorted({{S("oslo"), I(1)}, {S("oslo"), I(1)}, {S("rome"), I(2)}}));
}

TEST_F(JoinTest, ManyToManyDuplicatesProduceEveryPair) {
	Exec("CREATE TABLE a (id INT64 PRIMARY KEY, k INT64)");
	Exec("CREATE TABLE b (id INT64 PRIMARY KEY, k INT64)");
	// k=1: 3 x 2 pairs, k=2: 1 x 3, k=3: 2 x 0, k=4: 0 x 1.
	Exec("INSERT INTO a VALUES (1, 1), (2, 1), (3, 1), (4, 2), (5, 3), (6, 3)");
	Exec("INSERT INTO b VALUES (1, 1), (2, 1), (3, 2), (4, 2), (5, 2), (6, 4)");
	EXPECT_EQ(Rows("SELECT * FROM a JOIN b ON a.k = b.k").size(), 3u * 2u + 1u * 3u);
}

TEST_F(JoinTest, StringJoinKeys) {
	Exec("CREATE TABLE cities (id INT64 PRIMARY KEY, name VARCHAR(8))");
	Exec("CREATE TABLE people (id INT64 PRIMARY KEY, city VARCHAR(8))");
	Exec("INSERT INTO cities VALUES (1, 'oslo'), (2, 'rome'), (3, '')");
	Exec("INSERT INTO people VALUES (1, 'oslo'), (2, 'Oslo'), (3, ''), (4, 'oslo')");
	// Exact bytes: 'Oslo' matches nothing; '' matches ''.
	EXPECT_EQ(Sorted(Rows("SELECT people.id, cities.id FROM people JOIN cities "
	                      "ON people.city = cities.name")),
	          Sorted({{I(1), I(1)}, {I(3), I(3)}, {I(4), I(1)}}));
}

TEST_F(JoinTest, EmptySidesJoinToNothing) {
	Exec("CREATE TABLE a (id INT64 PRIMARY KEY)");
	Exec("CREATE TABLE b (id INT64 PRIMARY KEY)");
	EXPECT_TRUE(Rows("SELECT * FROM a JOIN b ON a.id = b.id").empty());
	Exec("INSERT INTO a VALUES (1), (2)");
	EXPECT_TRUE(Rows("SELECT * FROM a JOIN b ON a.id = b.id").empty());
	EXPECT_TRUE(Rows("SELECT * FROM b JOIN a ON a.id = b.id").empty());
}

TEST_F(JoinTest, WhereTermsOnEitherSideAndAcross) {
	CreatePeopleAndPets();
	EXPECT_EQ(Sorted(Rows("SELECT pets.name FROM people JOIN pets ON people.id = pets.owner "
	                      "WHERE people.city = 'oslo' AND pets.name != 'tom'")),
	          Sorted({{S("rex")}}));
	EXPECT_EQ(Sorted(Rows("SELECT pets.name FROM people JOIN pets ON people.id = pets.owner "
	                      "WHERE pets.id > people.id AND 1 = 1")),
	          Sorted({{S("rex")}, {S("tom")}, {S("kit")}}));
	EXPECT_TRUE(
	    Rows("SELECT * FROM people JOIN pets ON people.id = pets.owner WHERE 1 = 2").empty());
}

// ---- Binder errors ----

TEST_F(JoinTest, BinderErrors) {
	CreatePeopleAndPets();
	ExpectError("SELECT name FROM people JOIN pets ON people.id = pets.owner",
	            ErrorCode::kInvalidArgument, "ambiguous column name: name");
	ExpectError("SELECT * FROM people JOIN pets ON id = owner", ErrorCode::kInvalidArgument,
	            "ambiguous column name: id");
	ExpectError("SELECT * FROM people JOIN pets ON people.id = pets.owner ORDER BY name",
	            ErrorCode::kInvalidArgument, "ambiguous");
	ExpectError("SELECT * FROM people JOIN people ON people.id = people.id",
	            ErrorCode::kInvalidArgument, "cannot join people with itself");
	ExpectError("SELECT * FROM people JOIN nope ON people.id = nope.id", ErrorCode::kNotFound,
	            "no such table: nope");
	ExpectError("SELECT * FROM people JOIN pets ON people.id < pets.owner",
	            ErrorCode::kInvalidArgument, "must be an equality");
	ExpectError("SELECT * FROM people JOIN pets ON people.id = 1", ErrorCode::kInvalidArgument,
	            "must be an equality");
	ExpectError("SELECT * FROM people JOIN pets ON pets.id = pets.owner",
	            ErrorCode::kInvalidArgument, "must be an equality");
	ExpectError("SELECT * FROM people JOIN pets ON people.name = pets.owner",
	            ErrorCode::kInvalidArgument, "type mismatch");
	ExpectError("SELECT * FROM people JOIN pets ON people.id = pets.nope", ErrorCode::kNotFound,
	            "no such column: pets.nope");
	ExpectError("SELECT toys.id FROM people JOIN pets ON people.id = pets.owner",
	            ErrorCode::kInvalidArgument, "not in FROM");
	ExpectError("SELECT * FROM people ORDER BY nope", ErrorCode::kNotFound, "no such column");
}

// ---- EXPLAIN: pushdown and operator order ----

TEST_F(JoinTest, ExplainShowsPushdownJoinSortAndProjection) {
	CreatePeopleAndPets();
	EXPECT_EQ(Plan("EXPLAIN SELECT * FROM people JOIN pets ON people.id = pets.owner"),
	          (std::vector<std::string>{"HashJoin: people.id = pets.owner", "  SeqScan: people",
	                                    "  SeqScan: pets"}));
	// One-table terms go below the join, so the key term still becomes an IndexScan; the
	// cross-table term stays above.
	EXPECT_EQ(Plan("EXPLAIN SELECT pets.name FROM people JOIN pets ON people.id = pets.owner "
	               "WHERE pets.id >= 11 AND people.city = 'oslo' AND people.id < pets.id "
	               "ORDER BY people.name DESC, pets.id"),
	          (std::vector<std::string>{
	              "Projection: name",
	              "  Sort: name DESC, id",
	              "    Filter: people.id < pets.id",
	              "      HashJoin: people.id = pets.owner",
	              "        Filter: people.city = 'oslo'",
	              "          SeqScan: people",
	              "        IndexScan: pets (id >= 11)",
	          }));
	EXPECT_EQ(Plan("EXPLAIN SELECT name FROM people WHERE id > 1 ORDER BY city"),
	          (std::vector<std::string>{"Projection: name", "  Sort: city",
	                                    "    IndexScan: people (id >= 2)"}));
}

TEST_F(JoinTest, NestedLoopNamesItselfInExplain) {
	CreatePeopleAndPets();
	Result<QueryResult> r =
	    nested_->Execute("EXPLAIN SELECT * FROM people JOIN pets ON people.id = pets.owner");
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(std::get<std::string>(r->rows[0][0]), "NestedLoopJoin: people.id = pets.owner");
}

// ---- ORDER BY ----

TEST_F(JoinTest, OrderByAscDescAndTies) {
	Exec("CREATE TABLE t (id INT64 PRIMARY KEY, g INT64, s VARCHAR(4))");
	Exec("INSERT INTO t VALUES (1, 2, 'b'), (2, 1, 'a'), (3, 2, 'a'), (4, 1, 'c'), (5, 3, 'a')");
	EXPECT_EQ(Rows("SELECT id FROM t ORDER BY s, id DESC"),
	          (std::vector<Row>{{I(5)}, {I(3)}, {I(2)}, {I(1)}, {I(4)}}));
	EXPECT_EQ(Rows("SELECT id FROM t ORDER BY g DESC, s"),
	          (std::vector<Row>{{I(5)}, {I(3)}, {I(1)}, {I(2)}, {I(4)}}));
	EXPECT_EQ(Rows("SELECT id FROM t ORDER BY id DESC"),
	          (std::vector<Row>{{I(5)}, {I(4)}, {I(3)}, {I(2)}, {I(1)}}));
	// A sort key that is not projected.
	EXPECT_EQ(Rows("SELECT s FROM t WHERE g = 1 ORDER BY id"),
	          (std::vector<Row>{{S("a")}, {S("c")}}));
	EXPECT_TRUE(Rows("SELECT * FROM t WHERE id > 99 ORDER BY s").empty());
}

TEST_F(JoinTest, OrderByOverAJoin) {
	CreatePeopleAndPets();
	EXPECT_EQ(
	    Rows("SELECT people.name, pets.name FROM people JOIN pets "
	         "ON people.id = pets.owner ORDER BY pets.name DESC"),
	    (std::vector<Row>{{S("alice"), S("tom")}, {S("alice"), S("rex")}, {S("bob"), S("kit")}}));
}

// ---- Differential: hash join, nested loop, and a hand-joined model ----

TEST_F(JoinTest, RandomJoinsMatchAModel) {
	Exec("CREATE TABLE l (id INT64 PRIMARY KEY, k INT64, s VARCHAR(2))");
	Exec("CREATE TABLE r (id INT64 PRIMARY KEY, k INT64, s VARCHAR(2))");

	std::mt19937_64 rng(20261115);
	const auto rand_in = [&](int64_t lo, int64_t hi) {
		return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
	};
	const std::vector<std::string> strings = {"", "a", "b", "ab"};
	// Small value domains, so join columns are full of duplicates on both sides.
	std::map<int64_t, Row> left;
	std::map<int64_t, Row> right;
	for (auto* side : {&left, &right}) {
		const std::string name = side == &left ? "l" : "r";
		std::string sql = "INSERT INTO " + name + " VALUES ";
		for (int64_t id = 0; id < 60; ++id) {
			const int64_t k = rand_in(0, 7);
			const std::string& s = strings[static_cast<std::size_t>(rand_in(0, 3))];
			if (id > 0) sql += ", ";
			sql += std::format("({}, {}, '{}')", id, k, s);
			(*side)[id] = Row{Value{id}, Value{k}, Value{s}};
		}
		Exec(sql);
	}

	struct On {
		std::string sql;
		std::size_t left_col;
		std::size_t right_col;
	};
	const std::vector<On> ons = {
	    {"l.k = r.k", 1, 1}, {"r.s = l.s", 2, 2}, {"l.id = r.k", 0, 1}, {"r.id = l.k", 1, 0}};
	const std::vector<std::string_view> ops = {"=", "!=", "<", "<=", ">", ">="};
	const auto holds = [](std::string_view op, const Value& a, const Value& b) {
		if (op == "=") return a == b;
		if (op == "!=") return a != b;
		if (op == "<") return a < b;
		if (op == "<=") return a <= b;
		if (op == ">") return a > b;
		return a >= b;
	};

	for (int query = 0; query < 150; ++query) {
		const On& on = ons[static_cast<std::size_t>(rand_in(0, 3))];
		// 0–2 WHERE terms on combined-row columns (0..2 = l, 3..5 = r), int columns only.
		const std::vector<std::string> names = {"l.id", "l.k", "l.s", "r.id", "r.k", "r.s"};
		std::string where;
		std::vector<std::function<bool(const Row&)>> terms;
		for (int64_t t = rand_in(0, 2); t > 0; --t) {
			const std::size_t column = static_cast<std::size_t>(rand_in(0, 1)) * 3 +
			                           static_cast<std::size_t>(rand_in(0, 1));  // id or k
			const std::string_view op = ops[static_cast<std::size_t>(rand_in(0, 5))];
			const Value literal{rand_in(-1, 60)};
			where += std::format("{}{} {} {}", where.empty() ? " WHERE " : " AND ", names[column],
			                     op, ToSql(literal));
			terms.push_back([=](const Row& row) { return holds(op, row[column], literal); });
		}
		const bool ordered = rand_in(0, 1) == 1;
		const std::string sql = std::format("SELECT * FROM l JOIN r ON {}{}{}", on.sql, where,
		                                    ordered ? " ORDER BY r.s DESC, l.id, r.id" : "");

		std::vector<Row> expected;
		for (const auto& [lid, lrow] : left) {
			for (const auto& [rid, rrow] : right) {
				if (lrow[on.left_col] != rrow[on.right_col]) continue;
				Row joined = lrow;
				joined.insert(joined.end(), rrow.begin(), rrow.end());
				if (std::ranges::all_of(terms, [&](const auto& term) { return term(joined); })) {
					expected.push_back(std::move(joined));
				}
			}
		}

		const std::vector<Row> actual = Rows(sql);  // also checks hash == nested loop
		if (ordered) {
			// The ORDER BY keys are a total order here (l.id, r.id are unique), so the exact
			// sequence is determined.
			std::ranges::sort(expected, [](const Row& a, const Row& b) {
				if (a[5] != b[5]) return a[5] > b[5];
				if (a[0] != b[0]) return a[0] < b[0];
				return a[3] < b[3];
			});
			ASSERT_EQ(actual, expected) << sql;
		} else {
			ASSERT_EQ(Sorted(actual), Sorted(expected)) << sql;
		}
	}
}

}  // namespace kernsql

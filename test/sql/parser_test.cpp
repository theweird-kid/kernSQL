#include "sql/parser/parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "catalog/schema.hpp"
#include "common/status.hpp"
#include "sql/parser/ast.hpp"
#include "sql/parser/token.hpp"

namespace kernsql {

/*
 * DD-007, "Testing". Three kinds of check, each catching what the others miss:
 *
 *   - GOLDEN: messy input → expected canonical ToSql. Pins what every grammar feature means.
 *   - AST: a few trees built by hand and compared with ==. Golden alone would pass if ToSql and
 *     the parser were wrong in matching ways (say, both swapping a comparison's sides).
 *   - ROUND TRIP: Parse(ToSql(ast)) == ast. Pins that the canonical form is itself parseable.
 *
 * plus error positions, integer edges, every token-boundary prefix, and random token soup.
 */

namespace {

ParsedStatement MustParse(std::string_view sql) {
	Result<ParsedStatement> parsed = Parser::Parse(sql);
	EXPECT_TRUE(parsed.has_value()) << sql << " → " << parsed.error().message();
	return parsed.has_value() ? *parsed : ParsedStatement{};
}

// Parse must fail with kInvalidArgument, at `line`:`column`, with `fragment` in the message.
void ExpectError(std::string_view sql, std::string_view fragment, std::size_t line,
                 std::size_t column) {
	Result<ParsedStatement> parsed = Parser::Parse(sql);
	ASSERT_FALSE(parsed.has_value()) << "parsed without error: " << sql;
	EXPECT_EQ(parsed.error().code(), ErrorCode::kInvalidArgument) << sql;
	const std::string message(parsed.error().message());
	const std::string where = "line " + std::to_string(line) + ", column " + std::to_string(column);
	EXPECT_NE(message.find(where), std::string::npos) << sql << "\n  → " << message;
	EXPECT_NE(message.find(fragment), std::string::npos) << sql << "\n  → " << message;
}

int64_t ParseIntLiteral(std::string_view literal) {
	const ParsedStatement s = MustParse("INSERT INTO t VALUES (" + std::string(literal) + ")");
	const auto* insert = std::get_if<InsertStmt>(&s.body);
	if (insert == nullptr || insert->rows.size() != 1 || insert->rows[0].size() != 1) {
		ADD_FAILURE() << "unexpected shape for " << literal;
		return 0;
	}
	return std::get<int64_t>(insert->rows[0][0]);
}

struct Golden {
	std::string_view input;
	std::string_view canonical;
};

// One per grammar feature, written in messy case and spacing so normalisation is tested too.
const Golden kGolden[] = {
    // SELECT: projection, qualified names, every operator, flat AND.
    {"select * from T", "SELECT * FROM t"},
    {"SELECT a,b ,  c FROM t", "SELECT a, b, c FROM t"},
    {"select T.a, B from t where a=1 and b <> 'x'",
     "SELECT t.a, b FROM t WHERE a = 1 AND b != 'x'"},
    {"SELECT a FROM t WHERE a=1 AND a!=2 AND a<3 AND a<=4 AND a>5 AND a>=-6",
     "SELECT a FROM t WHERE a = 1 AND a != 2 AND a < 3 AND a <= 4 AND a > 5 AND a >= -6"},
    {"SELECT * FROM t WHERE 1 = 1", "SELECT * FROM t WHERE 1 = 1"},
    {"SELECT * FROM t WHERE 'a' < name", "SELECT * FROM t WHERE 'a' < name"},
    {"SELECT * FROM t WHERE a = -0", "SELECT * FROM t WHERE a = 0"},
    {"select * from t where x = 007", "SELECT * FROM t WHERE x = 7"},
    // JOIN and ORDER BY.
    {"select * from a join b on a.id = b.aid where a.x > 0 order by a.x desc, b.y asc, z",
     "SELECT * FROM a JOIN b ON a.id = b.aid WHERE a.x > 0 ORDER BY a.x DESC, b.y, z"},
    {"SELECT * FROM t ORDER BY a", "SELECT * FROM t ORDER BY a"},
    {"SELECT a FROM t JOIN u ON t.k = u.k", "SELECT a FROM t JOIN u ON t.k = u.k"},
    // INSERT.
    {"insert into t values (1, 'it''s'), (-2, '')", "INSERT INTO t VALUES (1, 'it''s'), (-2, '')"},
    {"INSERT INTO t VALUES ('line1\nline2')", "INSERT INTO t VALUES ('line1\nline2')"},
    {"INSERT INTO t VALUES (9223372036854775807, -9223372036854775808)",
     "INSERT INTO t VALUES (9223372036854775807, -9223372036854775808)"},
    // CREATE TABLE: INT synonym, PK not first, VARCHAR bounds left to Schema::Create.
    {"create table Users (id int primary key, name varchar(32), age INT64)",
     "CREATE TABLE users (id INT64 PRIMARY KEY, name VARCHAR(32), age INT64)"},
    {"CREATE TABLE t (a VARCHAR(5), b INT64 PRIMARY KEY)",
     "CREATE TABLE t (a VARCHAR(5), b INT64 PRIMARY KEY)"},
    {"CREATE TABLE t (a VARCHAR(0) PRIMARY KEY, b VARCHAR(65535))",
     "CREATE TABLE t (a VARCHAR(0) PRIMARY KEY, b VARCHAR(65535))"},
    // DELETE.
    {"delete from t", "DELETE FROM t"},
    {"DELETE FROM t WHERE id = 5;", "DELETE FROM t WHERE id = 5"},
    // EXPLAIN.
    {"explain select a from t where id = 1", "EXPLAIN SELECT a FROM t WHERE id = 1"},
    {"EXPLAIN DELETE FROM t WHERE a < 0", "EXPLAIN DELETE FROM t WHERE a < 0"},
    // Comments, newlines, trailing ';' with space after it.
    {"SELECT a -- first\n, b\nFROM t", "SELECT a, b FROM t"},
    {"  SELECT * FROM t ;  -- done", "SELECT * FROM t"},
};

}  // namespace

// ---- Golden and round trip ----

TEST(ParserTest, GoldenCanonicalSql) {
	for (const Golden& g : kGolden) {
		EXPECT_EQ(ToSql(MustParse(g.input)), g.canonical) << g.input;
	}
}

TEST(ParserTest, RoundTripThroughCanonicalSql) {
	for (const Golden& g : kGolden) {
		const ParsedStatement first = MustParse(g.input);
		const std::string canonical = ToSql(first);
		EXPECT_EQ(MustParse(canonical), first) << g.input << "\n  canonical: " << canonical;
		// And the canonical form is a fixed point.
		EXPECT_EQ(ToSql(MustParse(canonical)), canonical);
	}
}

// ---- AST built by hand ----

TEST(ParserTest, SelectAst) {
	const ParsedStatement s = MustParse(
	    "SELECT t.a, b FROM t JOIN u ON t.k = u.k WHERE b >= -3 AND 'x' != t.a "
	    "ORDER BY b DESC, a");
	ParsedStatement expected;
	expected.body = SelectStmt{
	    .columns = {{"t", "a"}, {"", "b"}},
	    .table = "t",
	    .join = JoinClause{.table = "u",
	                       .on = {ColumnRef{"t", "k"}, CompareOp::kEqual, ColumnRef{"u", "k"}}},
	    .where = {{ColumnRef{"", "b"}, CompareOp::kGreaterEqual, Value{int64_t{-3}}},
	              {Value{std::string("x")}, CompareOp::kNotEqual, ColumnRef{"t", "a"}}},
	    .order_by = {{.column = {"", "b"}, .descending = true},
	                 {.column = {"", "a"}, .descending = false}},
	};
	EXPECT_EQ(s, expected);
}

TEST(ParserTest, SelectStarIsAnEmptyProjection) {
	const ParsedStatement s = MustParse("SELECT * FROM t");
	const auto& select = std::get<SelectStmt>(s.body);
	EXPECT_TRUE(select.columns.empty());
	EXPECT_FALSE(select.join.has_value());
	EXPECT_TRUE(select.where.empty());
	EXPECT_TRUE(select.order_by.empty());
	EXPECT_FALSE(s.explain);
}

TEST(ParserTest, CreateTableAst) {
	const ParsedStatement s =
	    MustParse("CREATE TABLE t (name VARCHAR(20), id INT PRIMARY KEY, n INT64)");
	ParsedStatement expected;
	expected.body = CreateTableStmt{
	    .table = "t",
	    .columns = {{"name", ColumnType::kVarchar, 20},
	                {"id", ColumnType::kInt64, 0},
	                {"n", ColumnType::kInt64, 0}},
	    .primary_key = 1,
	};
	EXPECT_EQ(s, expected);
}

TEST(ParserTest, InsertAst) {
	const ParsedStatement s = MustParse("INSERT INTO t VALUES (1, 'a'), (2), ('', -5, 0)");
	ParsedStatement expected;
	expected.body = InsertStmt{
	    .table = "t",
	    .rows = {{Value{int64_t{1}}, Value{std::string("a")}},
	             {Value{int64_t{2}}},
	             {Value{std::string()}, Value{int64_t{-5}}, Value{int64_t{0}}}},
	};
	EXPECT_EQ(s, expected);
}

TEST(ParserTest, DeleteAndExplainAst) {
	const ParsedStatement s = MustParse("EXPLAIN DELETE FROM t WHERE id = 5");
	ParsedStatement expected;
	expected.explain = true;
	expected.body = DeleteStmt{
	    .table = "t",
	    .where = {{ColumnRef{"", "id"}, CompareOp::kEqual, Value{int64_t{5}}}},
	};
	EXPECT_EQ(s, expected);

	const ParsedStatement unfiltered = MustParse("DELETE FROM t");
	EXPECT_TRUE(std::get<DeleteStmt>(unfiltered.body).where.empty());
}

TEST(ParserTest, BothNotEqualSpellingsAreTheSameOp) {
	EXPECT_EQ(MustParse("SELECT * FROM t WHERE a != 1"), MustParse("SELECT * FROM t WHERE a <> 1"));
}

TEST(ParserTest, AscIsTheDefault) {
	EXPECT_EQ(MustParse("SELECT * FROM t ORDER BY a ASC"), MustParse("SELECT * FROM t ORDER BY a"));
}

// ---- Integers (DD-007, "Literals and the INT64_MIN trap") ----

TEST(ParserTest, Int64Edges) {
	EXPECT_EQ(ParseIntLiteral("9223372036854775807"), std::numeric_limits<int64_t>::max());
	EXPECT_EQ(ParseIntLiteral("-9223372036854775808"), std::numeric_limits<int64_t>::min());
	EXPECT_EQ(ParseIntLiteral("-9223372036854775807"), std::numeric_limits<int64_t>::min() + 1);
	EXPECT_EQ(ParseIntLiteral("-0"), 0);
	EXPECT_EQ(ParseIntLiteral("0000000000000000000000000042"), 42);  // leading zeros don't count

	ExpectError("INSERT INTO t VALUES (9223372036854775808)", "out of range", 1, 23);
	ExpectError("INSERT INTO t VALUES (-9223372036854775809)", "out of range", 1, 24);
	// Beyond uint64_t too: from_chars itself overflows.
	ExpectError("INSERT INTO t VALUES (18446744073709551616)", "out of range", 1, 23);
	ExpectError("INSERT INTO t VALUES (-99999999999999999999999)", "out of range", 1, 24);
}

TEST(ParserTest, VarcharLengthMustFitUint16) {
	MustParse("CREATE TABLE t (a VARCHAR(65535) PRIMARY KEY)");
	ExpectError("CREATE TABLE t (a VARCHAR(65536) PRIMARY KEY)", "out of range", 1, 27);
	ExpectError("CREATE TABLE t (a VARCHAR(70000) PRIMARY KEY)", "out of range", 1, 27);
	ExpectError("CREATE TABLE t (a VARCHAR(-1) PRIMARY KEY)", "expected VARCHAR length", 1, 27);
}

// ---- Errors: message and position ----

TEST(ParserTest, ErrorMessagesAndPositions) {
	struct Case {
		std::string_view sql;
		std::string_view fragment;
		std::size_t line;
		std::size_t column;
	};
	const Case cases[] = {
	    {"", "expected a statement", 1, 1},
	    {"   -- nothing", "found end of input", 1, 14},
	    {"SELEC * FROM t", "expected a statement", 1, 1},
	    {"SELECT * FORM t", "expected FROM, found identifier 'form'", 1, 10},
	    {"SELECT * FROM t garbage", "expected end of input, found identifier 'garbage'", 1, 17},
	    {"SELECT * FROM t; SELECT", "expected end of input, found SELECT", 1, 18},
	    {"SELECT * FROM t;;", "expected end of input, found ';'", 1, 17},
	    {"SELECT * FROM order", "expected table name, found ORDER", 1, 15},
	    {"SELECT FROM t", "expected '*' or column name, found FROM", 1, 8},
	    {"SELECT *, a FROM t", "expected FROM, found ','", 1, 9},
	    {"SELECT a, FROM t", "expected column name, found FROM", 1, 11},
	    {"SELECT a. FROM t", "expected column name after '.'", 1, 11},
	    {"SELECT * FROM t WHERE a", "expected comparison operator, found end of input", 1, 24},
	    {"SELECT * FROM t WHERE a = ", "expected column name or literal, found end of input", 1,
	     27},
	    {"SELECT * FROM t WHERE a = 1 AND", "expected column name or literal", 1, 32},
	    {"SELECT * FROM t WHERE a = 1 OR b = 2", "found identifier 'or'", 1, 29},
	    {"SELECT * FROM t WHERE a = b = c", "expected end of input, found '='", 1, 29},
	    {"SELECT * FROM t ORDER a", "expected BY after ORDER", 1, 23},
	    {"SELECT * FROM t ORDER BY", "expected column name, found end of input", 1, 25},
	    {"SELECT * FROM t WHERE a = 1 JOIN u ON a = b", "expected end of input, found JOIN", 1,
	     29},  // clauses out of order
	    {"SELECT * FROM a JOIN b ON x = y JOIN c ON p = q", "expected end of input, found JOIN", 1,
	     33},  // at most one JOIN
	    {"SELECT * FROM a JOIN ON x = y", "expected table name after JOIN, found ON", 1, 22},
	    {"SELECT * FROM a JOIN b x = y", "expected ON, found identifier 'x'", 1, 24},
	    {"INSERT INTO t VALUES (-'x')", "expected integer after '-', found string 'x'", 1, 24},
	    {"INSERT INTO t VALUES (- -1)", "expected integer after '-', found '-'", 1, 25},
	    {"INSERT INTO t VALUES ()", "expected literal (integer or string), found ')'", 1, 23},
	    {"INSERT INTO t VALUES (a)", "expected literal", 1, 23},
	    {"INSERT INTO t VALUES (1", "expected ',' or ')', found end of input", 1, 24},
	    {"INSERT INTO t VALUES", "expected '(', found end of input", 1, 21},
	    {"INSERT INTO t (1)", "expected VALUES", 1, 15},
	    {"INSERT t VALUES (1)", "expected INTO", 1, 8},
	    {"DELETE t", "expected FROM, found identifier 't'", 1, 8},
	    {"CREATE t (a INT64 PRIMARY KEY)", "expected TABLE", 1, 8},
	    {"CREATE TABLE t (a INT64)", "exactly one PRIMARY KEY", 1, 24},
	    {"CREATE TABLE t (a INT64 PRIMARY KEY, b INT64 PRIMARY KEY)", "more than one PRIMARY KEY",
	     1, 46},
	    {"CREATE TABLE t (a TEXT PRIMARY KEY)", "expected column type (INT64 or VARCHAR)", 1, 19},
	    {"CREATE TABLE t (a VARCHAR PRIMARY KEY)", "expected '(' after VARCHAR", 1, 27},
	    {"CREATE TABLE t (a INT64 PRIMARY)", "expected KEY after PRIMARY", 1, 32},
	    {"CREATE TABLE t ()", "expected column name, found ')'", 1, 17},
	    {"CREATE TABLE t (a INT64 PRIMARY KEY,)", "expected column name, found ')'", 1, 37},
	    {"CREATE TABLE key (a INT64 PRIMARY KEY)", "expected table name, found KEY", 1, 14},
	    {"EXPLAIN INSERT INTO t VALUES (1)", "EXPLAIN is only supported before SELECT or DELETE", 1,
	     9},
	    {"EXPLAIN CREATE TABLE t (a INT64 PRIMARY KEY)", "EXPLAIN is only supported", 1, 9},
	    {"EXPLAIN", "expected SELECT or DELETE after EXPLAIN, found end of input", 1, 8},
	    {"EXPLAIN EXPLAIN SELECT * FROM t", "expected SELECT or DELETE after EXPLAIN", 1, 9},
	    // A lexer error comes through unchanged, with its own position.
	    {"SELECT * FROM t\nWHERE a = @", "unexpected character '@'", 2, 11},
	    {"SELECT * FROM t WHERE a = 'open", "unterminated string", 1, 27},
	    // Position on a later line.
	    {"SELECT *\nFROM t\nWHERE a = 1\n  garbage", "found identifier 'garbage'", 4, 3},
	};
	for (const Case& c : cases) {
		ExpectError(c.sql, c.fragment, c.line, c.column);
	}
}

// ---- Every token-boundary prefix of a valid statement ----

/*
 * Cut a valid statement just before each of its tokens. A prefix must either parse (`SELECT *
 * FROM t` is a prefix of `... WHERE ...`), and then round-trip, or fail AT ITS END, on kEnd.
 *
 * Failing anywhere earlier would mean a rule rejected a token sequence that the full statement
 * proves legal. And the explicit list below pins the other direction — prefixes that must NOT
 * parse, catching a rule that accepts a statement cut short.
 */
TEST(ParserTest, EveryPrefixParsesOrFailsAtItsEnd) {
	for (const Golden& g : kGolden) {
		const Result<std::vector<Token>> tokens = Lexer::Tokenize(g.input);
		ASSERT_TRUE(tokens.has_value());
		for (const Token& cut : *tokens) {
			const std::string_view prefix = g.input.substr(0, cut.offset);
			Result<ParsedStatement> parsed = Parser::Parse(prefix);
			if (parsed.has_value()) {
				EXPECT_EQ(MustParse(ToSql(*parsed)), *parsed) << prefix;
				continue;
			}
			ASSERT_EQ(parsed.error().code(), ErrorCode::kInvalidArgument);
			const std::string at_end = FormatSyntaxError(prefix, prefix.size(), "");
			EXPECT_TRUE(std::string(parsed.error().message()).starts_with(at_end))
			    << "prefix: [" << prefix << "]\n  → " << parsed.error().message();
		}
	}
}

TEST(ParserTest, StatementsCutShortAreRejected) {
	for (std::string_view sql : {
	         "SELECT",
	         "SELECT *",
	         "SELECT * FROM",
	         "SELECT a,",
	         "SELECT * FROM t WHERE",
	         "SELECT * FROM t WHERE a =",
	         "SELECT * FROM t WHERE a = 1 AND",
	         "SELECT * FROM t JOIN",
	         "SELECT * FROM t JOIN u",
	         "SELECT * FROM t JOIN u ON",
	         "SELECT * FROM t JOIN u ON a =",
	         "SELECT * FROM t ORDER",
	         "SELECT * FROM t ORDER BY",
	         "SELECT * FROM t ORDER BY a,",
	         "SELECT t. FROM t",
	         "INSERT INTO t VALUES",
	         "INSERT INTO t VALUES (1",
	         "INSERT INTO t VALUES (1,",
	         "INSERT INTO t VALUES (1),",
	         "INSERT INTO t VALUES (-",
	         "CREATE TABLE t",
	         "CREATE TABLE t (",
	         "CREATE TABLE t (a",
	         "CREATE TABLE t (a INT64 PRIMARY KEY",
	         "CREATE TABLE t (a INT64 PRIMARY KEY,",
	         "CREATE TABLE t (a VARCHAR(",
	         "CREATE TABLE t (a VARCHAR(5",
	         "DELETE",
	         "DELETE FROM",
	         "DELETE FROM t WHERE",
	         "EXPLAIN",
	         ";",
	     }) {
		Result<ParsedStatement> parsed = Parser::Parse(sql);
		EXPECT_FALSE(parsed.has_value()) << "accepted a cut-short statement: " << sql;
	}
}

// ---- Random token soup: never crashes; whatever parses, round-trips ----

TEST(ParserTest, RandomTokenSequencesNeverCrash) {
	// Every token's spelling, with names that collide on table/column roles and literals at the
	// integer edges, so the deep paths (JOIN, ORDER BY, PRIMARY KEY counting) get reached.
	const std::vector<std::string_view> kSpellings = {
	    "SELECT",
	    "FROM",
	    "WHERE",
	    "AND",
	    "INSERT",
	    "INTO",
	    "VALUES",
	    "DELETE",
	    "CREATE",
	    "TABLE",
	    "PRIMARY",
	    "KEY",
	    "INT64",
	    "INT",
	    "VARCHAR",
	    "JOIN",
	    "ON",
	    "ORDER",
	    "BY",
	    "ASC",
	    "DESC",
	    "EXPLAIN",
	    "t",
	    "a",
	    "b",
	    "0",
	    "7",
	    "65536",
	    "9223372036854775808",
	    "'s'",
	    "''''",
	    ",",
	    "(",
	    ")",
	    "*",
	    ".",
	    ";",
	    "-",
	    "=",
	    "!=",
	    "<>",
	    "<",
	    "<=",
	    ">",
	    ">=",
	};
	// Start most sequences with a statement keyword, or almost everything dies at token one.
	const std::vector<std::string_view> kStarts = {"SELECT",         "INSERT INTO t VALUES",
	                                               "DELETE FROM t",  "CREATE TABLE t (",
	                                               "EXPLAIN SELECT", ""};
	std::mt19937 rng(20261115);
	std::uniform_int_distribution<std::size_t> pick(0, kSpellings.size() - 1);
	std::uniform_int_distribution<std::size_t> pick_start(0, kStarts.size() - 1);
	std::uniform_int_distribution<std::size_t> length(0, 16);

	int parsed_ok = 0;
	for (int i = 0; i < 10'000; ++i) {
		std::string sql(kStarts[pick_start(rng)]);
		for (std::size_t n = length(rng); n > 0; --n) {
			sql += ' ';
			sql += kSpellings[pick(rng)];
		}
		Result<ParsedStatement> parsed = Parser::Parse(sql);
		if (!parsed.has_value()) {
			ASSERT_EQ(parsed.error().code(), ErrorCode::kInvalidArgument) << sql;
			ASSERT_TRUE(std::string(parsed.error().message()).starts_with("syntax error at line "))
			    << sql;
			continue;
		}
		++parsed_ok;
		Result<ParsedStatement> again = Parser::Parse(ToSql(*parsed));
		ASSERT_TRUE(again.has_value()) << sql << "\n  canonical: " << ToSql(*parsed);
		ASSERT_EQ(*again, *parsed) << sql;
	}
	// Sanity: the generator actually reaches the accepting paths sometimes.
	EXPECT_GT(parsed_ok, 0);
}

// ---- ToSql pieces used by EXPLAIN ----

TEST(ToSqlTest, Pieces) {
	EXPECT_EQ(ToSql(ColumnRef{"", "a"}), "a");
	EXPECT_EQ(ToSql(ColumnRef{"t", "a"}), "t.a");
	EXPECT_EQ(ToSql(Value{int64_t{-7}}), "-7");
	EXPECT_EQ(ToSql(Value{std::numeric_limits<int64_t>::min()}), "-9223372036854775808");
	EXPECT_EQ(ToSql(Value{std::string("it's")}), "'it''s'");
	EXPECT_EQ(ToSql(Value{std::string("")}), "''");
	EXPECT_EQ(ToSql(CompareOp::kNotEqual), "!=");
	EXPECT_EQ(ToSql(Comparison{ColumnRef{"t", "id"}, CompareOp::kLessEqual, Value{int64_t{5}}}),
	          "t.id <= 5");
}

}  // namespace kernsql

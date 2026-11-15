#include "sql/parser/parser.hpp"

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace kernsql;

/*
 * Recursive descent, read top-down: ParseStatement at the top calls one Parse* per grammar rule
 * (DD-007, "Grammar"), each of which calls the rules on its right-hand side. Every method follows
 * the same three moves:
 *
 *   - a token the rule REQUIRES      → Expect(type, "what to call it in the error")
 *   - a token the rule MAY have      → Match(type), and branch on the bool
 *   - a choice between alternatives  → switch on Peek().type, the single token of lookahead
 *
 * and returns as soon as anything it called fails: the first error wins, nothing is recovered.
 */

namespace {

// How the offending token reads in "found ...": names and literals show their text, so the user
// sees `identifier 'form'`, not just `identifier`.
std::string Describe(const Token& token) {
	switch (token.type) {
		case TokenType::kIdentifier:
			return std::format("identifier '{}'", token.text);
		case TokenType::kInteger:
			return std::format("integer {}", token.text);
		case TokenType::kString:
			return std::format("string '{}'", token.text);
		default:
			return std::string(TokenTypeName(token.type));
	}
}

}  // namespace

// ---- Entry point ----

Result<ParsedStatement> Parser::Parse(std::string_view sql) {
	Result<std::vector<Token>> tokens = Lexer::Tokenize(sql);
	if (!tokens.has_value()) {
		return std::unexpected(std::move(tokens.error()));  // lexer errors pass through as-is
	}
	Parser parser(sql, std::move(*tokens));
	return parser.ParseStatement();
}

// ---- Token helpers ----

const Token& Parser::Peek() const {
	assert(pos_ < tokens_.size());
	return tokens_[pos_];
}

const Token& Parser::Advance() {
	const Token& token = Peek();
	// Parking on kEnd means Peek() is always safe: every rule that runs off the end of the input
	// just sees kEnd again and fails on it.
	if (token.type != TokenType::kEnd) {
		++pos_;
	}
	return token;
}

bool Parser::Match(TokenType type) {
	if (Peek().type != type) {
		return false;
	}
	Advance();
	return true;
}

Result<Token> Parser::Expect(TokenType type, std::string_view what) {
	if (Peek().type != type) {
		return std::unexpected(
		    ErrorAt(Peek(), std::format("expected {}, found {}", what, Describe(Peek()))));
	}
	return Advance();
}

Status Parser::ErrorAt(const Token& token, std::string_view message) const {
	return Status::InvalidArgument(FormatSyntaxError(sql_, token.offset, message));
}

// ---- statement := [EXPLAIN] (create | insert | select | delete) [';'] END ----

Result<ParsedStatement> Parser::ParseStatement() {
	ParsedStatement statement;
	statement.explain = Match(TokenType::kExplain);

	const Token& first = Peek();
	if (statement.explain && first.type != TokenType::kSelect && first.type != TokenType::kDelete) {
		// Checked before dispatch so the error names EXPLAIN's misuse, not some later token.
		if (first.type == TokenType::kCreate || first.type == TokenType::kInsert) {
			return std::unexpected(
			    ErrorAt(first, std::format("EXPLAIN is only supported before SELECT or DELETE, "
			                               "found {}",
			                               Describe(first))));
		}
		return std::unexpected(ErrorAt(
		    first,
		    std::format("expected SELECT or DELETE after EXPLAIN, found {}", Describe(first))));
	}

	// One token of lookahead picks the statement.
	switch (first.type) {
		case TokenType::kCreate: {
			Result<CreateTableStmt> create = ParseCreateTable();
			if (!create.has_value()) return std::unexpected(std::move(create.error()));
			statement.body = std::move(*create);
			break;
		}
		case TokenType::kInsert: {
			Result<InsertStmt> insert = ParseInsert();
			if (!insert.has_value()) return std::unexpected(std::move(insert.error()));
			statement.body = std::move(*insert);
			break;
		}
		case TokenType::kSelect: {
			Result<SelectStmt> select = ParseSelect();
			if (!select.has_value()) return std::unexpected(std::move(select.error()));
			statement.body = std::move(*select);
			break;
		}
		case TokenType::kDelete: {
			Result<DeleteStmt> del = ParseDelete();
			if (!del.has_value()) return std::unexpected(std::move(del.error()));
			statement.body = std::move(*del);
			break;
		}
		default:
			return std::unexpected(ErrorAt(
			    first, std::format("expected a statement (CREATE, INSERT, SELECT, DELETE, or "
			                       "EXPLAIN), found {}",
			                       Describe(first))));
	}

	Match(TokenType::kSemicolon);
	// Without this, `SELECT * FROM t garbage` would parse and silently drop `garbage`.
	if (Result<Token> end = Expect(TokenType::kEnd, "end of input"); !end.has_value()) {
		return std::unexpected(std::move(end.error()));
	}
	return statement;
}

// ---- create := CREATE TABLE ident '(' coldef { ',' coldef } ')' ----

Result<CreateTableStmt> Parser::ParseCreateTable() {
	if (Result<Token> t = Expect(TokenType::kCreate, "CREATE"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	if (Result<Token> t = Expect(TokenType::kTable, "TABLE"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	Result<Token> name = Expect(TokenType::kIdentifier, "table name");
	if (!name.has_value()) return std::unexpected(std::move(name.error()));
	if (Result<Token> t = Expect(TokenType::kLeftParen, "'('"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}

	CreateTableStmt create;
	create.table = std::move(name->text);
	bool have_primary_key = false;
	do {
		Result<ColumnDef> def = ParseColumnDef();
		if (!def.has_value()) return std::unexpected(std::move(def.error()));

		if (def->primary_key) {
			// Counting clauses is form, not meaning, so it is the parser's to check (DD-007).
			if (have_primary_key) {
				return std::unexpected(
				    ErrorAt(*def->primary_key_token, "more than one PRIMARY KEY column"));
			}
			if (create.columns.size() > std::numeric_limits<uint16_t>::max()) {
				// Its index would not fit primary_key. Schema::Create caps columns far lower; this
				// only stops a silent truncation before it gets there.
				return std::unexpected(ErrorAt(*def->primary_key_token, "too many columns"));
			}
			have_primary_key = true;
			create.primary_key = static_cast<uint16_t>(create.columns.size());
		}
		create.columns.push_back(std::move(def->column));
	} while (Match(TokenType::kComma));

	Result<Token> close = Expect(TokenType::kRightParen, "',' or ')'");
	if (!close.has_value()) return std::unexpected(std::move(close.error()));
	if (!have_primary_key) {
		// Reported at ')': the point where "there is none" first becomes known.
		return std::unexpected(
		    ErrorAt(*close, "CREATE TABLE needs exactly one PRIMARY KEY column"));
	}
	return create;
}

// ---- coldef := ident type [PRIMARY KEY]    type := INT64 | VARCHAR '(' integer ')' ----

Result<Parser::ColumnDef> Parser::ParseColumnDef() {
	Result<Token> name = Expect(TokenType::kIdentifier, "column name");
	if (!name.has_value()) return std::unexpected(std::move(name.error()));

	ColumnDef def;
	def.column.name = std::move(name->text);

	if (Match(TokenType::kInt64)) {
		def.column.type = ColumnType::kInt64;
		def.column.max_length = 0;
	} else if (Match(TokenType::kVarchar)) {
		if (Result<Token> t = Expect(TokenType::kLeftParen, "'(' after VARCHAR"); !t.has_value()) {
			return std::unexpected(std::move(t.error()));
		}
		Result<Token> digits = Expect(TokenType::kInteger, "VARCHAR length");
		if (!digits.has_value()) return std::unexpected(std::move(digits.error()));
		Result<uint16_t> length = ToUint16(*digits);
		if (!length.has_value()) return std::unexpected(std::move(length.error()));
		if (Result<Token> t = Expect(TokenType::kRightParen, "')'"); !t.has_value()) {
			return std::unexpected(std::move(t.error()));
		}
		def.column.type = ColumnType::kVarchar;
		def.column.max_length = *length;  // 1 .. MAX_VARCHAR_LENGTH is Schema::Create's rule
	} else {
		return std::unexpected(ErrorAt(
		    Peek(),
		    std::format("expected column type (INT64 or VARCHAR), found {}", Describe(Peek()))));
	}

	if (Peek().type == TokenType::kPrimary) {
		def.primary_key_token = &Peek();  // into tokens_, which never changes
		Advance();
		if (Result<Token> t = Expect(TokenType::kKey, "KEY after PRIMARY"); !t.has_value()) {
			return std::unexpected(std::move(t.error()));
		}
		def.primary_key = true;
	}
	return def;
}

// ---- insert := INSERT INTO ident VALUES row { ',' row } ----

Result<InsertStmt> Parser::ParseInsert() {
	if (Result<Token> t = Expect(TokenType::kInsert, "INSERT"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	if (Result<Token> t = Expect(TokenType::kInto, "INTO"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	Result<Token> name = Expect(TokenType::kIdentifier, "table name");
	if (!name.has_value()) return std::unexpected(std::move(name.error()));
	if (Result<Token> t = Expect(TokenType::kValues, "VALUES"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}

	InsertStmt insert;
	insert.table = std::move(name->text);
	do {
		Result<Row> row = ParseRow();
		if (!row.has_value()) return std::unexpected(std::move(row.error()));
		insert.rows.push_back(std::move(*row));
	} while (Match(TokenType::kComma));
	return insert;
}

// ---- row := '(' literal { ',' literal } ')' ----

Result<Row> Parser::ParseRow() {
	if (Result<Token> t = Expect(TokenType::kLeftParen, "'('"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	Row row;
	do {
		Result<Value> value = ParseLiteral();
		if (!value.has_value()) return std::unexpected(std::move(value.error()));
		row.push_back(std::move(*value));
	} while (Match(TokenType::kComma));
	if (Result<Token> t = Expect(TokenType::kRightParen, "',' or ')'"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	return row;
}

// ---- select := SELECT projection FROM ident [join] [where] [orderby] ----

Result<SelectStmt> Parser::ParseSelect() {
	if (Result<Token> t = Expect(TokenType::kSelect, "SELECT"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}

	SelectStmt select;
	Result<std::vector<ColumnRef>> columns = ParseProjection();
	if (!columns.has_value()) return std::unexpected(std::move(columns.error()));
	select.columns = std::move(*columns);

	if (Result<Token> t = Expect(TokenType::kFrom, "FROM"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	Result<Token> name = Expect(TokenType::kIdentifier, "table name");
	if (!name.has_value()) return std::unexpected(std::move(name.error()));
	select.table = std::move(name->text);

	// The optional clauses, in the only order the grammar allows. A clause out of order (WHERE
	// before JOIN) falls through to ParseStatement's Expect(kEnd) and is reported there.
	if (Match(TokenType::kJoin)) {
		Result<JoinClause> join = ParseJoin();
		if (!join.has_value()) return std::unexpected(std::move(join.error()));
		select.join = std::move(*join);
	}
	if (Match(TokenType::kWhere)) {
		Result<std::vector<Comparison>> where = ParseWhere();
		if (!where.has_value()) return std::unexpected(std::move(where.error()));
		select.where = std::move(*where);
	}
	if (Match(TokenType::kOrder)) {
		Result<std::vector<SortKey>> order_by = ParseOrderBy();
		if (!order_by.has_value()) return std::unexpected(std::move(order_by.error()));
		select.order_by = std::move(*order_by);
	}
	return select;
}

// ---- projection := '*' | colref { ',' colref } ----

Result<std::vector<ColumnRef>> Parser::ParseProjection() {
	if (Match(TokenType::kStar)) {
		return std::vector<ColumnRef>{};  // empty means '*'
	}
	if (Peek().type != TokenType::kIdentifier) {
		// Caught here rather than in ParseColumnRef so the message offers both alternatives.
		return std::unexpected(ErrorAt(
		    Peek(), std::format("expected '*' or column name, found {}", Describe(Peek()))));
	}
	std::vector<ColumnRef> columns;
	do {
		Result<ColumnRef> column = ParseColumnRef();
		if (!column.has_value()) return std::unexpected(std::move(column.error()));
		columns.push_back(std::move(*column));
	} while (Match(TokenType::kComma));
	return columns;
}

// ---- join := JOIN ident ON comparison    (JOIN already matched) ----

Result<JoinClause> Parser::ParseJoin() {
	Result<Token> name = Expect(TokenType::kIdentifier, "table name after JOIN");
	if (!name.has_value()) return std::unexpected(std::move(name.error()));
	if (Result<Token> t = Expect(TokenType::kOn, "ON"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	Result<Comparison> on = ParseComparison();
	if (!on.has_value()) return std::unexpected(std::move(on.error()));
	return JoinClause{.table = std::move(name->text), .on = std::move(*on)};
}

// ---- where := WHERE comparison { AND comparison }    (WHERE already matched) ----

Result<std::vector<Comparison>> Parser::ParseWhere() {
	std::vector<Comparison> where;
	do {
		Result<Comparison> comparison = ParseComparison();
		if (!comparison.has_value()) return std::unexpected(std::move(comparison.error()));
		where.push_back(std::move(*comparison));
	} while (Match(TokenType::kAnd));
	return where;
}

// ---- orderby := ORDER BY sortkey { ',' sortkey }    (ORDER already matched) ----

Result<std::vector<SortKey>> Parser::ParseOrderBy() {
	if (Result<Token> t = Expect(TokenType::kBy, "BY after ORDER"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	std::vector<SortKey> keys;
	do {
		Result<ColumnRef> column = ParseColumnRef();
		if (!column.has_value()) return std::unexpected(std::move(column.error()));
		SortKey key{.column = std::move(*column)};
		if (Match(TokenType::kDesc)) {
			key.descending = true;
		} else {
			Match(TokenType::kAsc);  // the default, spelled out or not
		}
		keys.push_back(std::move(key));
	} while (Match(TokenType::kComma));
	return keys;
}

// ---- delete := DELETE FROM ident [where] ----

Result<DeleteStmt> Parser::ParseDelete() {
	if (Result<Token> t = Expect(TokenType::kDelete, "DELETE"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	if (Result<Token> t = Expect(TokenType::kFrom, "FROM"); !t.has_value()) {
		return std::unexpected(std::move(t.error()));
	}
	Result<Token> name = Expect(TokenType::kIdentifier, "table name");
	if (!name.has_value()) return std::unexpected(std::move(name.error()));

	DeleteStmt del;
	del.table = std::move(name->text);
	if (Match(TokenType::kWhere)) {
		Result<std::vector<Comparison>> where = ParseWhere();
		if (!where.has_value()) return std::unexpected(std::move(where.error()));
		del.where = std::move(*where);
	}
	return del;
}

// ---- comparison := operand op operand ----

Result<Comparison> Parser::ParseComparison() {
	Result<Operand> lhs = ParseOperand();
	if (!lhs.has_value()) return std::unexpected(std::move(lhs.error()));
	Result<CompareOp> op = ParseCompareOp();
	if (!op.has_value()) return std::unexpected(std::move(op.error()));
	Result<Operand> rhs = ParseOperand();
	if (!rhs.has_value()) return std::unexpected(std::move(rhs.error()));
	return Comparison{.lhs = std::move(*lhs), .op = *op, .rhs = std::move(*rhs)};
}

// ---- operand := colref | literal ----

Result<Operand> Parser::ParseOperand() {
	switch (Peek().type) {
		case TokenType::kIdentifier: {
			Result<ColumnRef> column = ParseColumnRef();
			if (!column.has_value()) return std::unexpected(std::move(column.error()));
			return Operand{std::move(*column)};
		}
		case TokenType::kInteger:
		case TokenType::kMinus:
		case TokenType::kString: {
			Result<Value> value = ParseLiteral();
			if (!value.has_value()) return std::unexpected(std::move(value.error()));
			return Operand{std::move(*value)};
		}
		default:
			return std::unexpected(ErrorAt(
			    Peek(),
			    std::format("expected column name or literal, found {}", Describe(Peek()))));
	}
}

// ---- op := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>=' ----

Result<CompareOp> Parser::ParseCompareOp() {
	CompareOp op{};
	switch (Peek().type) {
		case TokenType::kEqual:
			op = CompareOp::kEqual;
			break;
		case TokenType::kNotEqual:
			op = CompareOp::kNotEqual;
			break;
		case TokenType::kLess:
			op = CompareOp::kLess;
			break;
		case TokenType::kLessEqual:
			op = CompareOp::kLessEqual;
			break;
		case TokenType::kGreater:
			op = CompareOp::kGreater;
			break;
		case TokenType::kGreaterEqual:
			op = CompareOp::kGreaterEqual;
			break;
		default:
			return std::unexpected(ErrorAt(
			    Peek(), std::format("expected comparison operator, found {}", Describe(Peek()))));
	}
	Advance();
	return op;
}

// ---- colref := ident [ '.' ident ] ----

Result<ColumnRef> Parser::ParseColumnRef() {
	Result<Token> first = Expect(TokenType::kIdentifier, "column name");
	if (!first.has_value()) return std::unexpected(std::move(first.error()));
	if (!Match(TokenType::kDot)) {
		return ColumnRef{.table = {}, .column = std::move(first->text)};
	}
	// `a.b`: the first name was the table.
	Result<Token> second = Expect(TokenType::kIdentifier, "column name after '.'");
	if (!second.has_value()) return std::unexpected(std::move(second.error()));
	return ColumnRef{.table = std::move(first->text), .column = std::move(second->text)};
}

// ---- literal := ['-'] integer | string ----

Result<Value> Parser::ParseLiteral() {
	if (Peek().type == TokenType::kString) {
		return Value{Advance().text};
	}
	const bool negative = Match(TokenType::kMinus);
	if (Peek().type != TokenType::kInteger) {
		const std::string_view what =
		    negative ? "integer after '-'" : "literal (integer or string)";
		return std::unexpected(
		    ErrorAt(Peek(), std::format("expected {}, found {}", what, Describe(Peek()))));
	}
	Result<int64_t> value = ToInt64(Advance(), negative);
	if (!value.has_value()) return std::unexpected(std::move(value.error()));
	return Value{*value};
}

// ---- Integers ----

Result<int64_t> Parser::ToInt64(const Token& digits, bool negative) const {
	assert(digits.type == TokenType::kInteger && !digits.text.empty());

	// The magnitude first, as unsigned. 2^63 fits a uint64_t but not an int64_t, and it is
	// exactly the magnitude of INT64_MIN: negating a parsed int64_t could never produce it.
	uint64_t magnitude = 0;
	const char* begin = digits.text.data();
	const char* end = begin + digits.text.size();
	const auto [ptr, ec] = std::from_chars(begin, end, magnitude);
	// The lexer only makes digit runs, so the only possible failure is overflow.
	assert(ec == std::errc{} || ec == std::errc::result_out_of_range);
	assert(ec != std::errc{} || ptr == end);

	constexpr auto kMax = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());  // 2^63 - 1
	const uint64_t limit = negative ? kMax + 1 : kMax;
	if (ec == std::errc::result_out_of_range || magnitude > limit) {
		return std::unexpected(ErrorAt(digits, std::format("integer {}{} out of range for INT64",
		                                                   negative ? "-" : "", digits.text)));
	}

	if (!negative) {
		return static_cast<int64_t>(magnitude);
	}
	if (magnitude == kMax + 1) {
		return std::numeric_limits<int64_t>::min();
	}
	return -static_cast<int64_t>(magnitude);  // magnitude <= 2^63 - 1 here, so this cannot overflow
}

Result<uint16_t> Parser::ToUint16(const Token& digits) const {
	assert(digits.type == TokenType::kInteger && !digits.text.empty());
	uint16_t value = 0;
	const auto [ptr, ec] =
	    std::from_chars(digits.text.data(), digits.text.data() + digits.text.size(), value);
	assert(ec == std::errc{} || ec == std::errc::result_out_of_range);
	if (ec == std::errc::result_out_of_range) {
		return std::unexpected(
		    ErrorAt(digits, std::format("VARCHAR length {} out of range (at most {})", digits.text,
		                                std::numeric_limits<uint16_t>::max())));
	}
	assert(ptr == digits.text.data() + digits.text.size());
	return value;
}

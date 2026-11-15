#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "common/status.hpp"
#include "sql/parser/ast.hpp"
#include "sql/parser/token.hpp"

namespace kernsql {

/*
 * Tokens → AST by recursive descent: one Parse* method per grammar rule in DD-007, each
 * consuming exactly its rule's tokens and returning its rule's piece of the tree.
 *
 * The grammar is LL(1): every choice is made by Peek() alone, so nothing ever backtracks. No rule
 * refers back to itself, so the call depth is fixed by the grammar, not by the input.
 *
 * Errors: the first one wins, returned as kInvalidArgument with a FormatSyntaxError message
 * pointing at the offending token. Every Parse* returns as soon as a callee fails.
 */
class Parser {
  public:
	/*
	 * Parse exactly one statement, optionally followed by ';', then end of input. Lexer errors
	 * are returned unchanged.
	 *
	 * Checks form only (DD-007, "What the parser checks"): beyond the grammar, integers must fit
	 * their type, a CREATE TABLE must have exactly one PRIMARY KEY, and EXPLAIN may only precede
	 * SELECT or DELETE.
	 */
	[[nodiscard]] static Result<ParsedStatement> Parse(std::string_view sql);

  private:
	Parser(std::string_view sql, std::vector<Token> tokens)
	    : sql_(sql), tokens_(std::move(tokens)) {}

	// ---- Token helpers. The four everything else is built from. ----

	// The current token, not consumed. Never past kEnd: the stream always ends with one.
	[[nodiscard]] const Token& Peek() const;

	// Consume and return the current token. Never moves past kEnd.
	const Token& Advance();

	// Consume the current token if it is `type`.
	bool Match(TokenType type);

	// Consume the current token if it is `type`, else fail with
	// "expected <what>, found <TokenTypeName (and text, for names and literals)>".
	[[nodiscard]] Result<Token> Expect(TokenType type, std::string_view what);

	[[nodiscard]] Status ErrorAt(const Token& token, std::string_view message) const;

	// ---- One method per grammar rule (DD-007, "Grammar"). ----

	// statement := [EXPLAIN] (create | insert | select | delete) [';'] END
	[[nodiscard]] Result<ParsedStatement> ParseStatement();

	// create := CREATE TABLE ident '(' coldef { ',' coldef } ')'
	// Fails on a second PRIMARY KEY (at that clause) or none (at the closing ')').
	[[nodiscard]] Result<CreateTableStmt> ParseCreateTable();

	// coldef := ident type [PRIMARY KEY]    type := INT64 | VARCHAR '(' integer ')'
	struct ColumnDef {
		Column column;
		bool primary_key{false};
		// For the "second PRIMARY KEY" error position. Points into tokens_, which never changes
		// after construction.
		const Token* primary_key_token{nullptr};
	};
	[[nodiscard]] Result<ColumnDef> ParseColumnDef();

	// insert := INSERT INTO ident VALUES row { ',' row }
	[[nodiscard]] Result<InsertStmt> ParseInsert();

	// row := '(' literal { ',' literal } ')'
	[[nodiscard]] Result<Row> ParseRow();

	// select := SELECT projection FROM ident [join] [where] [orderby]
	[[nodiscard]] Result<SelectStmt> ParseSelect();

	// projection := '*' | colref { ',' colref }      (empty result means '*')
	[[nodiscard]] Result<std::vector<ColumnRef>> ParseProjection();

	// join := JOIN ident ON comparison    (caller has matched JOIN)
	[[nodiscard]] Result<JoinClause> ParseJoin();

	// where := WHERE comparison { AND comparison }    (caller has matched WHERE)
	[[nodiscard]] Result<std::vector<Comparison>> ParseWhere();

	// orderby := ORDER BY sortkey { ',' sortkey }    sortkey := colref [ASC | DESC]
	// (caller has matched ORDER)
	[[nodiscard]] Result<std::vector<SortKey>> ParseOrderBy();

	// delete := DELETE FROM ident [where]
	[[nodiscard]] Result<DeleteStmt> ParseDelete();

	// comparison := operand op operand
	[[nodiscard]] Result<Comparison> ParseComparison();

	// operand := colref | literal    (decided by Peek(): identifier → colref)
	[[nodiscard]] Result<Operand> ParseOperand();

	// op := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>='
	[[nodiscard]] Result<CompareOp> ParseCompareOp();

	// colref := ident [ '.' ident ]
	[[nodiscard]] Result<ColumnRef> ParseColumnRef();

	// literal := ['-'] integer | string
	[[nodiscard]] Result<Value> ParseLiteral();

	// ---- Integers (DD-007, "Literals and the INT64_MIN trap"). ----

	// The kInteger `digits` as a signed value: magnitude into uint64_t first, then the sign.
	// Fails if the result does not fit int64_t; exactly 2^63 with `negative` is INT64_MIN.
	[[nodiscard]] Result<int64_t> ToInt64(const Token& digits, bool negative) const;

	// VARCHAR(n)'s n: must fit uint16_t. Its range is Schema::Create's to check.
	[[nodiscard]] Result<uint16_t> ToUint16(const Token& digits) const;

	std::string_view sql_;  // for error positions only
	std::vector<Token> tokens_;
	std::size_t pos_{0};
};

}  // namespace kernsql

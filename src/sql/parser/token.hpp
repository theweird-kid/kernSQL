#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.hpp"

namespace kernsql {

/*
 * Every kind of token the lexer produces (DD-007, "Lexer rules").
 *
 * Keywords are reserved: a word in the keyword table is never an identifier. `INT` and `INT64`
 * both lex to kInt64; `!=` and `<>` both lex to kNotEqual.
 */
enum class TokenType : uint8_t {
	// Keywords.
	kSelect,
	kFrom,
	kWhere,
	kAnd,
	kInsert,
	kInto,
	kValues,
	kDelete,
	kCreate,
	kTable,
	kPrimary,
	kKey,
	kInt64,
	kVarchar,
	kJoin,
	kOn,
	kOrder,
	kBy,
	kAsc,
	kDesc,
	kExplain,

	// Names and literals — the only tokens whose Token::text means anything.
	kIdentifier,  // text: the word, lowercased
	kInteger,     // text: the digits, unsigned; the sign is a separate kMinus
	kString,      // text: the content, quotes removed and '' unescaped

	// Punctuation and operators.
	kComma,
	kLeftParen,
	kRightParen,
	kStar,
	kDot,
	kSemicolon,
	kMinus,
	kEqual,         // =
	kNotEqual,      // != or <>
	kLess,          // <
	kLessEqual,     // <=
	kGreater,       // >
	kGreaterEqual,  // >=

	// Always the last token, exactly once, at offset sql.size().
	kEnd,
};

/*
 * How a token type is named in an error message: the keyword itself for keywords ("FROM"), the
 * symbol in quotes for punctuation ("','"), and a description for the rest ("identifier",
 * "integer", "string", "end of input").
 */
[[nodiscard]] std::string_view TokenTypeName(TokenType type);

struct Token {
	TokenType type{TokenType::kEnd};

	// Meaningful for kIdentifier, kInteger and kString only (see TokenType); empty otherwise.
	std::string text;

	// Where the token starts and how many input bytes it spans, quotes included. Error messages
	// use offset for the position.
	std::size_t offset{0};
	std::size_t length{0};

	bool operator==(const Token&) const = default;
};

/*
 * "syntax error at line L, column C: <message>". L and C are 1-based, C counts bytes, both
 * computed from `offset` into `sql`. offset == sql.size() (the kEnd token) is legal and points
 * one past the last byte.
 *
 * Shared by the lexer and the parser, so every error from either has the same shape.
 */
[[nodiscard]] std::string FormatSyntaxError(std::string_view sql, std::size_t offset,
                                            std::string_view message);

/*
 * Characters → tokens, in one pass, before the parser starts (DD-007, "Pipeline").
 *
 * A cursor over the input: skip whitespace and comments, look at one character, and that
 * character decides which Lex* reads the rest of the token.
 */
class Lexer {
  public:
	/*
	 * Tokenize the whole input. On success the vector ends with exactly one kEnd.
	 *
	 * kInvalidArgument (FormatSyntaxError message) on: a byte that starts no token, including
	 * every non-ASCII byte outside a string and a lone `!`; an unterminated string (reported at
	 * the opening quote); digits directly followed by a letter or `_`.
	 */
	[[nodiscard]] static Result<std::vector<Token>> Tokenize(std::string_view sql);

  private:
	explicit Lexer(std::string_view sql) : sql_(sql) {}

	[[nodiscard]] Result<std::vector<Token>> Run();

	// Whitespace, and `--` comments to end of line (or end of input).
	void SkipWhitespaceAndComments();

	// [A-Za-z_][A-Za-z0-9_]*, longest match, lowercased, then the keyword table. Cannot fail.
	[[nodiscard]] Token LexWord();

	// Decimal digits, kept as text. Fails if a letter or `_` follows directly.
	[[nodiscard]] Result<Token> LexNumber();

	// '...' with '' as an escaped quote. Fails at end of input inside the string.
	[[nodiscard]] Result<Token> LexString();

	// Punctuation and operators, longest match first. Fails on anything else.
	[[nodiscard]] Result<Token> LexSymbol();

	// A token of `type` spanning [start, pos_).
	[[nodiscard]] Token MakeToken(TokenType type, std::size_t start, std::string text = {}) const;

	[[nodiscard]] Status ErrorAt(std::size_t offset, std::string_view message) const;

	std::string_view sql_;
	std::size_t pos_{0};
};

}  // namespace kernsql

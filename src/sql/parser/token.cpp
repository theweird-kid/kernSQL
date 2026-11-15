#include "sql/parser/token.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace kernsql;

namespace {

// ASCII-only character classes. Deliberately not <cctype>: std::isalpha and friends depend on the
// C locale, and are undefined behaviour for a negative char (every non-ASCII byte, when char is
// signed). The lexer must classify bytes the same way on every machine.
constexpr bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}
constexpr bool IsWordStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
constexpr bool IsWordChar(char c) {
	return IsWordStart(c) || IsDigit(c);
}
constexpr bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// 'A'..'Z' → 'a'..'z'; everything else unchanged.
constexpr char ToLower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

struct Keyword {
	std::string_view spelling;  // lowercase
	TokenType type;
};

// Every reserved word (DD-007, "Lexer rules"). INT is a synonym of INT64.
constexpr std::array kKeywords{
    Keyword{"select", TokenType::kSelect},   Keyword{"from", TokenType::kFrom},
    Keyword{"where", TokenType::kWhere},     Keyword{"and", TokenType::kAnd},
    Keyword{"insert", TokenType::kInsert},   Keyword{"into", TokenType::kInto},
    Keyword{"values", TokenType::kValues},   Keyword{"delete", TokenType::kDelete},
    Keyword{"create", TokenType::kCreate},   Keyword{"table", TokenType::kTable},
    Keyword{"primary", TokenType::kPrimary}, Keyword{"key", TokenType::kKey},
    Keyword{"int64", TokenType::kInt64},     Keyword{"int", TokenType::kInt64},
    Keyword{"varchar", TokenType::kVarchar}, Keyword{"join", TokenType::kJoin},
    Keyword{"on", TokenType::kOn},           Keyword{"order", TokenType::kOrder},
    Keyword{"by", TokenType::kBy},           Keyword{"asc", TokenType::kAsc},
    Keyword{"desc", TokenType::kDesc},       Keyword{"explain", TokenType::kExplain},
};

// `word` must already be lowercase. nullopt → it is an identifier. A linear scan: 22 entries,
// called once per word of a short statement; a hash map would be slower to build than to skip.
std::optional<TokenType> LookupKeyword(std::string_view word) {
	for (const Keyword& keyword : kKeywords) {
		if (keyword.spelling == word) {
			return keyword.type;
		}
	}
	return std::nullopt;
}

}  // namespace

std::string_view kernsql::TokenTypeName(TokenType type) {
	// No `default:` — -Wswitch then flags any TokenType added later and not named here.
	switch (type) {
		// Keywords.
		case TokenType::kSelect:
			return "SELECT";
		case TokenType::kFrom:
			return "FROM";
		case TokenType::kWhere:
			return "WHERE";
		case TokenType::kAnd:
			return "AND";
		case TokenType::kInsert:
			return "INSERT";
		case TokenType::kInto:
			return "INTO";
		case TokenType::kValues:
			return "VALUES";
		case TokenType::kDelete:
			return "DELETE";
		case TokenType::kCreate:
			return "CREATE";
		case TokenType::kTable:
			return "TABLE";
		case TokenType::kPrimary:
			return "PRIMARY";
		case TokenType::kKey:
			return "KEY";
		case TokenType::kInt64:
			return "INT64";
		case TokenType::kVarchar:
			return "VARCHAR";
		case TokenType::kJoin:
			return "JOIN";
		case TokenType::kOn:
			return "ON";
		case TokenType::kOrder:
			return "ORDER";
		case TokenType::kBy:
			return "BY";
		case TokenType::kAsc:
			return "ASC";
		case TokenType::kDesc:
			return "DESC";
		case TokenType::kExplain:
			return "EXPLAIN";

		// Names and literals.
		case TokenType::kIdentifier:
			return "identifier";
		case TokenType::kInteger:
			return "integer";
		case TokenType::kString:
			return "string";

		// Punctuation and operators.
		case TokenType::kComma:
			return "','";
		case TokenType::kLeftParen:
			return "'('";
		case TokenType::kRightParen:
			return "')'";
		case TokenType::kStar:
			return "'*'";
		case TokenType::kDot:
			return "'.'";
		case TokenType::kSemicolon:
			return "';'";
		case TokenType::kMinus:
			return "'-'";
		case TokenType::kEqual:
			return "'='";
		case TokenType::kNotEqual:
			return "'!='";
		case TokenType::kLess:
			return "'<'";
		case TokenType::kLessEqual:
			return "'<='";
		case TokenType::kGreater:
			return "'>'";
		case TokenType::kGreaterEqual:
			return "'>='";

		case TokenType::kEnd:
			return "end of input";
	}
	std::unreachable();
}

std::string kernsql::FormatSyntaxError(std::string_view sql, std::size_t offset,
                                       std::string_view message) {
	// offset == sql.size() is legal: kEnd sits there.
	assert(offset <= sql.size());

	// Only bytes before offset count; a newline after the error is irrelevant.
	std::size_t line{1};
	std::size_t line_start{0};  // index of the first byte of offset's line
	for (std::size_t i = 0; i < offset; ++i) {
		if (sql[i] == '\n') {
			++line;
			line_start = i + 1;
		}
	}
	const std::size_t column = offset - line_start + 1;

	return std::format("syntax error at line {}, column {}: {}", line, column, message);
}

// ---- Lexer helpers. Run() and the four Lex* routines are below them. ----

Result<std::vector<Token>> Lexer::Tokenize(std::string_view sql) {
	return Lexer(sql).Run();
}

void Lexer::SkipWhitespaceAndComments() {
	while (pos_ < sql_.size()) {
		if (IsSpace(sql_[pos_])) {
			++pos_;
		} else if (sql_.substr(pos_).starts_with("--")) {
			// To the newline, which the next iteration skips as whitespace, or to end of input:
			// a comment on the last line has no newline after it.
			const std::size_t newline = sql_.find('\n', pos_);
			pos_ = (newline == std::string_view::npos) ? sql_.size() : newline;
		} else {
			return;
		}
	}
}

Token Lexer::MakeToken(TokenType type, std::size_t start, std::string text) const {
	assert(start <= pos_);
	return Token{.type = type, .text = std::move(text), .offset = start, .length = pos_ - start};
}

Status Lexer::ErrorAt(std::size_t offset, std::string_view message) const {
	return Status::InvalidArgument(FormatSyntaxError(sql_, offset, message));
}

Result<std::vector<Token>> Lexer::Run() {
	std::vector<Token> res;
	while (true) {
		SkipWhitespaceAndComments();
		if (pos_ >= sql_.size()) {
			// The only successful exit, so kEnd appears exactly once, at sql_.size(), length 0.
			res.push_back(MakeToken(TokenType::kEnd, pos_));
			return res;
		}

		// One character decides which routine reads the rest of the token.
		const char curr = sql_[pos_];
		Result<Token> token;
		if (IsWordStart(curr)) {
			token = LexWord();  // cannot fail
		} else if (IsDigit(curr)) {
			token = LexNumber();
		} else if (curr == '\'') {
			token = LexString();
		} else {
			token = LexSymbol();
		}
		if (!token.has_value()) {
			return std::unexpected(std::move(token.error()));
		}
		res.push_back(std::move(*token));
	}
}

Token Lexer::LexWord() {
	const std::size_t start = pos_;
	// Maximal munch: read the whole word before looking it up, so `selectx` is one identifier.
	while (pos_ < sql_.size() && IsWordChar(sql_[pos_])) {
		++pos_;
	}

	std::string word(sql_.substr(start, pos_ - start));
	for (char& c : word) {
		c = ToLower(c);
	}

	if (const std::optional<TokenType> keyword = LookupKeyword(word)) {
		return MakeToken(*keyword, start);  // keywords carry no text
	}
	return MakeToken(TokenType::kIdentifier, start, std::move(word));
}

Result<Token> Lexer::LexNumber() {
	const std::size_t start = pos_;
	while (pos_ < sql_.size() && IsDigit(sql_[pos_])) {
		++pos_;
	}

	// `123abc` is not a number followed by a name; reject it rather than split it in two.
	// No overflow check here: whether the digits fit is the parser's call, by context.
	if (pos_ < sql_.size() && IsWordStart(sql_[pos_])) {
		return std::unexpected(ErrorAt(start, "invalid number: digits followed by a letter"));
	}
	return MakeToken(TokenType::kInteger, start, std::string(sql_.substr(start, pos_ - start)));
}

Result<Token> Lexer::LexString() {
	const std::size_t start = pos_;
	++pos_;  // the opening quote

	std::string content;
	while (true) {
		if (pos_ >= sql_.size()) {
			// Reported at the opening quote: that is where the user has to look.
			return std::unexpected(ErrorAt(start, "unterminated string"));
		}
		const char c = sql_[pos_];
		if (c != '\'') {
			content.push_back(c);  // any byte: newline and non-ASCII included
			++pos_;
			continue;
		}
		// A quote: either the first half of an escaped '' or the closing quote.
		if (pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '\'') {
			content.push_back('\'');
			pos_ += 2;
			continue;
		}
		++pos_;  // the closing quote
		return MakeToken(TokenType::kString, start, std::move(content));
	}
}

Result<Token> Lexer::LexSymbol() {
	const std::size_t start = pos_;
	const char c = sql_[pos_];
	++pos_;

	// The second character of a two-character operator, or '\0' at end of input. '\0' can never
	// complete an operator, so "no next character" and "wrong next character" are one case.
	const char next = pos_ < sql_.size() ? sql_[pos_] : '\0';
	auto two_char = [&](TokenType type) {
		++pos_;
		return MakeToken(type, start);
	};

	switch (c) {
		case ',':
			return MakeToken(TokenType::kComma, start);
		case '(':
			return MakeToken(TokenType::kLeftParen, start);
		case ')':
			return MakeToken(TokenType::kRightParen, start);
		case '*':
			return MakeToken(TokenType::kStar, start);
		case '.':
			return MakeToken(TokenType::kDot, start);
		case ';':
			return MakeToken(TokenType::kSemicolon, start);
		case '-':
			// Never a comment: SkipWhitespaceAndComments ran just before and consumed any `--`.
			return MakeToken(TokenType::kMinus, start);
		case '=':
			return MakeToken(TokenType::kEqual, start);
		case '<':
			if (next == '=') return two_char(TokenType::kLessEqual);
			if (next == '>') return two_char(TokenType::kNotEqual);
			return MakeToken(TokenType::kLess, start);
		case '>':
			if (next == '=') return two_char(TokenType::kGreaterEqual);
			return MakeToken(TokenType::kGreater, start);
		case '!':
			if (next == '=') return two_char(TokenType::kNotEqual);
			return std::unexpected(ErrorAt(start, "unexpected character '!' (did you mean '!='?)"));
		default:
			break;
	}

	// Printable ASCII as itself; anything else (control bytes, every non-ASCII byte) as hex, so
	// half a UTF-8 sequence never ends up inside the error message.
	const auto byte = static_cast<unsigned char>(c);
	const std::string shown =
	    (byte >= 0x20 && byte < 0x7f) ? std::format("'{}'", c) : std::format("0x{:02X}", byte);
	return std::unexpected(ErrorAt(start, std::format("unexpected character {}", shown)));
}

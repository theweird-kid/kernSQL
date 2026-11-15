#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.hpp"
#include "sql/parser/token.hpp"

namespace kernsql {

/*
 * The lexer is a pure function of a string, so every test is "this input → these tokens" or
 * "this input → this error at this line and column". DD-007, "Testing", lists what must be
 * covered; each block below names the rule it pins.
 */

namespace {

using T = TokenType;

std::vector<Token> Lex(std::string_view sql) {
	Result<std::vector<Token>> tokens = Lexer::Tokenize(sql);
	EXPECT_TRUE(tokens.has_value()) << sql << " → " << tokens.error().message();
	return tokens.has_value() ? *tokens : std::vector<Token>{};
}

std::vector<TokenType> Types(std::string_view sql) {
	std::vector<TokenType> types;
	for (const Token& token : Lex(sql)) {
		types.push_back(token.type);
	}
	return types;
}

// The lexer must fail on `sql`, with a message naming `line`/`column` and containing `fragment`.
void ExpectError(std::string_view sql, std::size_t line, std::size_t column,
                 std::string_view fragment) {
	Result<std::vector<Token>> tokens = Lexer::Tokenize(sql);
	ASSERT_FALSE(tokens.has_value()) << "lexed without error: " << sql;
	EXPECT_EQ(tokens.error().code(), ErrorCode::kInvalidArgument);
	const std::string message(tokens.error().message());
	const std::string where = "line " + std::to_string(line) + ", column " + std::to_string(column);
	EXPECT_NE(message.find(where), std::string::npos) << sql << " → " << message;
	EXPECT_NE(message.find(fragment), std::string::npos) << sql << " → " << message;
}

}  // namespace

// ---- FormatSyntaxError and TokenTypeName ----

TEST(FormatSyntaxErrorTest, LineAndColumnFromOffset) {
	struct Case {
		std::string_view sql;
		std::size_t offset;
		std::string_view expected;
	};
	const Case cases[] = {
	    {"SELECT x", 0, "line 1, column 1"},
	    {"SELECT x", 7, "line 1, column 8"},
	    {"SELECT\nx", 7, "line 2, column 1"},
	    {"SELECT\nx", 6, "line 1, column 7"},  // the newline belongs to the line it ends
	    {"ab", 2, "line 1, column 3"},         // one past the end: where kEnd sits
	    {"", 0, "line 1, column 1"},
	    {"a\n\nb", 3, "line 3, column 1"},   // blank line in between
	    {"a\nbc\n", 5, "line 3, column 1"},  // after a trailing newline
	};
	for (const Case& c : cases) {
		EXPECT_EQ(FormatSyntaxError(c.sql, c.offset, "msg"),
		          "syntax error at " + std::string(c.expected) + ": msg")
		    << "offset " << c.offset << " in " << c.sql;
	}
}

TEST(TokenTypeNameTest, NamesUsedInErrors) {
	EXPECT_EQ(TokenTypeName(T::kFrom), "FROM");
	EXPECT_EQ(TokenTypeName(T::kInt64), "INT64");
	EXPECT_EQ(TokenTypeName(T::kComma), "','");
	EXPECT_EQ(TokenTypeName(T::kNotEqual), "'!='");
	EXPECT_EQ(TokenTypeName(T::kIdentifier), "identifier");
	EXPECT_EQ(TokenTypeName(T::kEnd), "end of input");
}

// ---- The kEnd invariant: exactly one, last, at sql.size(), length 0 ----

TEST(LexerTest, EmptyAndBlankInputIsJustEnd) {
	for (std::string_view sql : {"", "   ", "\n\t\r ", "-- only a comment", "--"}) {
		const std::vector<Token> tokens = Lex(sql);
		ASSERT_EQ(tokens.size(), 1u) << sql;
		EXPECT_EQ(tokens[0].type, T::kEnd);
		EXPECT_EQ(tokens[0].offset, sql.size()) << sql;
		EXPECT_EQ(tokens[0].length, 0u);
	}
}

TEST(LexerTest, EndSitsAtInputSizeAfterTrailingWhitespace) {
	const std::vector<Token> tokens = Lex("x  \n");
	ASSERT_EQ(tokens.size(), 2u);
	EXPECT_EQ(tokens[1].type, T::kEnd);
	EXPECT_EQ(tokens[1].offset, 4u);
}

// ---- Words: keywords, case folding, identifiers ----

TEST(LexerTest, EveryKeyword) {
	EXPECT_EQ(Types("select from where and insert into values delete create table primary key "
	                "int64 int varchar join on order by asc desc explain"),
	          (std::vector<TokenType>{T::kSelect,  T::kFrom,    T::kWhere,  T::kAnd,    T::kInsert,
	                                  T::kInto,    T::kValues,  T::kDelete, T::kCreate, T::kTable,
	                                  T::kPrimary, T::kKey,     T::kInt64,  T::kInt64,  T::kVarchar,
	                                  T::kJoin,    T::kOn,      T::kOrder,  T::kBy,     T::kAsc,
	                                  T::kDesc,    T::kExplain, T::kEnd}));
}

TEST(LexerTest, KeywordsAreCaseInsensitiveAndCarryNoText) {
	for (std::string_view sql : {"SELECT", "select", "SeLeCt", "sELECT"}) {
		const std::vector<Token> tokens = Lex(sql);
		ASSERT_EQ(tokens.size(), 2u);
		EXPECT_EQ(tokens[0].type, T::kSelect) << sql;
		EXPECT_EQ(tokens[0].text, "") << sql;
		EXPECT_EQ(tokens[0].length, 6u);
	}
}

TEST(LexerTest, IdentifiersAreLowercased) {
	const std::vector<Token> tokens = Lex("Users _tmp A1_b2 x");
	ASSERT_EQ(tokens.size(), 5u);
	for (std::size_t i = 0; i < 4; ++i) {
		EXPECT_EQ(tokens[i].type, T::kIdentifier);
	}
	EXPECT_EQ(tokens[0].text, "users");
	EXPECT_EQ(tokens[1].text, "_tmp");
	EXPECT_EQ(tokens[2].text, "a1_b2");
	EXPECT_EQ(tokens[3].text, "x");
}

TEST(LexerTest, LongestMatchMakesKeywordPrefixesIdentifiers) {
	// Maximal munch: the whole word is read before the keyword lookup.
	for (std::string_view sql : {"selectx", "select_", "select1", "int6", "int640", "_select"}) {
		const std::vector<Token> tokens = Lex(sql);
		ASSERT_EQ(tokens.size(), 2u) << sql;
		EXPECT_EQ(tokens[0].type, T::kIdentifier) << sql;
	}
}

// ---- Integers ----

TEST(LexerTest, IntegersKeepTheirDigitsAsText) {
	const std::vector<Token> tokens = Lex("0 007 42 99999999999999999999999");
	ASSERT_EQ(tokens.size(), 5u);
	EXPECT_EQ(tokens[0].text, "0");
	EXPECT_EQ(tokens[1].text, "007");
	EXPECT_EQ(tokens[2].text, "42");
	// Overflow is the parser's to judge, by context: the token is fine.
	EXPECT_EQ(tokens[3].type, T::kInteger);
	EXPECT_EQ(tokens[3].text, "99999999999999999999999");
}

TEST(LexerTest, SignIsASeparateToken) {
	const std::vector<Token> tokens = Lex("-5");
	ASSERT_EQ(tokens.size(), 3u);
	EXPECT_EQ(tokens[0].type, T::kMinus);
	EXPECT_EQ(tokens[1].type, T::kInteger);
	EXPECT_EQ(tokens[1].text, "5");
}

TEST(LexerTest, DigitsFollowedByLetterAreAnError) {
	ExpectError("123abc", 1, 1, "invalid number");
	ExpectError("x = 1_", 1, 5, "invalid number");
	ExpectError("SELECT\n  9z", 2, 3, "invalid number");
}

TEST(LexerTest, DigitsFollowedByPunctuationAreFine) {
	EXPECT_EQ(
	    Types("1,2)3;4.5"),
	    (std::vector<TokenType>{T::kInteger, T::kComma, T::kInteger, T::kRightParen, T::kInteger,
	                            T::kSemicolon, T::kInteger, T::kDot, T::kInteger, T::kEnd}));
}

// ---- Strings ----

TEST(LexerTest, StringContentAndLength) {
	struct Case {
		std::string_view sql;
		std::string_view text;
		std::size_t length;
	};
	const Case cases[] = {
	    {"'abc'", "abc", 5},
	    {"''", "", 2},
	    {"'it''s'", "it's", 7},
	    {"''''", "'", 4},
	    {"'a''''b'", "a''b", 8},
	    {"'line1\nline2'", "line1\nline2", 13},  // any byte inside, newline included
	    {"'SELECT -- not a comment'", "SELECT -- not a comment", 25},
	    {"'caf\xC3\xA9'", "caf\xC3\xA9", 7},  // non-ASCII is fine inside a string
	    {"'Keep Case'", "Keep Case", 11},     // no lowercasing inside strings
	};
	for (const Case& c : cases) {
		const std::vector<Token> tokens = Lex(c.sql);
		ASSERT_EQ(tokens.size(), 2u) << c.sql;
		EXPECT_EQ(tokens[0].type, T::kString) << c.sql;
		EXPECT_EQ(tokens[0].text, c.text) << c.sql;
		EXPECT_EQ(tokens[0].length, c.length) << c.sql;
	}
}

TEST(LexerTest, AdjacentStringsAreTwoTokens) {
	// `'a' 'b'` (with a space) is two strings, not one with an escaped quote.
	const std::vector<Token> tokens = Lex("'a' 'b'");
	ASSERT_EQ(tokens.size(), 3u);
	EXPECT_EQ(tokens[0].text, "a");
	EXPECT_EQ(tokens[1].text, "b");
}

TEST(LexerTest, UnterminatedStringIsReportedAtTheOpeningQuote) {
	ExpectError("'abc", 1, 1, "unterminated string");
	ExpectError("'", 1, 1, "unterminated string");
	ExpectError("'ab''", 1, 1, "unterminated string");  // '' is an escape: still open
	ExpectError("x = 'oops\nmore", 1, 5, "unterminated string");
}

// ---- Operators and punctuation ----

TEST(LexerTest, EveryPunctuationAndOperator) {
	EXPECT_EQ(
	    Types(", ( ) * . ; - = != <> < <= > >="),
	    (std::vector<TokenType>{T::kComma, T::kLeftParen, T::kRightParen, T::kStar, T::kDot,
	                            T::kSemicolon, T::kMinus, T::kEqual, T::kNotEqual, T::kNotEqual,
	                            T::kLess, T::kLessEqual, T::kGreater, T::kGreaterEqual, T::kEnd}));
}

TEST(LexerTest, OperatorsTakeTheLongestMatchWithoutSpaces) {
	EXPECT_EQ(Types("a<=b"),
	          (std::vector<TokenType>{T::kIdentifier, T::kLessEqual, T::kIdentifier, T::kEnd}));
	EXPECT_EQ(Types("a<>b"),
	          (std::vector<TokenType>{T::kIdentifier, T::kNotEqual, T::kIdentifier, T::kEnd}));
	EXPECT_EQ(Types("a>=b"),
	          (std::vector<TokenType>{T::kIdentifier, T::kGreaterEqual, T::kIdentifier, T::kEnd}));
	EXPECT_EQ(Types("a<-1"),
	          (std::vector<TokenType>{T::kIdentifier, T::kLess, T::kMinus, T::kInteger, T::kEnd}));
	// `><` is two operators, not a not-equal: only `<>` and `!=` are.
	EXPECT_EQ(Types("><"), (std::vector<TokenType>{T::kGreater, T::kLess, T::kEnd}));
	// `<` `=` with a space between is two tokens.
	EXPECT_EQ(Types("< ="), (std::vector<TokenType>{T::kLess, T::kEqual, T::kEnd}));
}

TEST(LexerTest, OperatorAtEndOfInputDoesNotReadPastIt) {
	// Under ASan these would catch a peek at sql_[pos_ + 1] past the end.
	EXPECT_EQ(Types("a <"), (std::vector<TokenType>{T::kIdentifier, T::kLess, T::kEnd}));
	EXPECT_EQ(Types("a >"), (std::vector<TokenType>{T::kIdentifier, T::kGreater, T::kEnd}));
	ExpectError("a !", 1, 3, "'!'");
}

TEST(LexerTest, LoneBangIsAnError) {
	ExpectError("a ! b", 1, 3, "'!'");
	ExpectError("!", 1, 1, "'!'");
}

// ---- Comments ----

TEST(LexerTest, CommentsRunToEndOfLine) {
	EXPECT_EQ(Types("SELECT -- the columns\n* FROM t -- trailing"),
	          (std::vector<TokenType>{T::kSelect, T::kStar, T::kFrom, T::kIdentifier, T::kEnd}));
	EXPECT_EQ(Types("x--y"), (std::vector<TokenType>{T::kIdentifier, T::kEnd}));
	EXPECT_EQ(Types("-- a\n-- b\nx"), (std::vector<TokenType>{T::kIdentifier, T::kEnd}));
}

TEST(LexerTest, MinusThenMinusAcrossSpaceIsTwoMinuses) {
	// `- -5` has a space: no comment, two minus tokens (the parser will reject it).
	EXPECT_EQ(Types("- -5"), (std::vector<TokenType>{T::kMinus, T::kMinus, T::kInteger, T::kEnd}));
}

// ---- Bytes that start no token ----

TEST(LexerTest, UnexpectedPrintableCharacterIsShownQuoted) {
	ExpectError("SELECT @", 1, 8, "unexpected character '@'");
	ExpectError("a # b", 1, 3, "unexpected character '#'");
	ExpectError("\"Foo\"", 1, 1, "unexpected character '\"'");  // no quoted identifiers
}

TEST(LexerTest, NonAsciiAndControlBytesAreShownAsHex) {
	ExpectError("caf\xC3\xA9", 1, 4, "0xC3");
	ExpectError("x \x01", 1, 3, "0x01");
	ExpectError(std::string_view("a\0b", 3), 1, 2, "0x00");  // an embedded NUL is just a byte
}

// ---- Offsets and lengths of every token ----

TEST(LexerTest, OffsetsAndLengths) {
	//                         0         1         2
	//                         0123456789012345678901234567
	const std::vector<Token> tokens = Lex("SELECT a, 'x''y' FROM t<=-7");
	struct Expected {
		TokenType type;
		std::size_t offset;
		std::size_t length;
	};
	const Expected expected[] = {
	    {T::kSelect, 0, 6},   {T::kIdentifier, 7, 1},  {T::kComma, 8, 1},      {T::kString, 10, 6},
	    {T::kFrom, 17, 4},    {T::kIdentifier, 22, 1}, {T::kLessEqual, 23, 2}, {T::kMinus, 25, 1},
	    {T::kInteger, 26, 1}, {T::kEnd, 27, 0},
	};
	ASSERT_EQ(tokens.size(), std::size(expected));
	for (std::size_t i = 0; i < tokens.size(); ++i) {
		EXPECT_EQ(tokens[i].type, expected[i].type) << "token " << i;
		EXPECT_EQ(tokens[i].offset, expected[i].offset) << "token " << i;
		EXPECT_EQ(tokens[i].length, expected[i].length) << "token " << i;
	}
}

TEST(LexerTest, ErrorLineAndColumnOnLaterLines) {
	ExpectError("SELECT *\nFROM t\nWHERE a = @", 3, 11, "'@'");
}

// ---- Garbage: never crashes, always ok or kInvalidArgument, kEnd invariant on success ----

TEST(LexerTest, RandomBytesNeverCrash) {
	std::mt19937 rng(20261115);
	// Biased toward the characters that start or continue tokens, so the interesting paths run.
	constexpr std::string_view kAlphabet = "aZ_9 '\n-<>=!,().;*\"@\x80\xFF\0";
	std::uniform_int_distribution<std::size_t> pick(0, kAlphabet.size() - 1);
	std::uniform_int_distribution<std::size_t> length(0, 24);
	for (int i = 0; i < 10'000; ++i) {
		std::string sql;
		for (std::size_t n = length(rng); n > 0; --n) {
			sql.push_back(kAlphabet[pick(rng)]);
		}
		Result<std::vector<Token>> tokens = Lexer::Tokenize(sql);
		if (!tokens.has_value()) {
			ASSERT_EQ(tokens.error().code(), ErrorCode::kInvalidArgument);
			continue;
		}
		ASSERT_FALSE(tokens->empty());
		ASSERT_EQ(tokens->back().type, T::kEnd);
		ASSERT_EQ(tokens->back().offset, sql.size());
		for (std::size_t t = 0; t + 1 < tokens->size(); ++t) {
			ASSERT_NE((*tokens)[t].type, T::kEnd);
			// Tokens are in order, inside the input, and never overlap.
			ASSERT_LE((*tokens)[t].offset + (*tokens)[t].length, (*tokens)[t + 1].offset);
		}
	}
}

}  // namespace kernsql

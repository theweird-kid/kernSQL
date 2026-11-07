#include "catalog/schema.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "common/status.hpp"
#include "heap/heap_page.hpp"

namespace kernsql {

/*
 * Schema is pure: no pages, no pool, no file. So these tests work at the byte level — the row
 * format is checked against a hand-built byte array rather than only against Decode, because a
 * round trip alone passes for ANY self-consistent encoding, including one that is wrong on disk
 * and would silently break every file written before it was fixed.
 *
 * Expected bytes are built with memcpy, not written as literals, because the format is
 * host-endian (the page_header.hpp convention): the test states the format, not this machine's
 * byte order.
 */

namespace {

Column Int(std::string name) {
	return Column{std::move(name), ColumnType::kInt64, 0};
}
Column Varchar(std::string name, uint16_t n) {
	return Column{std::move(name), ColumnType::kVarchar, n};
}

// Create or fail the test outright.
Schema MustCreate(std::vector<Column> columns, uint16_t primary_key = 0) {
	auto schema = Schema::Create(std::move(columns), primary_key);
	EXPECT_TRUE(schema.has_value()) << schema.error().message();
	if (!schema.has_value()) return Schema::Create({Int("fallback")}, 0).value();
	return std::move(schema.value());
}

// Create must refuse, and with kInvalidArgument specifically — a schema rejected for the wrong
// reason (an assert, a corruption code) would pass a bare !has_value().
void ExpectRejected(std::vector<Column> columns, uint16_t primary_key, const char* why) {
	auto schema = Schema::Create(std::move(columns), primary_key);
	ASSERT_FALSE(schema.has_value()) << "accepted a schema that should fail: " << why;
	EXPECT_EQ(schema.error().code(), ErrorCode::kInvalidArgument) << why;
}

void AppendInt(std::vector<std::byte>& out, int64_t v) {
	const std::size_t at = out.size();
	out.resize(at + sizeof v);
	std::memcpy(out.data() + at, &v, sizeof v);
}
void AppendVarchar(std::vector<std::byte>& out, std::string_view s) {
	const auto length = static_cast<uint16_t>(s.size());
	const std::size_t at = out.size();
	out.resize(at + sizeof length + s.size());
	std::memcpy(out.data() + at, &length, sizeof length);
	std::memcpy(out.data() + at + sizeof length, s.data(), s.size());
}

// The schema most tests share: key not in position 0, so nothing passes by assuming it is.
Schema UsersSchema() {
	return MustCreate({Varchar("name", 16), Int("id"), Varchar("email", 32), Int("age")}, 1);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// IsValidName
// ---------------------------------------------------------------------------------------------

TEST(SchemaNameTest, AcceptsLowercaseIdentifiers) {
	for (const char* name : {"a", "_", "t1", "a_b", "_x9", "users", "z0_"}) {
		EXPECT_TRUE(IsValidName(name)) << name;
	}
	EXPECT_TRUE(IsValidName(std::string(MAX_NAME_LENGTH, 'a')));
}

TEST(SchemaNameTest, RejectsEverythingElse) {
	EXPECT_FALSE(IsValidName(""));
	EXPECT_FALSE(IsValidName(std::string(MAX_NAME_LENGTH + 1, 'a')));
	// Uppercase is rejected, not folded: lowercasing is the parser's job, and a name that
	// reached disk with capitals could never be matched by a lowercased lookup.
	for (const char* name :
	     {"A", "Users", "1a", "9", "a-b", "a b", " a", "a.b", "a;", "\xc3\xa9"}) {
		EXPECT_FALSE(IsValidName(name)) << name;
	}
	EXPECT_FALSE(IsValidName(std::string_view("a\0b", 3))) << "embedded NUL";
}

// ---------------------------------------------------------------------------------------------
// Schema::Create
// ---------------------------------------------------------------------------------------------

TEST(SchemaCreateTest, BuildsAndExposesTheColumns) {
	const Schema s = UsersSchema();
	ASSERT_EQ(s.ColumnCount(), 4u);
	EXPECT_EQ(s.PrimaryKeyColumn(), 1);
	EXPECT_EQ(s.Columns()[0], Varchar("name", 16));
	EXPECT_EQ(s.Columns()[1], Int("id"));
	EXPECT_EQ(s.Columns()[2], Varchar("email", 32));
	EXPECT_EQ(s.Columns()[3], Int("age"));
}

TEST(SchemaCreateTest, RejectsAnEmptyColumnList) {
	ExpectRejected({}, 0, "no columns");
}

TEST(SchemaCreateTest, RejectsBadAndDuplicateColumnNames) {
	ExpectRejected({Int("id"), Int("")}, 0, "empty name");
	ExpectRejected({Int("id"), Int("Name")}, 0, "uppercase name");
	ExpectRejected({Int("id"), Int(std::string(MAX_NAME_LENGTH + 1, 'x'))}, 0, "name too long");
	ExpectRejected({Int("id"), Varchar("id", 4)}, 0, "duplicate name, different type");
	ExpectRejected({Int("a"), Int("b"), Int("a")}, 0, "duplicate name, not adjacent");
}

TEST(SchemaCreateTest, RejectsBadLengths) {
	ExpectRejected({Column{"id", ColumnType::kInt64, 8}}, 0, "INT64 with a max_length");
	ExpectRejected({Int("id"), Varchar("v", 0)}, 0, "VARCHAR(0)");
	ExpectRejected({Int("id"), Varchar("v", MAX_VARCHAR_LENGTH + 1)}, 0, "VARCHAR over the max");
}

TEST(SchemaCreateTest, RejectsAColumnTypeOutsideTheEnum) {
	// 0 is deliberately not a type, so a zeroed catalog stream fails here instead of decoding.
	for (const int raw : {0, 3, 255}) {
		ExpectRejected({Int("id"), Column{"x", static_cast<ColumnType>(raw), 0}}, 0,
		               "type outside the enum");
	}
}

TEST(SchemaCreateTest, RejectsABadPrimaryKey) {
	ExpectRejected({Int("id"), Int("x")}, 2, "primary key past the end");
	ExpectRejected({Int("id")}, std::numeric_limits<uint16_t>::max(), "primary key far past");
	ExpectRejected({Int("id"), Varchar("name", 8)}, 1, "VARCHAR primary key");
}

// The CREATE TABLE half of the 2000-byte cap, at the exact boundary.
TEST(SchemaCreateTest, TheRowCapIsInclusiveAtMaxTupleSize) {
	static_assert(MAX_TUPLE_SIZE == 2000, "boundary arithmetic below assumes 2000");

	// 8 + (2 + 1990) = 2000: accepted. One more byte: rejected.
	const Schema at = MustCreate({Int("id"), Varchar("v", 1990)});
	EXPECT_EQ(at.MaxRowSize(), MAX_TUPLE_SIZE);
	ExpectRejected({Int("id"), Varchar("v", 1991)}, 0, "2001-byte row");

	// The same boundary reached through INT64s alone: 250 * 8 = 2000, 251 * 8 = 2008.
	std::vector<Column> ints;
	for (int i = 0; i < 251; ++i) ints.push_back(Int("c" + std::to_string(i)));
	ExpectRejected(ints, 0, "251 INT64 columns");
	ints.pop_back();
	EXPECT_EQ(MustCreate(ints).MaxRowSize(), MAX_TUPLE_SIZE);
}

// MAX_VARCHAR_LENGTH is the per-column bound; the row cap is tighter. Because the key must be
// INT64, a VARCHAR(MAX_VARCHAR_LENGTH) can never appear in a legal schema — the per-column check
// alone would let through a table the heap could not store, which is why the row check exists.
TEST(SchemaCreateTest, TheWidestLegalColumnIsBoundByTheRowCapNotTheColumnCap) {
	ExpectRejected({Int("id"), Varchar("v", MAX_VARCHAR_LENGTH)}, 0, "8 + 2 + 1998 bytes");
}

// ---------------------------------------------------------------------------------------------
// ColumnIndex / MaxRowSize / KeyOf / equality
// ---------------------------------------------------------------------------------------------

TEST(SchemaLookupTest, ColumnIndexIsAnExactMatch) {
	const Schema s = UsersSchema();
	EXPECT_EQ(s.ColumnIndex("name"), 0);
	EXPECT_EQ(s.ColumnIndex("id"), 1);
	EXPECT_EQ(s.ColumnIndex("age"), 3);
	EXPECT_EQ(s.ColumnIndex("missing"), std::nullopt);
	EXPECT_EQ(s.ColumnIndex("ID"), std::nullopt) << "the parser lowercases, not the schema";
	EXPECT_EQ(s.ColumnIndex("nam"), std::nullopt) << "prefix";
	EXPECT_EQ(s.ColumnIndex(""), std::nullopt);
}

TEST(SchemaLookupTest, MaxRowSizeSumsTheWorstCaseOfEveryColumn) {
	EXPECT_EQ(MustCreate({Int("id")}).MaxRowSize(), 8u);
	// (2 + 16) + 8 + (2 + 32) + 8
	EXPECT_EQ(UsersSchema().MaxRowSize(), 68u);
}

TEST(SchemaLookupTest, KeyOfReadsThePrimaryKeyColumnWhereverItIs) {
	const Schema s = UsersSchema();
	EXPECT_EQ(s.KeyOf(Row{std::string("ann"), int64_t{42}, std::string("a@x"), int64_t{7}}), 42);
	EXPECT_EQ(s.KeyOf(Row{std::string(""), int64_t{-5}, std::string(""), int64_t{99}}), -5);
}

TEST(SchemaLookupTest, EqualityComparesColumnsAndKey) {
	EXPECT_EQ(UsersSchema(), UsersSchema());
	EXPECT_NE(MustCreate({Int("a"), Int("b")}, 0), MustCreate({Int("a"), Int("b")}, 1));
	EXPECT_NE(MustCreate({Int("a"), Varchar("b", 4)}), MustCreate({Int("a"), Varchar("b", 5)}));
	EXPECT_NE(MustCreate({Int("a"), Int("b")}), MustCreate({Int("a"), Int("c")}));
}

// ---------------------------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------------------------

TEST(SchemaValidateTest, AcceptsALegalRowIncludingEmptyAndFullStrings) {
	const Schema s = UsersSchema();
	EXPECT_TRUE(s.Validate({std::string(""), int64_t{1}, std::string(""), int64_t{0}}).ok());
	EXPECT_TRUE(
	    s.Validate({std::string(16, 'n'), int64_t{1}, std::string(32, 'e'), int64_t{0}}).ok());
}

TEST(SchemaValidateTest, RejectsTheWrongNumberOfValues) {
	const Schema s = UsersSchema();
	for (const Row& row :
	     {Row{}, Row{std::string("a"), int64_t{1}, std::string("b")},
	      Row{std::string("a"), int64_t{1}, std::string("b"), int64_t{2}, int64_t{3}}}) {
		const Status st = s.Validate(row);
		EXPECT_EQ(st.code(), ErrorCode::kInvalidArgument) << row.size() << " values";
	}
}

TEST(SchemaValidateTest, RejectsAValueOfTheWrongTypeInEitherDirection) {
	const Schema s = UsersSchema();
	EXPECT_EQ(s.Validate({int64_t{1}, int64_t{1}, std::string(""), int64_t{0}}).code(),
	          ErrorCode::kInvalidArgument)
	    << "INT64 into VARCHAR";
	EXPECT_EQ(s.Validate({std::string(""), std::string("1"), std::string(""), int64_t{0}}).code(),
	          ErrorCode::kInvalidArgument)
	    << "VARCHAR into INT64";
}

// The INSERT half of the 2000-byte cap: n is in bytes, so a multi-byte character counts double.
TEST(SchemaValidateTest, RejectsAStringOneByteOverItsLength) {
	const Schema s = MustCreate({Int("id"), Varchar("v", 4)});
	EXPECT_TRUE(s.Validate({int64_t{1}, std::string("abcd")}).ok());
	EXPECT_EQ(s.Validate({int64_t{1}, std::string("abcde")}).code(), ErrorCode::kInvalidArgument);
	EXPECT_EQ(s.Validate({int64_t{1}, std::string("ab\xc3\xa9\xc3\xa9")}).code(),
	          ErrorCode::kInvalidArgument)
	    << "3 characters, 6 bytes";
}

// ---------------------------------------------------------------------------------------------
// Encode — the on-disk format
// ---------------------------------------------------------------------------------------------

TEST(SchemaEncodeTest, ProducesTheSequentialFormatByteForByte) {
	const Schema s = UsersSchema();
	auto bytes = s.Encode({std::string("ann"), int64_t{42}, std::string(""), int64_t{-1}});
	ASSERT_TRUE(bytes.has_value()) << bytes.error().message();

	// Schema order, no row header, no padding, no alignment.
	std::vector<std::byte> expected;
	AppendVarchar(expected, "ann");
	AppendInt(expected, 42);
	AppendVarchar(expected, "");
	AppendInt(expected, -1);
	EXPECT_EQ(bytes.value(), expected);
	EXPECT_EQ(bytes.value().size(), 2 + 3 + 8 + 2 + 0 + 8u);
}

TEST(SchemaEncodeTest, AFullRowEncodesToExactlyMaxRowSize) {
	const Schema s = UsersSchema();
	auto bytes = s.Encode({std::string(16, 'n'), int64_t{1}, std::string(32, 'e'), int64_t{2}});
	ASSERT_TRUE(bytes.has_value());
	EXPECT_EQ(bytes.value().size(), s.MaxRowSize());

	// And at the global cap, which is what makes the heap's MAX_TUPLE_SIZE check unreachable.
	const Schema widest = MustCreate({Int("id"), Varchar("v", 1990)});
	auto wide = widest.Encode({int64_t{1}, std::string(1990, 'w')});
	ASSERT_TRUE(wide.has_value());
	EXPECT_EQ(wide.value().size(), MAX_TUPLE_SIZE);
}

TEST(SchemaEncodeTest, RefusesAnInvalidRowWithoutPartialOutput) {
	const Schema s = UsersSchema();
	auto bytes = s.Encode({std::string("ann"), int64_t{1}, std::string(33, 'e'), int64_t{0}});
	ASSERT_FALSE(bytes.has_value());
	EXPECT_EQ(bytes.error().code(), ErrorCode::kInvalidArgument);
}

// ---------------------------------------------------------------------------------------------
// Decode — bytes off a page, trusted for nothing
// ---------------------------------------------------------------------------------------------

TEST(SchemaDecodeTest, RoundTripsTheAwkwardValues) {
	const Schema s = MustCreate({Int("id"), Varchar("v", 8), Int("w")});
	const std::vector<Row> rows = {
	    {std::numeric_limits<int64_t>::min(), std::string(""), std::numeric_limits<int64_t>::max()},
	    {int64_t{0}, std::string("\0\0\0", 3), int64_t{-1}},  // NULs are data, not terminators
	    {int64_t{1}, std::string("\xff\x00\xfe", 3), int64_t{0}},
	    {int64_t{-7}, std::string(8, 'x'), int64_t{7}},
	};
	for (const Row& row : rows) {
		auto bytes = s.Encode(row);
		ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
		auto back = s.Decode(bytes.value());
		ASSERT_TRUE(back.has_value()) << back.error().message();
		EXPECT_EQ(back.value(), row);
	}
}

// Every proper prefix of a valid row — cut inside an INT64, inside a length prefix, inside
// string bytes, and exactly between columns — must be corruption, never a short row.
TEST(SchemaDecodeTest, EveryTruncationIsCorruption) {
	const Schema s = UsersSchema();
	auto bytes = s.Encode({std::string("ann"), int64_t{42}, std::string("a@x.io"), int64_t{30}});
	ASSERT_TRUE(bytes.has_value());
	const std::vector<std::byte>& full = bytes.value();

	for (std::size_t len = 0; len < full.size(); ++len) {
		auto row = s.Decode(std::span(full).first(len));
		ASSERT_FALSE(row.has_value()) << "decoded a row from the first " << len << " bytes";
		EXPECT_EQ(row.error().code(), ErrorCode::kCorruption) << len << " bytes";
	}
}

TEST(SchemaDecodeTest, TrailingBytesAreCorruption) {
	const Schema s = UsersSchema();
	auto bytes = s.Encode({std::string("ann"), int64_t{42}, std::string(""), int64_t{30}});
	ASSERT_TRUE(bytes.has_value());
	std::vector<std::byte> longer = bytes.value();
	longer.push_back(std::byte{0});

	auto row = s.Decode(longer);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kCorruption);
}

// A stored length over the column's n is corruption even when the bytes to back it are there:
// a row that decoded would be one Validate rejects, and the executor could then hand back a
// value no INSERT could have written.
TEST(SchemaDecodeTest, AStoredLengthOverTheColumnsNIsCorruption) {
	const Schema s = MustCreate({Int("id"), Varchar("v", 4)});
	std::vector<std::byte> bytes;
	AppendInt(bytes, 1);
	AppendVarchar(bytes, "abcde");  // five bytes, present in full

	auto row = s.Decode(bytes);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kCorruption);
}

// A huge length prefix must be caught by the bounds check, not turned into a giant allocation
// or a read past the span (asan watches the latter).
TEST(SchemaDecodeTest, AMaximalLengthPrefixDoesNotReadPastTheBytes) {
	const Schema s = MustCreate({Int("id"), Varchar("v", 1990)});
	std::vector<std::byte> bytes;
	AppendInt(bytes, 1);
	const uint16_t length = 1990;  // legal for the column, but only 3 bytes follow
	const std::size_t at = bytes.size();
	bytes.resize(at + sizeof length + 3);
	std::memcpy(bytes.data() + at, &length, sizeof length);

	auto row = s.Decode(bytes);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kCorruption);
}

// Fixed seed: a failure is reproducible, and the case count is small enough to stay fast.
TEST(SchemaDecodeTest, RandomRowsRoundTrip) {
	const Schema s =
	    MustCreate({Int("id"), Varchar("a", 1), Varchar("b", 50), Int("c"), Varchar("d", 300)}, 0);
	std::mt19937_64 rng(20261107);
	auto random_string = [&rng](uint16_t max) {
		std::string out(std::uniform_int_distribution<std::size_t>(0, max)(rng), '\0');
		for (char& ch : out) ch = static_cast<char>(rng());
		return out;
	};

	for (int i = 0; i < 2000; ++i) {
		const Row row = {static_cast<int64_t>(rng()), random_string(1), random_string(50),
		                 static_cast<int64_t>(rng()), random_string(300)};
		auto bytes = s.Encode(row);
		ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
		ASSERT_LE(bytes.value().size(), s.MaxRowSize());
		auto back = s.Decode(bytes.value());
		ASSERT_TRUE(back.has_value()) << "case " << i << ": " << back.error().message();
		ASSERT_EQ(back.value(), row) << "case " << i;
	}
}

}  // namespace kernsql

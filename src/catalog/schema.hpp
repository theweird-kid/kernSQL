#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/status.hpp"
#include "heap/heap_page.hpp"
#include "indices/b_plus_tree_page.hpp"

namespace kernsql {

/*
 * Column types (DD-006, "Row format"). Two on purpose: INT64 covers keys, numbers and join
 * columns, VARCHAR covers text, and every further type adds encoding, comparison and parsing
 * code to every later stage without teaching anything new.
 *
 * Persisted as a u8 in the catalog stream. 0 is deliberately NOT a type, so a zeroed or
 * truncated stream fails validation instead of decoding as a column of the first type.
 */
enum class ColumnType : uint8_t {
	kInt64 = 1,
	kVarchar = 2,
};

// Table and column names, in bytes. Persisted with a u8 length, so anything up to 255 would fit;
// 64 is the limit the catalog enforces.
inline constexpr std::size_t MAX_NAME_LENGTH = 64;

// Bytes a VARCHAR spends on its length prefix, and an INT64 on its value.
inline constexpr std::size_t VARCHAR_LENGTH_PREFIX_SIZE = sizeof(uint16_t);
inline constexpr std::size_t INT64_SIZE = sizeof(int64_t);

// The largest n a VARCHAR(n) can declare: one such column alone must still fit MAX_TUPLE_SIZE.
// A schema can still be rejected below this — the cap is on the whole row (Schema::Create).
inline constexpr uint16_t MAX_VARCHAR_LENGTH =
    static_cast<uint16_t>(MAX_TUPLE_SIZE - VARCHAR_LENGTH_PREFIX_SIZE);

/*
 * A legal table or column name: 1 to MAX_NAME_LENGTH bytes of [a-z_][a-z0-9_]*.
 *
 * The parser lowercases identifiers and should never produce anything else; the catalog calls
 * this again anyway, so a name built by hand — a test, a future API — cannot reach disk in a
 * shape a later lookup would never match. Lowercase-only is what makes names case-insensitive
 * without the catalog ever comparing case-insensitively.
 */
[[nodiscard]] bool IsValidName(std::string_view name);

struct Column {
	std::string name;
	ColumnType type{ColumnType::kInt64};

	// VARCHAR(n): n, in BYTES, 1 .. MAX_VARCHAR_LENGTH. INT64: always 0.
	uint16_t max_length{0};

	bool operator==(const Column&) const = default;
};

/*
 * One column's value. No NULL alternative: every column is NOT NULL (DD-006, "No NULLs").
 *
 * std::string for VARCHAR, compared with operator<, which is memcmp order — exactly the
 * collation DD-006 chose. Never give it a locale.
 */
using Value = std::variant<int64_t, std::string>;

// One value per column, in schema order. What every operator above this layer passes around.
using Row = std::vector<Value>;

/*
 * A table's columns and its primary key, plus the row format built on them.
 *
 * ROW FORMAT (DD-006, "Encoding: sequential"): the columns in schema order, no row header. INT64
 * is 8 bytes; VARCHAR is a u16 byte length followed by that many bytes. Host-endian memcpy, the
 * page_header.hpp convention. To reach column k, walk the k columns before it — fine, because
 * the executor always decodes whole rows.
 *
 * A value type, cheap to copy at this size, and IMMUTABLE once built: Create is the only way to
 * get one, so every Schema in the program has passed validation.
 */
class Schema {
  public:
	/*
	 * Validate and build. kInvalidArgument, with the reason, unless ALL of:
	 *   - at least one column;
	 *   - every name passes IsValidName, and no two are equal;
	 *   - every INT64 has max_length 0, every VARCHAR 1 .. MAX_VARCHAR_LENGTH;
	 *   - `primary_key` names an existing column, and that column is INT64;
	 *   - MaxRowSize() <= MAX_TUPLE_SIZE.
	 *
	 * The last check is the CREATE TABLE half of the 2000-byte cap: a schema that passes it
	 * cannot produce a row the heap would refuse.
	 */
	[[nodiscard]] static Result<Schema> Create(std::vector<Column> columns, uint16_t primary_key);

	[[nodiscard]] const std::vector<Column>& Columns() const { return columns_; }
	[[nodiscard]] std::size_t ColumnCount() const { return columns_.size(); }
	[[nodiscard]] uint16_t PrimaryKeyColumn() const { return primary_key_; }

	// The position of the column called `name`, or nullopt. Exact match — names are stored
	// lowercase, and lowercasing the query is the parser's job.
	[[nodiscard]] std::optional<uint16_t> ColumnIndex(std::string_view name) const;

	// The encoded size of the largest row this schema allows: INT64_SIZE per INT64 column,
	// VARCHAR_LENGTH_PREFIX_SIZE + n per VARCHAR(n).
	[[nodiscard]] std::size_t MaxRowSize() const;

	/*
	 * Is `row` a legal row of this schema? kInvalidArgument, naming the column, on the wrong
	 * number of values, a value of the wrong type, or a string longer than its column's n.
	 *
	 * The INSERT half of the 2000-byte cap: a row that passes encodes to at most MaxRowSize().
	 */
	[[nodiscard]] Status Validate(const Row& row) const;

	/*
	 * Encode `row` in the format above. Validates first, so a row that encodes is a legal row —
	 * callers never need a separate Validate. kInvalidArgument as Validate.
	 */
	[[nodiscard]] Result<std::vector<std::byte>> Encode(const Row& row) const;

	/*
	 * Decode bytes produced by Encode under this schema. The bytes come off a page, so nothing
	 * is trusted: kCorruption if they run out mid-column, if a VARCHAR length exceeds its
	 * column's n, or if bytes are left over at the end.
	 */
	[[nodiscard]] Result<Row> Decode(std::span<const std::byte> bytes) const;

	// The primary-key value — the index key. Precondition (asserted): `row` passed Validate.
	[[nodiscard]] index_key_t KeyOf(const Row& row) const;

	bool operator==(const Schema&) const = default;

  private:
	Schema(std::vector<Column> columns, uint16_t primary_key)
	    : columns_(std::move(columns)), primary_key_(primary_key) {}

	std::vector<Column> columns_;
	uint16_t primary_key_;
};

}  // namespace kernsql

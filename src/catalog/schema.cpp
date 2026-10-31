#include "catalog/schema.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <numeric>
#include <ranges>
#include <set>
#include <string>
#include <utility>

#include "common/status.hpp"
#include "heap/heap_page.hpp"

using namespace kernsql;

namespace {

std::size_t MaxEncodedSize(const Column& col) {
	return col.type == ColumnType::kVarchar ? VARCHAR_LENGTH_PREFIX_SIZE + col.max_length
	                                        : INT64_SIZE;
}

bool IsNameStart(char c) {
	return (c >= 'a' && c <= 'z') || c == '_';
}
bool IsNameChar(char c) {
	return IsNameStart(c) || (c >= '0' && c <= '9');
}

}  // namespace

bool kernsql::IsValidName(std::string_view name) {
	if (name.empty() || name.size() > MAX_NAME_LENGTH) return false;
	return IsNameStart(name.front()) && std::ranges::all_of(name, IsNameChar);
}

Result<Schema> Schema::Create(std::vector<Column> columns, uint16_t primary_key) {
	if (columns.empty()) {
		return std::unexpected(Status::InvalidArgument("at least one column must be specified"));
	}

	std::set<std::string_view> column_names;
	std::size_t max_row_size{0};
	for (const auto& col : columns) {
		if (!IsValidName(col.name)) {
			return std::unexpected(Status::InvalidArgument("invalid column name: " + col.name));
		}
		// insert's .second is false when the name was already there.
		if (!column_names.insert(col.name).second) {
			return std::unexpected(Status::InvalidArgument("duplicate column name: " + col.name));
		}
		switch (col.type) {
			case ColumnType::kInt64:
				if (col.max_length != 0) {
					return std::unexpected(
					    Status::InvalidArgument("INT64 column has a max_length: " + col.name));
				}
				break;
			case ColumnType::kVarchar:
				if (col.max_length < 1 || col.max_length > MAX_VARCHAR_LENGTH) {
					return std::unexpected(Status::InvalidArgument(
					    "VARCHAR length outside [1, " + std::to_string(MAX_VARCHAR_LENGTH) +
					    "]: " + col.name));
				}
				break;
			default:
				return std::unexpected(
				    Status::InvalidArgument("invalid column type for: " + col.name));
		}
		max_row_size += MaxEncodedSize(col);
	}

	if (max_row_size > MAX_TUPLE_SIZE) {
		return std::unexpected(
		    Status::InvalidArgument("max row size " + std::to_string(max_row_size) +
			                        " exceeds the limit of " + std::to_string(MAX_TUPLE_SIZE)));
	}

	if (primary_key >= columns.size()) {
		return std::unexpected(Status::InvalidArgument("primary key column out of range"));
	}
	if (columns[primary_key].type != ColumnType::kInt64) {
		return std::unexpected(
		    Status::InvalidArgument("primary key must be INT64: " + columns[primary_key].name));
	}

	return Schema(std::move(columns), primary_key);
}

std::optional<uint16_t> Schema::ColumnIndex(std::string_view name) const {
	const auto it =
	    std::ranges::find_if(columns_, [name](const Column& col) { return col.name == name; });
	if (it == columns_.end()) return std::nullopt;
	return static_cast<uint16_t>(it - columns_.begin());
}

std::size_t Schema::MaxRowSize() const {
	auto sizes = columns_ | std::views::transform(MaxEncodedSize);
	return std::accumulate(sizes.begin(), sizes.end(), std::size_t{0});
}

Status Schema::Validate(const Row& row) const {
	if (row.size() != columns_.size()) {
		return Status::InvalidArgument("expected " + std::to_string(columns_.size()) +
		                               " values, got " + std::to_string(row.size()));
	}

	for (std::size_t i = 0; i < columns_.size(); ++i) {
		const Column& col = columns_[i];
		if (col.type == ColumnType::kInt64) {
			if (!std::holds_alternative<int64_t>(row[i])) {
				return Status::InvalidArgument("column " + col.name + " expects INT64");
			}
			continue;
		}

		const auto* str = std::get_if<std::string>(&row[i]);
		if (str == nullptr) {
			return Status::InvalidArgument("column " + col.name + " expects VARCHAR");
		}
		if (str->size() > col.max_length) {
			return Status::InvalidArgument("value too long for column " + col.name + ": " +
			                               std::to_string(str->size()) + " bytes, max " +
			                               std::to_string(col.max_length));
		}
	}
	return Status::OK();
}

Result<std::vector<std::byte>> Schema::Encode(const Row& row) const {
	if (Status s = Validate(row); !s.ok()) return std::unexpected(std::move(s));

	std::vector<std::byte> out;
	out.reserve(MaxRowSize());
	auto append = [&out](const void* data, std::size_t size) {
		const auto* bytes = static_cast<const std::byte*>(data);
		out.insert(out.end(), bytes, bytes + size);
	};

	for (const Value& value : row) {
		if (const auto* num = std::get_if<int64_t>(&value)) {
			append(num, INT64_SIZE);
			continue;
		}
		// Validate bounded the length by max_length <= MAX_VARCHAR_LENGTH, so it fits a u16.
		const auto& str = std::get<std::string>(value);
		const auto length = static_cast<uint16_t>(str.size());
		append(&length, VARCHAR_LENGTH_PREFIX_SIZE);
		append(str.data(), str.size());
	}
	return out;
}

Result<Row> Schema::Decode(std::span<const std::byte> bytes) const {
	Row row;
	row.reserve(columns_.size());
	std::size_t offset = 0;

	// Copy `size` bytes at the cursor into `dest` and advance, or report that they aren't there.
	auto read = [&bytes, &offset](void* dest, std::size_t size) {
		if (bytes.size() - offset < size) return false;
		std::memcpy(dest, bytes.data() + offset, size);
		offset += size;
		return true;
	};

	for (const Column& col : columns_) {
		if (col.type == ColumnType::kInt64) {
			int64_t num{};
			if (!read(&num, INT64_SIZE)) {
				return std::unexpected(Status::Corruption("row ends inside column " + col.name));
			}
			row.emplace_back(num);
			continue;
		}

		uint16_t length{};
		if (!read(&length, VARCHAR_LENGTH_PREFIX_SIZE)) {
			return std::unexpected(Status::Corruption("row ends inside column " + col.name));
		}
		if (length > col.max_length) {
			return std::unexpected(
			    Status::Corruption("stored length " + std::to_string(length) + " exceeds VARCHAR(" +
				                   std::to_string(col.max_length) + ") in " + col.name));
		}
		std::string str(length, '\0');
		if (!read(str.data(), length)) {
			return std::unexpected(Status::Corruption("row ends inside column " + col.name));
		}
		row.emplace_back(std::move(str));
	}

	if (offset != bytes.size()) {
		return std::unexpected(Status::Corruption(std::to_string(bytes.size() - offset) +
		                                          " bytes left over after the last column"));
	}
	return row;
}

index_key_t Schema::KeyOf(const Row& row) const {
	assert(primary_key_ < row.size() && std::holds_alternative<int64_t>(row[primary_key_]));
	return std::get<int64_t>(row[primary_key_]);
}

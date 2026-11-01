#include "catalog/catalog.hpp"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/types.hpp"

using namespace kernsql;

namespace {

// Fixed part of one table entry: table_id u32 | name length u8 | heap_first i32 | index_header i32
// | primary_key u16 | column_count u16. The name's bytes come on top.
constexpr std::size_t TABLE_ENTRY_FIXED_SIZE = 4 + 1 + 4 + 4 + 2 + 2;

// Fixed part of one column entry: name length u8 | type u8 | max_length u16.
constexpr std::size_t COLUMN_ENTRY_FIXED_SIZE = 1 + 1 + 2;

std::size_t EntrySize(std::string_view name, const Schema& schema) {
	std::size_t size = TABLE_ENTRY_FIXED_SIZE + name.size();
	for (const Column& column : schema.Columns()) {
		size += COLUMN_ENTRY_FIXED_SIZE + column.name.size();
	}
	return size;
}

std::size_t PagesFor(std::size_t stream_length) {
	return (stream_length + PAGE_BODY_SIZE - 1) / PAGE_BODY_SIZE;
}

// Appends fixed-width fields, host-endian by memcpy, as PageHeader does.
class StreamWriter {
  public:
	explicit StreamWriter(std::size_t reserve) { out_.reserve(reserve); }

	template <typename T>
	void Put(T value) {
		const auto* p = reinterpret_cast<const std::byte*>(&value);
		out_.insert(out_.end(), p, p + sizeof(T));
	}

	// Callers have already checked the length against MAX_NAME_LENGTH, which fits a u8.
	void PutName(std::string_view name) {
		Put(static_cast<uint8_t>(name.size()));
		const auto* p = reinterpret_cast<const std::byte*>(name.data());
		out_.insert(out_.end(), p, p + name.size());
	}

	std::vector<std::byte> Take() { return std::move(out_); }

  private:
	std::vector<std::byte> out_;
};

// Reads fields back, refusing to run past the end. Every Get returns nullopt rather than reading
// out of bounds: the bytes come off disk and are not trusted.
class StreamReader {
  public:
	explicit StreamReader(std::span<const std::byte> in) : in_(in) {}

	template <typename T>
	std::optional<T> Get() {
		if (in_.size() - pos_ < sizeof(T)) return std::nullopt;
		T value;
		std::memcpy(&value, in_.data() + pos_, sizeof(T));
		pos_ += sizeof(T);
		return value;
	}

	std::optional<std::string> GetName() {
		auto length = Get<uint8_t>();
		if (!length || in_.size() - pos_ < *length) return std::nullopt;
		std::string name(reinterpret_cast<const char*>(in_.data() + pos_), *length);
		pos_ += *length;
		return name;
	}

	[[nodiscard]] std::size_t Position() const { return pos_; }

  private:
	std::span<const std::byte> in_;
	std::size_t pos_{0};
};

Status Corrupt(const std::string& what) {
	return Status::Corruption("catalog: " + what);
}

bool IsReservedOrInvalid(page_id_t id) {
	return id == INVALID_PAGE || id == META_PAGE_ID || id == CATALOG_ROOT_PAGE_ID;
}

// Everything one table entry says, before anything is opened.
struct TableEntry {
	table_id_t table_id{0};
	std::string name;
	page_id_t heap_first_page_id{INVALID_PAGE};
	page_id_t index_header_page_id{INVALID_PAGE};
	Schema schema;  // no default: always built by ParseEntry
};

Result<TableEntry> ParseEntry(StreamReader& in, uint32_t index) {
	const std::string where = "table entry " + std::to_string(index);
	auto table_id = in.Get<uint32_t>();
	auto name = in.GetName();
	auto heap_first = in.Get<page_id_t>();
	auto index_header = in.Get<page_id_t>();
	auto primary_key = in.Get<uint16_t>();
	auto column_count = in.Get<uint16_t>();
	if (!table_id || !name || !heap_first || !index_header || !primary_key || !column_count) {
		return std::unexpected(Corrupt(where + " runs past stream_length"));
	}

	std::vector<Column> columns;
	columns.reserve(*column_count);
	for (uint16_t c = 0; c < *column_count; ++c) {
		auto column_name = in.GetName();
		auto type = in.Get<uint8_t>();
		auto max_length = in.Get<uint16_t>();
		if (!column_name || !type || !max_length) {
			return std::unexpected(
			    Corrupt(where + ", column " + std::to_string(c) + " runs past stream_length"));
		}
		if (*type != static_cast<uint8_t>(ColumnType::kInt64) &&
		    *type != static_cast<uint8_t>(ColumnType::kVarchar)) {
			return std::unexpected(Corrupt(where + ", column " + std::to_string(c) +
			                               ": unknown type " + std::to_string(*type)));
		}
		columns.push_back(
		    Column{std::move(*column_name), static_cast<ColumnType>(*type), *max_length});
	}

	if (!IsValidName(*name)) {
		return std::unexpected(Corrupt(where + ": invalid table name"));
	}
	if (IsReservedOrInvalid(*heap_first) || IsReservedOrInvalid(*index_header)) {
		return std::unexpected(Corrupt("table " + *name + ": reserved or invalid page id"));
	}
	auto schema = Schema::Create(std::move(columns), *primary_key);
	if (!schema.has_value()) {
		return std::unexpected(
		    Corrupt("table " + *name + ": bad schema: " + std::string(schema.error().message())));
	}
	return TableEntry{*table_id, std::move(*name), *heap_first, *index_header,
	                  std::move(schema.value())};
}

}  // namespace

/*
 * Two phases. First the chain is walked and the stream copied out, so no guard is held while
 * tables open their own pages. Then the stream is parsed and every entry validated, and only then
 * opened.
 */
Result<std::unique_ptr<Catalog>> Catalog::Open(BufferPoolManager& bpm) {
	auto catalog_guard = bpm.FetchPageRead(CATALOG_ROOT_PAGE_ID);
	if (!catalog_guard.has_value()) return std::unexpected(catalog_guard.error());

	auto catalog = std::unique_ptr<Catalog>(new Catalog(bpm));

	auto ct_header = CatalogHeader::ReadFrom(catalog_guard->Body().first<CATALOG_HEADER_SIZE>());
	if (ct_header.format_version == 0) {  // never written: an empty catalog
		if (catalog_guard->Header().next_page_id != INVALID_PAGE) {
			return std::unexpected(Corrupt("an unwritten catalog has a chain"));
		}
		catalog->chain_.push_back(CATALOG_ROOT_PAGE_ID);
		return catalog;
	}

	if (ct_header.format_version != CATALOG_FORMAT_VERSION) {
		return std::unexpected(
		    Corrupt("unknown format_version " + std::to_string(ct_header.format_version)));
	}
	if (ct_header.stream_length < CATALOG_HEADER_SIZE) {
		return std::unexpected(Corrupt("stream_length " + std::to_string(ct_header.stream_length) +
		                               " is shorter than the header"));
	}

	// Walk the WHOLE chain — chain_ must know every page, so CreateTable never allocates a page
	// the chain already has — and copy out the first stream_length bytes. A page id seen twice is
	// a cycle; a page not typed CATALOG is not part of a catalog.
	std::vector<std::byte> stream;
	stream.reserve(ct_header.stream_length);
	std::set<page_id_t> seen;
	page_id_t page_id = CATALOG_ROOT_PAGE_ID;
	std::optional<ReadPageGuard> guard(std::move(catalog_guard.value()));
	while (true) {
		if (guard->Header().page_type != PageType::CATALOG) {
			return std::unexpected(
			    Corrupt("page " + std::to_string(page_id) + " in the chain is not a CATALOG page"));
		}
		seen.insert(page_id);
		catalog->chain_.push_back(page_id);

		const std::size_t wanted =
		    std::min<std::size_t>(PAGE_BODY_SIZE, ct_header.stream_length - stream.size());
		const auto body = guard->Body();
		stream.insert(stream.end(), body.begin(),
		              body.begin() + static_cast<std::ptrdiff_t>(wanted));

		const page_id_t next = guard->Header().next_page_id;
		guard.reset();
		if (next == INVALID_PAGE) break;
		if (IsReservedOrInvalid(next) || seen.contains(next)) {
			return std::unexpected(Corrupt("page " + std::to_string(page_id) + " links to page " +
			                               std::to_string(next)));
		}
		auto next_guard = bpm.FetchPageRead(next);
		if (!next_guard.has_value()) return std::unexpected(next_guard.error());
		guard.emplace(std::move(next_guard.value()));
		page_id = next;
	}
	if (stream.size() != ct_header.stream_length) {
		return std::unexpected(Corrupt("stream_length " + std::to_string(ct_header.stream_length) +
		                               " is longer than the chain"));
	}

	// Parse and validate every entry before opening any table.
	StreamReader in(stream);
	(void)in.Get<CatalogHeader>();
	std::vector<TableEntry> entries;
	std::set<std::string, std::less<>> names;
	std::set<table_id_t> ids;
	for (uint32_t i = 0; i < ct_header.table_count; ++i) {
		auto entry = ParseEntry(in, i);
		if (!entry.has_value()) return std::unexpected(entry.error());
		if (!names.insert(entry->name).second) {
			return std::unexpected(Corrupt("duplicate table name " + entry->name));
		}
		if (entry->table_id >= ct_header.next_table_id || !ids.insert(entry->table_id).second) {
			return std::unexpected(Corrupt("table " + entry->name + ": bad table_id " +
			                               std::to_string(entry->table_id)));
		}
		entries.push_back(std::move(entry.value()));
	}
	if (in.Position() != stream.size()) {
		return std::unexpected(Corrupt(std::to_string(stream.size() - in.Position()) +
		                               " bytes after the last table entry"));
	}

	for (TableEntry& entry : entries) {
		std::string name = entry.name;
		auto table =
		    Table::Open(bpm, entry.table_id, std::move(entry.name), std::move(entry.schema),
		                entry.heap_first_page_id, entry.index_header_page_id);
		if (!table.has_value()) return std::unexpected(table.error());
		catalog->tables_.emplace(std::move(name), std::move(table.value()));
	}
	catalog->next_table_id_ = ct_header.next_table_id;
	return catalog;
}

Catalog::~Catalog() = default;

Result<Table*> Catalog::CreateTable(std::string_view name, Schema schema, uint16_t index_fanout) {
	// 1. Validate and size.
	if (!IsValidName(name)) {
		return std::unexpected(Status::InvalidArgument("invalid table name"));
	}
	if (tables_.contains(name)) {
		return std::unexpected(
		    Status::DuplicateKey("table " + std::string(name) + " already exists"));
	}
	if (next_table_id_ == std::numeric_limits<uint32_t>::max()) {
		return std::unexpected(Status::InvalidArgument("table ids exhausted"));
	}
	const std::size_t stream_length = StreamLengthWith(name, schema);
	if (stream_length > std::numeric_limits<uint32_t>::max()) {
		return std::unexpected(Status::InvalidArgument("catalog too large"));
	}

	// 2. Every page the new stream needs, latched; at most one of them new.
	auto pages = FetchChainForWrite(stream_length);
	if (!pages.has_value()) return std::unexpected(pages.error());

	// 3. The table. All or nothing itself; the only thing to undo here is a new catalog page.
	auto table =
	    Table::Create(bpm_, next_table_id_, std::string(name), std::move(schema), index_fanout);
	if (!table.has_value()) {
		if (pages->size() > chain_.size()) {
			const page_id_t fresh = pages->back().PageId();
			pages->clear();                // DeletePage rejects a pinned page
			(void)bpm_.DeletePage(fresh);  // best effort, as BPlusTree::Create
		}
		return std::unexpected(table.error());
	}

	// 4. Mutate. Nothing below can fail.
	Table* created = table.value().get();
	tables_.emplace(std::string(name), std::move(table.value()));
	++next_table_id_;
	const std::vector<std::byte> stream = Serialize();
	WriteStream(stream, pages.value());
	return created;
}

Result<Table*> Catalog::GetTable(std::string_view name) const {
	auto it = tables_.find(name);
	if (it == tables_.end()) {
		return std::unexpected(Status::NotFound("no table " + std::string(name)));
	}
	return it->second.get();
}

std::vector<std::string> Catalog::TableNames() const {
	std::vector<std::string> names;
	names.reserve(tables_.size());
	for (const auto& [name, table] : tables_) names.push_back(name);
	return names;  // std::map iterates in key order: already sorted
}

std::size_t Catalog::StreamLengthWith(std::string_view name, const Schema& schema) const {
	std::size_t length = CATALOG_HEADER_SIZE + EntrySize(name, schema);
	for (const auto& [table_name, table] : tables_) {
		length += EntrySize(table_name, table->GetSchema());
	}
	return length;
}

/*
 * Entries in map order (by name), not creation order: the order carries no meaning, and Open
 * accepts any. Lengths fit their fields because every name passed IsValidName (<= 64 bytes) and
 * Schema::Create bounds the column count.
 */
std::vector<std::byte> Catalog::Serialize() const {
	std::size_t length = CATALOG_HEADER_SIZE;
	for (const auto& [name, table] : tables_) length += EntrySize(name, table->GetSchema());

	StreamWriter out(length);
	CatalogHeader header;
	header.format_version = CATALOG_FORMAT_VERSION;
	header.stream_length = static_cast<uint32_t>(length);
	header.table_count = static_cast<uint32_t>(tables_.size());
	header.next_table_id = next_table_id_;
	out.Put(header);

	for (const auto& [name, table] : tables_) {
		const Schema& schema = table->GetSchema();
		out.Put(table->Id());
		out.PutName(name);
		out.Put(table->HeapFirstPageId());
		out.Put(table->IndexHeaderPageId());
		out.Put(schema.PrimaryKeyColumn());
		out.Put(static_cast<uint16_t>(schema.Columns().size()));
		for (const Column& column : schema.Columns()) {
			out.PutName(column.name);
			out.Put(static_cast<uint8_t>(column.type));
			out.Put(column.max_length);
		}
	}
	return out.Take();
}

Result<std::vector<WritePageGuard>> Catalog::FetchChainForWrite(std::size_t stream_length) {
	const std::size_t needed = PagesFor(stream_length);
	if (needed > chain_.size() + 1) {
		return std::unexpected(
		    Status::Internal("catalog stream grew by more than a page in one CREATE TABLE"));
	}

	std::vector<WritePageGuard> pages;
	pages.reserve(needed);
	for (std::size_t i = 0; i < std::min(needed, chain_.size()); ++i) {
		auto guard = bpm_.FetchPageWrite(chain_[i]);
		if (!guard.has_value()) return std::unexpected(guard.error());
		pages.push_back(std::move(guard.value()));
	}
	if (needed > chain_.size()) {
		auto fresh = bpm_.NewPage();
		if (!fresh.has_value()) return std::unexpected(fresh.error());
		fresh->SetPageType(PageType::CATALOG);
		fresh->SetNextPageId(INVALID_PAGE);
		pages.push_back(std::move(fresh.value()));
	}
	return pages;
}

void Catalog::WriteStream(std::span<const std::byte> stream, std::vector<WritePageGuard>& pages) {
	for (std::size_t i = 0; i < pages.size(); ++i) {
		const std::size_t offset = i * PAGE_BODY_SIZE;
		const std::size_t count = std::min<std::size_t>(PAGE_BODY_SIZE, stream.size() - offset);
		auto body = pages[i].MutableBody();
		std::memcpy(body.data(), stream.data() + offset, count);
		// Zero the rest, so the bytes past stream_length are never a stale copy of older entries.
		std::fill(body.begin() + static_cast<std::ptrdiff_t>(count), body.end(), std::byte{0});
	}
	if (pages.size() > chain_.size()) {
		const page_id_t fresh = pages.back().PageId();
		pages[pages.size() - 2].SetNextPageId(fresh);
		chain_.push_back(fresh);
	}
}

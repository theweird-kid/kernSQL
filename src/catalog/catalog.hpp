#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "catalog/schema.hpp"
#include "catalog/table.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "indices/b_plus_tree_page.hpp"

namespace kernsql {

// Version of the catalog stream below. 0 is "never written": DiskManager creates page 1 with a
// zeroed body, and that must read as an empty catalog.
inline constexpr uint32_t CATALOG_FORMAT_VERSION = 1;
inline constexpr std::size_t CATALOG_HEADER_SIZE = 16;

/*
 * The catalog stream's fixed header (DD-006, "Catalog storage"), at the start of page 1's body.
 *
 *   format_version  CATALOG_FORMAT_VERSION, or 0 for a catalog never written
 *   stream_length   bytes of stream, THIS HEADER INCLUDED, across the whole chain
 *   table_count     entries that follow
 *   next_table_id   the id the next CREATE TABLE gets; never reused
 *
 * Followed by `table_count` entries, each:
 *
 *   table_id u32 | name | heap_first_page_id i32 | index_header_page_id i32
 *   | primary_key u16 | column_count u16 | column_count x (name | type u8 | max_length u16)
 *
 * where a name is a u8 length and that many bytes. Same memcpy convention as PageHeader.
 */
struct CatalogHeader {
	uint32_t format_version{0};
	uint32_t stream_length{0};
	uint32_t table_count{0};
	uint32_t next_table_id{0};

	static CatalogHeader ReadFrom(std::span<const std::byte, CATALOG_HEADER_SIZE> bytes) {
		CatalogHeader ch;
		std::memcpy(&ch, bytes.data(), sizeof(CatalogHeader));
		return ch;
	}

	void WriteTo(std::span<std::byte, CATALOG_HEADER_SIZE> bytes) const {
		std::memcpy(bytes.data(), this, sizeof(CatalogHeader));
	}
};
static_assert(sizeof(CatalogHeader) == CATALOG_HEADER_SIZE);

/*
 * Every table in the database: the name -> Table map, and its persistence on the CATALOG page
 * chain that starts at page 1.
 *
 * READ ONCE, WRITTEN WHOLE. Open reads the stream, validates all of it and opens every table;
 * after that the catalog pages are touched only by CreateTable, which rewrites the whole stream.
 * Lookups never read a page.
 *
 * OWNS every Table. A Table* handed out stays valid for the Catalog's lifetime: there is no
 * DROP TABLE, so nothing removes one.
 *
 * THREADING: none until stage 5, which adds a reader-writer lock — CreateTable exclusive,
 * lookups shared.
 */
class Catalog {
  public:
	/*
	 * Read the catalog from page 1 and open every table in it.
	 *
	 * A zeroed header (format_version 0) is an empty catalog. Anything else that does not add up
	 * is kCorruption — nothing on disk is trusted: an unknown format_version or column type; a
	 * stream_length shorter than the header or longer than the chain; an entry running past
	 * stream_length; a name failing IsValidName, or a duplicate table name; a schema that
	 * Schema::Create rejects; a reserved page id (META_PAGE_ID, CATALOG_ROOT_PAGE_ID) as a heap
	 * or index page; a table_id >= next_table_id. Errors from opening a table are returned as
	 * they come.
	 */
	[[nodiscard]] static Result<std::unique_ptr<Catalog>> Open(BufferPoolManager& bpm);

	Catalog(const Catalog&) = delete;
	Catalog& operator=(const Catalog&) = delete;
	Catalog(Catalog&&) = delete;
	Catalog& operator=(Catalog&&) = delete;
	~Catalog();

	/*
	 * Create a table and persist it. The returned pointer is owned by the catalog.
	 *
	 * kInvalidArgument if `name` fails IsValidName; kDuplicateKey if a table already has that
	 * name. Both before anything is touched.
	 *
	 * > EVERY FALLIBLE STEP FIRST, THEN MUTATE — as BPlusTree::Insert.
	 *
	 *   1. Validate, and size the new stream (StreamLengthWith).
	 *   2. Fetch every catalog page that stream needs, write-latched; if it has outgrown the
	 *      chain, allocate ONE more page — not yet linked.
	 *   3. Table::Create, which is itself all or nothing.
	 *   4. Insert into the map, Serialize, and write the stream across the pages (linking the new
	 *      one). Nothing here can fail.
	 *
	 * If 3 fails, the page 2 allocated is freed, best effort. Any error leaves the catalog
	 * unchanged in memory and on disk. Durable at Shutdown, as every page is (DD-002).
	 *
	 * `index_fanout` is passed through to Table::Create — tests only.
	 */
	[[nodiscard]] Result<Table*> CreateTable(std::string_view name, Schema schema,
	                                         uint16_t index_fanout = NODE_CAPACITY);

	// The table called `name`, or kNotFound. Exact match: names are stored lowercase.
	[[nodiscard]] Result<Table*> GetTable(std::string_view name) const;

	// Every table name, sorted. For the shell's table listing and for tests.
	[[nodiscard]] std::vector<std::string> TableNames() const;

	[[nodiscard]] std::size_t TableCount() const { return tables_.size(); }

  private:
	explicit Catalog(BufferPoolManager& bpm) : bpm_(bpm) {}

	// The stream length — header included — once a table called `name` with `schema` is added.
	// Computable before that table exists, because page ids are fixed-width: CreateTable needs it
	// at step 2 to fetch the pages, and the table is only created at step 3.
	[[nodiscard]] std::size_t StreamLengthWith(std::string_view name, const Schema& schema) const;

	// The whole stream for the tables in the map, header included. CreateTable calls it at step 4,
	// after inserting the new table, so it never needs a second code path for "plus one more".
	[[nodiscard]] std::vector<std::byte> Serialize() const;

	/*
	 * Write-latch every page of the chain that a stream of `stream_length` bytes needs, in chain
	 * order, plus one newly allocated page — typed CATALOG, not linked — if the chain is too short.
	 * Never more than one: a single CREATE TABLE adds far less than a page of stream.
	 *
	 * The only fallible part of writing. On failure, holds nothing and has allocated nothing.
	 */
	[[nodiscard]] Result<std::vector<WritePageGuard>> FetchChainForWrite(std::size_t stream_length);

	/*
	 * Copy `stream` across `pages`, PAGE_BODY_SIZE bytes per page, and link the last page onto
	 * the chain if it is new. Cannot fail: every page is already latched and typed. Updates
	 * chain_ to match.
	 */
	void WriteStream(std::span<const std::byte> stream, std::vector<WritePageGuard>& pages);

	BufferPoolManager& bpm_;

	// The CATALOG chain, page 1 first. Read at Open, extended by WriteStream; never shrinks.
	std::vector<page_id_t> chain_;

	uint32_t next_table_id_{0};

	// std::less<> so GetTable can look up a string_view without building a std::string.
	std::map<std::string, std::unique_ptr<Table>, std::less<>> tables_;
};

}  // namespace kernsql

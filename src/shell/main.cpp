// A hand-driving REPL for the engine. Not SQL yet — the parser is the next stage — so it exposes
// one command per operation of the layers that exist: tables through the catalog, and the raw
// buffer pool and heap pages underneath for inspection.
//
// Everything starts from Database (DD-006), exactly as the executor will: DiskManager ->
// BufferPoolManager -> Catalog, owned and torn down in that order. Tables are created through
// the catalog, so they come back on the next run; there is deliberately no command that builds a
// bare TableHeap any more — one made outside the catalog is never recorded, so its pages leak, and
// a row written to a table's heap directly bypasses its index.
//
// Two rules from DD-003 shape the structure here, and they are the reason every command
// acquires and drops its guards and iterators inside a single function:
//
//   - a page guard never outlives the function that acquired it, and
//   - a pin never spans a client round-trip.
//
// This REPL *is* the client. A guard held across the prompt would pin a frame for as long as
// the user takes to type, which is exactly the pool exhaustion those rules exist to prevent.
//
// The page commands can corrupt the catalog or a table on purpose (`write 1 ...`). That is what
// they are for; `check` and a restart show what the layers above make of it.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "buffer/pool_stats.hpp"
#include "catalog/catalog.hpp"
#include "catalog/database.hpp"
#include "catalog/schema.hpp"
#include "catalog/table.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "heap/heap_page.hpp"
#include "heap/table_heap.hpp"
#include "indices/b_plus_tree.hpp"

using namespace kernsql;

namespace {

// The guard hands out the body only — the header is not reachable as raw bytes, so there is
// no offset arithmetic to get wrong here. The buffer pool has no opinion about page contents
// (DD-003), so `write`/`read` just treat the body as a NUL-terminated blob.
constexpr std::size_t kBodySize = PAGE_BODY_SIZE;

constexpr index_key_t kMinKey = std::numeric_limits<index_key_t>::min();
constexpr index_key_t kMaxKey = std::numeric_limits<index_key_t>::max();

// ---- Parsing -----------------------------------------------------------------------------------

std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

// Splits the leading whitespace-delimited token off `rest`, advancing it past what was taken.
std::string_view NextToken(std::string_view& rest) {
	rest = Trim(rest);
	const auto end = rest.find_first_of(" \t");
	std::string_view token = rest.substr(0, end);
	rest = (end == std::string_view::npos) ? std::string_view{} : Trim(rest.substr(end));
	return token;
}

template <typename T>
bool ParseInt(std::string_view token, T& out) {
	if (token.empty()) return false;
	const auto* first = token.data();
	const auto* last = token.data() + token.size();
	auto [ptr, ec] = std::from_chars(first, last, out);
	return ec == std::errc{} && ptr == last;
}

// Identifiers are case-insensitive and stored lowercase (DD-006, "Names"). The SQL parser will
// do this; until then the shell does, so `Users` and `users` name the same table.
std::string Lower(std::string_view s) {
	std::string out(s);
	std::ranges::transform(out, out.begin(),
	                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

// Whitespace-separated tokens, where a double-quoted token may contain spaces and \" or \\.
// nullopt on an unterminated quote. The quotes themselves are not part of the token.
std::optional<std::vector<std::string>> Tokenize(std::string_view s) {
	std::vector<std::string> tokens;
	std::size_t i = 0;
	while (true) {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
		if (i == s.size()) return tokens;
		std::string token;
		if (s[i] == '"') {
			++i;
			bool closed = false;
			while (i < s.size()) {
				if (s[i] == '\\' && i + 1 < s.size()) {
					token.push_back(s[i + 1]);
					i += 2;
				} else if (s[i] == '"') {
					++i;
					closed = true;
					break;
				} else {
					token.push_back(s[i++]);
				}
			}
			if (!closed) return std::nullopt;
		} else {
			while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') {
				token.push_back(s[i++]);
			}
		}
		tokens.push_back(std::move(token));
	}
}

// `int`, `int64`, `bigint`, or `varchar(n)`.
std::optional<Column> ParseColumnSpec(std::string_view spec) {
	const auto colon = spec.find(':');
	if (colon == std::string_view::npos) return std::nullopt;
	Column column;
	column.name = Lower(spec.substr(0, colon));
	const std::string type = Lower(spec.substr(colon + 1));
	if (type == "int" || type == "int64" || type == "bigint") {
		column.type = ColumnType::kInt64;
		return column;
	}
	if (type.starts_with("varchar(") && type.ends_with(")")) {
		const std::string_view n =
		    std::string_view(type).substr(8, type.size() - 9);  // between the parentheses
		if (!ParseInt(n, column.max_length)) return std::nullopt;
		column.type = ColumnType::kVarchar;
		return column;
	}
	return std::nullopt;
}

// ---- Output ------------------------------------------------------------------------------------

void PrintStatus(std::string_view what, const Status& st) {
	if (st.ok())
		std::println("{}: ok", what);
	else
		std::println("{}: {}", what, st.message());
}

std::string TypeName(const Column& column) {
	return column.type == ColumnType::kInt64 ? std::string("INT64")
	                                         : std::format("VARCHAR({})", column.max_length);
}

std::string FormatValue(const Value& value) {
	if (const auto* i = std::get_if<int64_t>(&value)) return std::to_string(*i);
	std::string out = "\"";
	for (const char c : std::get<std::string>(value)) {
		if (c == '"' || c == '\\') out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

std::string FormatRow(const Row& row) {
	std::string out = "(";
	for (std::size_t i = 0; i < row.size(); ++i) {
		if (i > 0) out += ", ";
		out += FormatValue(row[i]);
	}
	return out + ")";
}

// ---- Catalog and table commands ----------------------------------------------------------------

Table* LookUp(Catalog& catalog, std::string_view cmd, std::string_view name) {
	if (name.empty()) {
		std::println("{}: expected a table name", cmd);
		return nullptr;
	}
	auto table = catalog.GetTable(Lower(name));
	if (!table) {
		std::println("{}: {}", cmd, table.error().message());
		return nullptr;
	}
	return *table;
}

// create <table> <col>:<type> ... [key=<col>]
// The key defaults to the first column, which must then be INT64 (Schema::Create checks).
void CmdCreate(Catalog& catalog, std::string_view rest) {
	auto tokens = Tokenize(rest);
	if (!tokens || tokens->size() < 2) {
		std::println("create: expected <table> <col>:<type> ... [key=<col>]");
		return;
	}
	const std::string name = Lower((*tokens)[0]);

	std::vector<Column> columns;
	std::optional<std::string> key;
	for (std::size_t i = 1; i < tokens->size(); ++i) {
		const std::string_view token = (*tokens)[i];
		if (token.starts_with("key=")) {
			key = Lower(token.substr(4));
			continue;
		}
		auto column = ParseColumnSpec(token);
		if (!column) {
			std::println("create: bad column '{}' — want name:int or name:varchar(n)", token);
			return;
		}
		columns.push_back(std::move(*column));
	}

	uint16_t primary_key = 0;
	if (key) {
		auto it = std::ranges::find(columns, *key, &Column::name);
		if (it == columns.end()) {
			std::println("create: key column '{}' is not in the table", *key);
			return;
		}
		primary_key = static_cast<uint16_t>(it - columns.begin());
	}

	auto schema = Schema::Create(std::move(columns), primary_key);
	if (!schema) {
		std::println("create: {}", schema.error().message());
		return;
	}
	auto table = catalog.CreateTable(name, std::move(*schema));
	if (!table) {
		std::println("create: {}", table.error().message());
		return;
	}
	std::println("create: table {} (id {}), heap page {}, index header page {}", (*table)->Name(),
	             (*table)->Id(), (*table)->HeapFirstPageId(), (*table)->IndexHeaderPageId());
}

void CmdTables(Catalog& catalog) {
	for (const std::string& name : catalog.TableNames()) {
		auto table = catalog.GetTable(name);
		if (!table) continue;
		std::println("  {:<20} id {:<4} {} columns", name, (*table)->Id(),
		             (*table)->GetSchema().Columns().size());
	}
	std::println("tables: {}", catalog.TableCount());
}

void CmdDescribe(Table& table) {
	const Schema& schema = table.GetSchema();
	std::println("table {} (id {}), heap page {}, index header page {}, max row {} bytes",
	             table.Name(), table.Id(), table.HeapFirstPageId(), table.IndexHeaderPageId(),
	             schema.MaxRowSize());
	for (std::size_t i = 0; i < schema.Columns().size(); ++i) {
		const Column& column = schema.Columns()[i];
		if (i == schema.PrimaryKeyColumn())
			std::println("  {:<20} {:<14} PRIMARY KEY", column.name, TypeName(column));
		else
			std::println("  {:<20} {}", column.name, TypeName(column));
	}
}

// insert <table> <value> ... — one value per column, in schema order. Strings with spaces in
// double quotes. The token is read according to its column's type, so `42` is a string in a
// VARCHAR column.
void CmdInsert(Table& table, const std::vector<std::string>& values) {
	const auto& columns = table.GetSchema().Columns();
	if (values.size() != columns.size()) {
		std::println("insert: {} has {} columns, got {} values", table.Name(), columns.size(),
		             values.size());
		return;
	}
	Row row;
	for (std::size_t i = 0; i < columns.size(); ++i) {
		if (columns[i].type == ColumnType::kInt64) {
			int64_t v{};
			if (!ParseInt(values[i], v)) {
				std::println("insert: column {} is INT64, got '{}'", columns[i].name, values[i]);
				return;
			}
			row.emplace_back(v);
		} else {
			row.emplace_back(values[i]);
		}
	}
	PrintStatus("insert", table.Insert(row));
}

void CmdGet(Table& table, index_key_t key) {
	auto row = table.Get(key);
	if (!row) {
		std::println("get: {}", row.error().message());
		return;
	}
	std::println("{}", FormatRow(*row));
}

// No range: every row in HEAP order, through Table::Scan — the SeqScan path.
// With a range: rows with lo <= key <= hi in KEY order, through the index and RowAt — the
// IndexScan path. Either way the iterator dies inside this function.
void CmdScan(Table& table, std::optional<std::pair<index_key_t, index_key_t>> range) {
	std::size_t rows = 0;
	if (!range) {
		TableIterator it = table.Scan();
		for (;;) {
			auto more = it.Next();
			if (!more) {
				std::println("scan: {}", more.error().message());
				return;
			}
			if (!*more) break;
			auto row = table.GetSchema().Decode(it.Tuple());
			const RID rid = it.Rid();
			if (row)
				std::println("  {{{}, {}}} {}", rid.page_id, rid.slot, FormatRow(*row));
			else
				std::println("  {{{}, {}}} <{}>", rid.page_id, rid.slot, row.error().message());
			++rows;
		}
		std::println("scan: {} rows (heap order)", rows);
		return;
	}

	auto it = table.ScanIndex(range->first, range->second);
	if (!it) {
		std::println("scan: {}", it.error().message());
		return;
	}
	for (;;) {
		auto more = it->Next();
		if (!more) {
			std::println("scan: {}", more.error().message());
			return;
		}
		if (!*more) break;
		// RowAt only reads the heap; the index iterator's leaf guard is on a different page.
		auto row = table.RowAt(it->Rid());
		if (row)
			std::println("  {} {}", it->Key(), FormatRow(*row));
		else
			std::println("  {} <{}>", it->Key(), row.error().message());
		++rows;
	}
	std::println("scan: {} rows (key order)", rows);
}

// One past the largest key, so `fill` never collides. Collect to completion, as everywhere.
std::optional<index_key_t> NextFreeKey(Table& table) {
	auto it = table.ScanIndex(kMinKey, kMaxKey);
	if (!it) return std::nullopt;
	std::optional<index_key_t> largest;
	for (;;) {
		auto more = it->Next();
		if (!more) return std::nullopt;
		if (!*more) break;
		largest = it->Key();
	}
	if (!largest) return 0;
	if (*largest == kMaxKey) return std::nullopt;
	return *largest + 1;
}

// Bulk insert generated rows, to grow the heap past a page and the index past a leaf. The key
// column gets consecutive keys; other INT64 columns get the key too, VARCHARs "r<key>" cut to fit.
void CmdFill(Table& table, int count) {
	auto start = NextFreeKey(table);
	if (!start) {
		std::println("fill: cannot find a free key");
		return;
	}
	const Schema& schema = table.GetSchema();
	for (int i = 0; i < count; ++i) {
		const index_key_t key = *start + i;
		Row row;
		for (const Column& column : schema.Columns()) {
			if (column.type == ColumnType::kInt64) {
				row.emplace_back(key);
			} else {
				std::string s = std::format("r{}", key);
				s.resize(std::min<std::size_t>(s.size(), column.max_length));
				row.emplace_back(std::move(s));
			}
		}
		if (const Status st = table.Insert(row); !st.ok()) {
			std::println("fill: stopped after {} rows: {}", i, st.message());
			return;
		}
	}
	std::println("fill: {} rows, keys {}..{}", count, *start, *start + count - 1);
}

// ---- Page commands -----------------------------------------------------------------------------

void CmdNew(BufferPoolManager& bpm) {
	auto guard = bpm.NewPage();
	if (!guard) {
		std::println("new: {}", guard.error().message());
		return;
	}
	// Report the id and drop the guard immediately. Holding it would be the round-trip pin.
	std::println("new: allocated page {}", guard->PageId());
}

void CmdWrite(BufferPoolManager& bpm, page_id_t page_id, std::string_view text) {
	if (text.size() + 1 > kBodySize) {
		std::println("write: text too long ({} bytes, max {})", text.size(), kBodySize - 1);
		return;
	}

	auto guard = bpm.FetchPageWrite(page_id);
	if (!guard) {
		std::println("write: {}", guard.error().message());
		return;
	}

	auto body = guard->MutableBody();
	std::fill(body.begin(), body.end(), std::byte{0});  // no tail of a previous, longer write
	std::memcpy(body.data(), text.data(), text.size());

	std::println("write: page {} <- {} bytes", page_id, text.size());
	// Guard drops here: dirty_epoch bumped, latch released, pin dropped. Nothing is on disk
	// yet — that needs `flush` or `quit`.
}

void CmdRead(BufferPoolManager& bpm, page_id_t page_id) {
	auto guard = bpm.FetchPageRead(page_id);
	if (!guard) {
		std::println("read: {}", guard.error().message());
		return;
	}

	auto body = guard->Body();
	std::size_t len = 0;
	while (len < body.size() && body[len] != std::byte{0}) ++len;

	std::println("read: page {} (type {}) -> \"{}\"", page_id,
	             static_cast<int>(guard->Header().page_type),
	             std::string_view(reinterpret_cast<const char*>(body.data()), len));
}

// The accounting view. dead_bytes rising while contiguous stays put is a delete; reclaimable
// collapsing back into contiguous is a compaction. CheckInvariants is the real prize — it proves
// every byte of the tuple region belongs to exactly one live tuple or to nobody.
void CmdHeapDump(BufferPoolManager& bpm, page_id_t page_id) {
	auto guard = bpm.FetchPageRead(page_id);
	if (!guard) {
		std::println("hdump: {}", guard.error().message());
		return;
	}
	auto page = AsHeapPage(*guard);
	if (!page) {
		std::println("hdump: {}", page.error().message());
		return;
	}

	const HeapSubHeader sub = page->Header();
	std::println("hdump: page {} next={}", page_id, guard->Header().next_page_id);
	std::println("       slots={} live={} dead_bytes={} tuple_data_start={}", sub.slot_count,
	             sub.live_count, sub.dead_bytes, sub.tuple_data_start);
	std::println("       contiguous={} reclaimable={}", page->Contiguous(), page->Reclaimable());
	std::println("       invariants: {}", page->CheckInvariants() ? "ok" : "BROKEN");
}

void CmdHelp() {
	std::println("tables (through the catalog — they persist):");
	std::println("  create <t> <col>:<type> ... [key=<col>]");
	std::println("                      types: int, varchar(n); key defaults to the first column");
	std::println("  tables              list tables");
	std::println("  describe <t>        columns, key, page ids");
	std::println("  insert <t> <v> ...  one value per column; \"quote strings with spaces\"");
	std::println("  get <t> <key>       one row by primary key");
	std::println("  remove <t> <key>    delete one row by primary key");
	std::println("  scan <t>            every row, heap order (sequential scan)");
	std::println("  scan <t> <lo> <hi>  keys in [lo, hi], key order (index scan)");
	std::println("  fill <t> <n>        insert n generated rows after the largest key");
	std::println("  check <t>           heap <-> index consistency + tree invariants");
	std::println("pages (raw, for inspection — can corrupt things on purpose):");
	std::println("  new                 allocate a page and return its id");
	std::println("  write <page> <text> overwrite the page body");
	std::println("  read <page>         print the page type and body");
	std::println("  delete <page>       free the page");
	std::println("  flush <page>        write one page through to the file");
	std::println("  flushall            write every resident page through");
	std::println("  hdump <page>        heap page accounting + invariant check");
	std::println("  stat                file and pool counters");
	std::println("  help                this list");
	std::println("  quit                shutdown (flush + fsync) and exit");
}

// Returns false when the REPL should stop.
bool Dispatch(Database& db, const std::filesystem::path& path, std::string_view line) {
	BufferPoolManager& bpm = db.Pool();
	Catalog& catalog = db.GetCatalog();

	std::string_view rest = line;
	const std::string_view cmd = NextToken(rest);
	if (cmd.empty()) return true;

	auto needs_page_id = [&](page_id_t& id) {
		if (ParseInt(NextToken(rest), id)) return true;
		std::println("{}: expected a page id", cmd);
		return false;
	};
	auto needs_key = [&](index_key_t& key) {
		if (ParseInt(NextToken(rest), key)) return true;
		std::println("{}: expected an integer key", cmd);
		return false;
	};

	if (cmd == "quit" || cmd == "exit") return false;
	if (cmd == "help") {
		CmdHelp();
	} else if (cmd == "create") {
		CmdCreate(catalog, rest);
	} else if (cmd == "tables") {
		CmdTables(catalog);
	} else if (cmd == "describe") {
		if (Table* t = LookUp(catalog, cmd, NextToken(rest))) CmdDescribe(*t);
	} else if (cmd == "insert") {
		auto tokens = Tokenize(rest);
		if (!tokens || tokens->empty()) {
			std::println("insert: expected <table> <value> ... (check your quotes)");
		} else if (Table* t = LookUp(catalog, cmd, tokens->front())) {
			CmdInsert(*t, std::vector<std::string>(tokens->begin() + 1, tokens->end()));
		}
	} else if (cmd == "get") {
		index_key_t key{};
		if (Table* t = LookUp(catalog, cmd, NextToken(rest)); t && needs_key(key)) CmdGet(*t, key);
	} else if (cmd == "remove") {
		index_key_t key{};
		if (Table* t = LookUp(catalog, cmd, NextToken(rest)); t && needs_key(key))
			PrintStatus("remove", t->Delete(key));
	} else if (cmd == "scan") {
		if (Table* t = LookUp(catalog, cmd, NextToken(rest))) {
			if (Trim(rest).empty()) {
				CmdScan(*t, std::nullopt);
			} else {
				index_key_t lo{};
				index_key_t hi{};
				if (needs_key(lo) && needs_key(hi)) CmdScan(*t, std::pair{lo, hi});
			}
		}
	} else if (cmd == "fill") {
		int count{};
		if (Table* t = LookUp(catalog, cmd, NextToken(rest))) {
			if (ParseInt(NextToken(rest), count) && count > 0)
				CmdFill(*t, count);
			else
				std::println("fill: expected <table> <n>, n > 0");
		}
	} else if (cmd == "check") {
		if (Table* t = LookUp(catalog, cmd, NextToken(rest)))
			std::println("check: {}", t->CheckConsistency() ? "ok" : "INCONSISTENT (see log)");
	} else if (cmd == "new") {
		CmdNew(bpm);
	} else if (cmd == "stat") {
		std::error_code ec;
		const auto bytes = std::filesystem::file_size(path, ec);
		const PoolStats st = bpm.GetStats();
		std::println("stat: {} pages on disk", ec ? 0 : bytes / PAGE_SIZE);
		std::println("      frames {}: {} free / {} resident / {} loading / {} failed", st.capacity,
		             st.free_frames, st.resident_frames, st.loading_frames, st.failed_frames);
		std::println("      {} pinned, {} evictable, free list holds {}", st.pinned_frames,
		             st.evictable, st.free_list_size);
	} else if (cmd == "flushall") {
		PrintStatus("flushall", bpm.FlushAllPages());
	} else if (cmd == "write") {
		page_id_t id{};
		if (needs_page_id(id)) CmdWrite(bpm, id, rest);
	} else if (cmd == "read") {
		page_id_t id{};
		if (needs_page_id(id)) CmdRead(bpm, id);
	} else if (cmd == "delete") {
		page_id_t id{};
		if (needs_page_id(id)) PrintStatus("delete", bpm.DeletePage(id));
	} else if (cmd == "flush") {
		page_id_t id{};
		if (needs_page_id(id)) PrintStatus("flush", bpm.FlushPage(id));
	} else if (cmd == "hdump") {
		page_id_t id{};
		if (needs_page_id(id)) CmdHeapDump(bpm, id);
	} else {
		std::println("unknown command '{}' — try `help`", cmd);
	}
	return true;
}

}  // namespace

int main(int argc, char** argv) {
	const std::filesystem::path db_path =
	    (argc > 1) ? std::filesystem::path(argv[1])
	               : std::filesystem::temp_directory_path() / "kernsql.db";

	auto db = Database::Open(db_path);
	if (!db) {
		std::println(stderr, "cannot open {}: {}", db_path.string(), db.error().message());
		return 1;
	}

	std::println("kernSQL — {} ({} tables)", db_path.string(), (*db)->GetCatalog().TableCount());
	std::println("`help` for commands, `quit` to shut down cleanly.");

	std::string line;
	while (true) {
		std::print("kernsql> ");
		std::cout.flush();
		if (!std::getline(std::cin, line)) {
			// std::println("") rather than std::println(): the zero-argument overload is
			// P3142, a C++26 addition. libc++ 21 has it, libc++ 18 (what CI builds with) does
			// not, and this is the only place we would need it.
			std::println("");  // EOF (ctrl-D) — treat as a clean quit
			break;
		}
		if (!Dispatch(**db, db_path, line)) break;
	}

	// Shutdown() is the durable operation, not the destructor (DD-002). Quiescence is trivially
	// satisfied here — the REPL is single-threaded and every guard and iterator was
	// function-scoped, so no pin outlives the loop. Skipping this would trip the pool
	// destructor's backstop, which logs the mistake and aborts rather than dropping dirty pages.
	Status st = (*db)->Shutdown();
	if (!st.ok()) {
		std::println(stderr, "shutdown failed: {}", st.message());
		return 1;  // the caller decides what a failed flush means — here, a non-zero exit
	}
	std::println("shutdown ok");
	return 0;
}

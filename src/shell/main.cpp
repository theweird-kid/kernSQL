// A hand-driving REPL for the storage engine. Not SQL — it exposes one command per
// BufferPoolManager operation so the layer can be exercised and misused on purpose before
// anything is built on top of it.
//
// Two rules from DD-003 shape the structure here, and they are the reason every command
// acquires and drops its guard inside a single function:
//
//   - a page guard never outlives the function that acquired it, and
//   - a pin never spans a client round-trip.
//
// This REPL *is* the client. A guard held across the prompt would pin a frame for as long as
// the user takes to type, which is exactly the pool exhaustion those rules exist to prevent.

#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "buffer/pool_stats.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "heap/heap_page.hpp"
#include "heap/table_heap.hpp"
#include "storage/disk_manager.hpp"

using namespace kernsql;

namespace {

constexpr std::size_t kPoolFrames = 16;

// The guard hands out the body only — the header is not reachable as raw bytes, so there is
// no offset arithmetic to get wrong here. The buffer pool has no opinion about page contents
// (DD-003), so this shell just treats the body as a NUL-terminated blob.
constexpr std::size_t kBodySize = PAGE_BODY_SIZE;

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

bool ParsePageId(std::string_view token, page_id_t& out) {
	if (token.empty()) return false;
	const auto* first = token.data();
	const auto* last = token.data() + token.size();
	auto [ptr, ec] = std::from_chars(first, last, out);
	return ec == std::errc{} && ptr == last;
}

void PrintStatus(std::string_view what, const Status& st) {
	if (st.ok())
		std::println("{}: ok", what);
	else
		std::println("{}: {}", what, st.message());
}

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

	std::println("read: page {} -> \"{}\"", page_id,
	             std::string_view(reinterpret_cast<const char*>(body.data()), len));
}

bool ParseSlot(std::string_view token, slot_id_t& out) {
	unsigned value{};
	const auto* first = token.data();
	const auto* last = token.data() + token.size();
	auto [ptr, ec] = std::from_chars(first, last, value);
	if (ec != std::errc{} || ptr != last || value > 0xFFFFU) return false;
	out = static_cast<slot_id_t>(value);
	return true;
}

// The heap moves opaque blobs, so the shell picks one encoding and stays out of the way. No NUL
// terminator and no padding: a tuple is exactly the bytes typed, which is what makes the length
// accounting in `tdump` mean something.
std::span<const std::byte> AsBytes(std::string_view text) {
	return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
std::string_view AsText(std::span<const std::byte> bytes) {
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void CmdTableCreate(BufferPoolManager& bpm, std::unique_ptr<TableHeap>& heap) {
	auto created = TableHeap::Create(bpm);
	if (!created) {
		std::println("tcreate: {}", created.error().message());
		return;
	}
	heap = std::move(*created);
	std::println("tcreate: first={} last={}", heap->FirstPageId(), heap->LastPageId());
	std::println("         persist those two ids — the catalog will own them eventually");
}

void CmdTableOpen(BufferPoolManager& bpm, std::unique_ptr<TableHeap>& heap, page_id_t first,
                  page_id_t last) {
	auto opened = TableHeap::Open(bpm, first, last);
	if (!opened) {
		std::println("topen: {}", opened.error().message());
		return;
	}
	heap = std::move(*opened);
	// Open validates nothing on purpose — the first fetch does it through AsHeapPage. A wrong
	// pair of ids here surfaces as kCorruption on the next command, not now.
	std::println("topen: first={} last={}", heap->FirstPageId(), heap->LastPageId());
}

void CmdTableInsert(TableHeap& heap, std::string_view text) {
	auto rid = heap.Insert(AsBytes(text));
	if (!rid) {
		std::println("tinsert: {}", rid.error().message());
		return;
	}
	std::println("tinsert: {} bytes -> {{{}, {}}} (last page now {})", text.size(), rid->page_id,
	             rid->slot, heap.LastPageId());
}

void CmdTableGet(TableHeap& heap, RID rid) {
	auto row = heap.Get(rid);
	if (!row) {
		std::println("tget: {}", row.error().message());
		return;
	}
	std::println("tget: {{{}, {}}} -> \"{}\"", rid.page_id, rid.slot, AsText(*row));
}

void CmdTableUpdate(TableHeap& heap, RID rid, std::string_view text) {
	auto moved = heap.Update(rid, AsBytes(text));
	if (!moved) {
		std::println("tupdate: {}", moved.error().message());
		return;
	}
	// The RID is the interesting output, not the status: a row that outgrew its page was deleted
	// and reinserted, and every index entry or collected RID naming the old one is now stale.
	if (*moved == rid)
		std::println("tupdate: {{{}, {}}} updated in place", rid.page_id, rid.slot);
	else
		std::println("tupdate: {{{}, {}}} RELOCATED to {{{}, {}}}", rid.page_id, rid.slot,
		             moved->page_id, moved->slot);
}

void CmdTableScan(TableHeap& heap) {
	auto it = heap.Scan();
	std::size_t rows = 0;
	for (;;) {
		auto more = it.Next();
		if (!more) {
			std::println("tscan: {}", more.error().message());
			return;
		}
		if (!*more) break;
		const RID rid = it.Rid();
		std::println("  {{{}, {}}} \"{}\"", rid.page_id, rid.slot, AsText(it.Tuple()));
		++rows;
	}
	std::println("tscan: {} rows", rows);
}

// Bulk insert, to push the table past one page and exercise chain extension and the insert hint.
void CmdTableFill(TableHeap& heap, int count, std::string_view prefix) {
	const page_id_t last_before = heap.LastPageId();
	for (int i = 0; i < count; ++i) {
		const std::string row = std::format("{}-{:06d}", prefix, i);
		auto rid = heap.Insert(AsBytes(row));
		if (!rid) {
			std::println("tfill: stopped after {} rows: {}", i, rid.error().message());
			return;
		}
	}
	std::println("tfill: {} rows, last page {} -> {}", count, last_before, heap.LastPageId());
}

/*
 * Delete every row containing `needle` — and the point of the command is the two-phase shape.
 *
 * COLLECT TO COMPLETION, THEN MUTATE. Calling Delete inside the scan loop would have it take a
 * write latch on the page the iterator is still holding a read latch on; the frame latch is a
 * non-recursive shared_mutex, so it would hang on the first match, on one thread, every run. The
 * scan is scoped so the iterator is destroyed — and its guard released — before any delete runs.
 */
void CmdTablePurge(TableHeap& heap, std::string_view needle) {
	std::vector<RID> matched;
	{
		auto it = heap.Scan();
		for (;;) {
			auto more = it.Next();
			if (!more) {
				std::println("tpurge: {}", more.error().message());
				return;
			}
			if (!*more) break;
			if (AsText(it.Tuple()).find(needle) != std::string_view::npos) matched.push_back(it.Rid());
		}
	}

	std::size_t deleted = 0;
	for (const RID& rid : matched) {
		const Status st = heap.Delete(rid);
		if (st.ok())
			++deleted;
		else
			std::println("  {{{}, {}}}: {}", rid.page_id, rid.slot, st.message());
	}
	std::println("tpurge: {} matched, {} deleted", matched.size(), deleted);
}

// The accounting view. dead_bytes rising while contiguous stays put is a delete; reclaimable
// collapsing back into contiguous is a compaction. CheckInvariants is the real prize — it proves
// every byte of the tuple region belongs to exactly one live tuple or to nobody.
void CmdTableDump(BufferPoolManager& bpm, page_id_t page_id) {
	auto guard = bpm.FetchPageRead(page_id);
	if (!guard) {
		std::println("tdump: {}", guard.error().message());
		return;
	}
	auto page = AsHeapPage(*guard);
	if (!page) {
		std::println("tdump: {}", page.error().message());
		return;
	}

	const HeapSubHeader sub = page->Header();
	std::println("tdump: page {} next={}", page_id, guard->Header().next_page_id);
	std::println("       slots={} live={} dead_bytes={} tuple_data_start={}", sub.slot_count,
	             sub.live_count, sub.dead_bytes, sub.tuple_data_start);
	std::println("       contiguous={} reclaimable={}", page->Contiguous(), page->Reclaimable());
	std::println("       invariants: {}", page->CheckInvariants() ? "ok" : "BROKEN");
}

void CmdHelp() {
	std::println("commands:");
	std::println("  new                 allocate a page and return its id");
	std::println("  write <page> <text> overwrite the page body");
	std::println("  read <page>         print the page body");
	std::println("  delete <page>       free the page");
	std::println("  flush <page>        write one page through to the file");
	std::println("  flushall            write every resident page through");
	std::println("  stat                page count on disk");
	std::println("table (heap layer):");
	std::println("  tcreate             new empty table; prints first/last page id");
	std::println("  topen <first> <last>  reopen one from those two ids");
	std::println("  tinsert <text>      insert a row, print its RID");
	std::println("  tget <page> <slot>  read one row");
	std::println("  tupdate <page> <slot> <text>  overwrite; prints the RID it now has");
	std::println("  tdelete <page> <slot>         delete one row");
	std::println("  tscan               walk every live row");
	std::println("  tfill <n> <prefix>  bulk insert, to force chain extension");
	std::println("  tpurge <text>       collect-then-delete every row containing <text>");
	std::println("  tdump <page>        heap page accounting + invariant check");
	std::println("  help                this list");
	std::println("  quit                shutdown (flush + fsync) and exit");
}

// Returns false when the REPL should stop.
bool Dispatch(BufferPoolManager& bpm, DiskManager& dm, std::unique_ptr<TableHeap>& heap,
              std::string_view line) {
	std::string_view rest = line;
	const std::string_view cmd = NextToken(rest);
	if (cmd.empty()) return true;

	auto needs_page_id = [&](page_id_t& id) {
		if (ParsePageId(NextToken(rest), id)) return true;
		std::println("{}: expected a page id", cmd);
		return false;
	};

	auto needs_rid = [&](RID& rid) {
		if (!ParsePageId(NextToken(rest), rid.page_id) || !ParseSlot(NextToken(rest), rid.slot)) {
			std::println("{}: expected <page> <slot>", cmd);
			return false;
		}
		return true;
	};

	auto needs_table = [&]() -> TableHeap* {
		if (heap) return heap.get();
		std::println("{}: no table — run `tcreate` or `topen` first", cmd);
		return nullptr;
	};

	if (cmd == "quit" || cmd == "exit") return false;
	if (cmd == "help") {
		CmdHelp();
	} else if (cmd == "new") {
		CmdNew(bpm);
	} else if (cmd == "stat") {
		const PoolStats st = bpm.GetStats();
		std::println("stat: {} pages on disk", dm.PageCount());
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
	} else if (cmd == "tcreate") {
		CmdTableCreate(bpm, heap);
	} else if (cmd == "topen") {
		page_id_t first{};
		page_id_t last{};
		if (ParsePageId(NextToken(rest), first) && ParsePageId(NextToken(rest), last))
			CmdTableOpen(bpm, heap, first, last);
		else
			std::println("topen: expected <first> <last>");
	} else if (cmd == "tinsert") {
		if (auto* t = needs_table()) CmdTableInsert(*t, rest);
	} else if (cmd == "tget") {
		RID rid{};
		if (auto* t = needs_table(); t && needs_rid(rid)) CmdTableGet(*t, rid);
	} else if (cmd == "tupdate") {
		RID rid{};
		if (auto* t = needs_table(); t && needs_rid(rid)) CmdTableUpdate(*t, rid, rest);
	} else if (cmd == "tdelete") {
		RID rid{};
		if (auto* t = needs_table(); t && needs_rid(rid))
			PrintStatus("tdelete", t->Delete(rid));
	} else if (cmd == "tscan") {
		if (auto* t = needs_table()) CmdTableScan(*t);
	} else if (cmd == "tfill") {
		page_id_t count{};
		if (auto* t = needs_table(); t && ParsePageId(NextToken(rest), count) && count > 0)
			CmdTableFill(*t, count, rest.empty() ? "row" : rest);
		else if (heap)
			std::println("tfill: expected <n> <prefix>, n > 0");
	} else if (cmd == "tpurge") {
		if (auto* t = needs_table()) CmdTablePurge(*t, rest);
	} else if (cmd == "tdump") {
		page_id_t id{};
		if (needs_page_id(id)) CmdTableDump(bpm, id);
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

	auto dm = DiskManager::Open(db_path);
	if (!dm) {
		std::println(stderr, "cannot open {}: {}", db_path.string(), dm.error().message());
		return 1;
	}

	BufferPoolManager bpm(**dm, kPoolFrames);

	std::println("kernSQL — {} ({} pages)", db_path.string(), (*dm)->PageCount());
	std::println("`help` for commands, `quit` to shut down cleanly.");

	// Outlives the loop but is destroyed BEFORE Shutdown() below, which matters: the heap holds
	// no pins of its own, but an iterator inside one of the commands does, and Shutdown() verifies
	// quiescence rather than arranging it.
	std::unique_ptr<TableHeap> heap;

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
		if (!Dispatch(bpm, **dm, heap, line)) break;
	}

	heap.reset();  // before Shutdown(), see above

	// Shutdown() is the durable operation, not the destructor (DD-002). Quiescence is trivially
	// satisfied here — the REPL is single-threaded and every guard was function-scoped, so no
	// pin outlives the loop. Skipping this would trip the destructor's backstop, which logs the
	// mistake and aborts rather than silently dropping dirty pages.
	Status st = bpm.Shutdown();
	if (!st.ok()) {
		std::println(stderr, "shutdown failed: {}", st.message());
		return 1;  // the caller decides what a failed flush means — here, a non-zero exit
	}
	std::println("shutdown ok");
	return 0;
}

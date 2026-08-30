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
#include <iostream>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "buffer/pool_stats.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
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

void CmdHelp() {
	std::println("commands:");
	std::println("  new                 allocate a page and return its id");
	std::println("  write <page> <text> overwrite the page body");
	std::println("  read <page>         print the page body");
	std::println("  delete <page>       free the page");
	std::println("  flush <page>        write one page through to the file");
	std::println("  flushall            write every resident page through");
	std::println("  stat                page count on disk");
	std::println("  help                this list");
	std::println("  quit                shutdown (flush + fsync) and exit");
}

// Returns false when the REPL should stop.
bool Dispatch(BufferPoolManager& bpm, DiskManager& dm, std::string_view line) {
	std::string_view rest = line;
	const std::string_view cmd = NextToken(rest);
	if (cmd.empty()) return true;

	auto needs_page_id = [&](page_id_t& id) {
		if (ParsePageId(NextToken(rest), id)) return true;
		std::println("{}: expected a page id", cmd);
		return false;
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
		if (!Dispatch(bpm, **dm, line)) break;
	}

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

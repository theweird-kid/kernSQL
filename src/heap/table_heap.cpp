#include "table_heap.hpp"

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>

#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "heap/heap_page.hpp"

namespace kernsql {

Result<std::unique_ptr<TableHeap>> TableHeap::Create(BufferPoolManager& bpm) {
	// alloacte first page and stamp as Heap
	auto first_page = bpm.NewPage();
	if (!first_page.has_value()) return std::unexpected(first_page.error());

	// Stamp page as Heap
	first_page.value().SetPageType(PageType::HEAP);
	first_page.value().SetNextPageId(INVALID_PAGE);

	auto heap_page = AsHeapPage(first_page.value());
	if (!heap_page.has_value()) return std::unexpected(heap_page.error());
	heap_page->Init();

	page_id_t page_id = first_page.value().PageId();

	auto table_heap = std::unique_ptr<TableHeap>(new TableHeap(bpm, page_id, page_id));
	return table_heap;
}

Result<std::unique_ptr<TableHeap>> TableHeap::Open(BufferPoolManager& bpm, page_id_t first_page_id,
                                                   page_id_t last_page_id) {
	auto table_heap = std::unique_ptr<TableHeap>(new TableHeap(bpm, first_page_id, last_page_id));
	return table_heap;
}

namespace {

// How many pages the rotating cursor probes before giving up and appending at the last page
// (DD-004). Bounded on purpose: an unbounded sweep over a table with no free space turns one
// insert into a full chain walk that flushes the pool, which is strictly worse than appending.
constexpr int INSERT_PROBE_LIMIT = 4;

// Compact() rewrites the whole tuple region and touches every live slot. Below this much dead
// space the rewrite costs more than the bytes it hands back, so the page is treated as full and
// the probe moves on.
constexpr std::size_t COMPACT_THRESHOLD = PAGE_BODY_SIZE / 8;

/*
 * Try to place `tuple` on an ALREADY-LATCHED heap page.
 *
 * Three outcomes, and keeping them distinct is the whole point: a slot means it landed,
 * std::nullopt means this page cannot take it and the caller should look elsewhere, and an error
 * means something is actually wrong. kPageFull is a routing decision, not a failure — it must
 * never escape TableHeap::Insert as an error.
 *
 * `compact_threshold` is the smallest amount of dead space worth a rewrite. Pass 0 on the append
 * path, where the alternative is allocating a whole new page and any reclaim beats that.
 */
Result<std::optional<slot_id_t>> TryInsertOnPage(HeapPage& page, std::span<const std::byte> tuple,
                                                 std::size_t compact_threshold) {
	// Conservative: HeapPage::Insert pays for a new slot entry unless it finds a dead one to
	// reuse, and which of the two it will do is not knowable from out here.
	const std::size_t need = tuple.size() + SLOT_SIZE;

	if (page.Contiguous() < need) {
		if (page.Reclaimable() < need) return std::nullopt;
		if (page.Header().dead_bytes < compact_threshold) return std::nullopt;
		page.Compact();
	}

	auto slot = page.Insert(tuple);
	if (!slot.has_value()) {
		if (slot.error().code() == ErrorCode::kPageFull) return std::nullopt;
		return std::unexpected(slot.error());
	}
	return slot.value();
}

}  // namespace

Result<RID> TableHeap::Insert(std::span<const std::byte> tuple) {
	// Fail before any I/O. HeapPage::Insert enforces both of these too, but only after a probe
	// has fetched, latched and possibly compacted its way across the chain.
	if (tuple.empty()) {
		return std::unexpected(Status::InvalidArgument("cannot insert a zero-length tuple"));
	}
	if (tuple.size() > MAX_TUPLE_SIZE) {
		return std::unexpected(Status::InvalidArgument("tuple exceeds the maximum tuple size"));
	}

	// --- v1 rotating insert cursor (DD-004) -------------------------------------------------
	//
	// One loop, not two: running off the end of the chain wraps to the front rather than
	// terminating, so the hint's starting position needs no special case.
	page_id_t page_id = insert_hint_.load(std::memory_order_relaxed);
	if (page_id == INVALID_PAGE) page_id = first_page_id_;

	// True only while the page under consideration came from the hint rather than from a
	// next_page_id link. See the tolerance rule below.
	bool from_hint = true;

	for (int probe = 0; probe < INSERT_PROBE_LIMIT && page_id != INVALID_PAGE; ++probe) {
		// A HINT, NEVER A TRUTH. A hint that no longer resolves, or no longer names a heap page,
		// costs one wasted fetch and sends the sweep back to the front of the chain — it must
		// never fail the insert. A page reached through next_page_id gets no such tolerance: a
		// broken chain link is a real error and has to surface.
		const bool hint_recoverable = from_hint && page_id != first_page_id_;

		auto guard = bpm_.FetchPageWrite(page_id);
		if (!guard.has_value()) {
			if (!hint_recoverable) return std::unexpected(guard.error());
			page_id = first_page_id_;
			from_hint = false;
			continue;
		}

		auto heap_page = AsHeapPage(guard.value());
		if (!heap_page.has_value()) {
			if (!hint_recoverable) return std::unexpected(heap_page.error());
			page_id = first_page_id_;
			from_hint = false;
			continue;
		}
		from_hint = false;

		auto slot = TryInsertOnPage(heap_page.value(), tuple, COMPACT_THRESHOLD);
		if (!slot.has_value()) return std::unexpected(slot.error());
		if (slot.value().has_value()) {
			// Leave the cursor on the page that worked: it probably still has room.
			insert_hint_.store(page_id, std::memory_order_relaxed);
			return RID{page_id, slot.value().value()};
		}

		// Full. Advance, wrapping at the end, and publish the cursor so the NEXT insert resumes
		// here instead of re-probing these same pages from the start. That carry-over across
		// calls is what makes the sweep eventually reach every page in the chain.
		page_id_t next = guard.value().Header().next_page_id;
		page_id = (next == INVALID_PAGE) ? first_page_id_ : next;
		insert_hint_.store(page_id, std::memory_order_relaxed);
	}

	// --- append at the last page, extending the chain if it is full --------------------------
	//
	// > THE WRITE LATCH ON THE CURRENT LAST PAGE IS THE EXTENSION LOCK.
	page_id_t last_id = last_page_id_.load();
	for (;;) {
		auto last_guard = bpm_.FetchPageWrite(last_id);
		if (!last_guard.has_value()) return std::unexpected(last_guard.error());

		// Re-read next_page_id UNDER the latch. A thread that extended the chain while we waited
		// for it has already published the link, so follow that instead of allocating a second
		// page over the top of theirs — which would overwrite one link and leak a page,
		// unreachable forever, with no error raised anywhere.
		//
		// Terminates: the chain is finite and only ever appended to, and the guard drops at the
		// continue, so this still holds one latch at a time while it walks.
		page_id_t next = last_guard.value().Header().next_page_id;
		if (next != INVALID_PAGE) {
			last_id = next;
			continue;
		}

		auto last_heap = AsHeapPage(last_guard.value());
		if (!last_heap.has_value()) return std::unexpected(last_heap.error());

		// Try here before allocating: bounded probing may never have reached this page, and the
		// latch is already held. Threshold 0 — any reclaim beats a whole new page.
		auto slot = TryInsertOnPage(last_heap.value(), tuple, 0);
		if (!slot.has_value()) return std::unexpected(slot.error());
		if (slot.value().has_value()) {
			insert_hint_.store(last_id, std::memory_order_relaxed);
			return RID{last_id, slot.value().value()};
		}

		// Genuinely full: extend. The content latch above is held across NewPage, which is
		// allowed — DD-002's prohibitions cover the metadata mutex and the shard locks, not
		// content latches. Lock order is always old -> new, and no other thread can name the new
		// page yet, so there is no cycle.
		auto new_guard = bpm_.NewPage();
		if (!new_guard.has_value()) return std::unexpected(new_guard.error());

		new_guard.value().SetPageType(PageType::HEAP);
		new_guard.value().SetNextPageId(INVALID_PAGE);

		auto new_heap = AsHeapPage(new_guard.value());
		if (!new_heap.has_value()) return std::unexpected(new_heap.error());
		new_heap.value().Init();

		// Cannot fail: MAX_TUPLE_SIZE is chosen so two tuples fit on an empty page, and the
		// tuple was bounds-checked on entry. A failure here is a broken invariant, not a full
		// page, so it propagates rather than routing anywhere.
		auto new_slot = new_heap.value().Insert(tuple);
		if (!new_slot.has_value()) return std::unexpected(new_slot.error());

		const page_id_t new_id = new_guard.value().PageId();

		// Link, then publish. Nothing here spans the two pages atomically — there is no WAL — so
		// a crash between the two writebacks can leave a link to an ALLOCATED page that
		// AsHeapPage will reject on the next scan. Known and accepted (DD-004).
		last_guard.value().SetNextPageId(new_id);
		last_page_id_.store(new_id);
		insert_hint_.store(new_id, std::memory_order_relaxed);

		return RID{new_id, new_slot.value()};
	}
}

Result<std::vector<std::byte>> TableHeap::Get(RID rid) {
	page_id_t page_id = rid.page_id;
	slot_id_t slot_id = rid.slot;

	auto page = bpm_.FetchPageRead(page_id);
	if (!page.has_value()) return std::unexpected(page.error());

	auto heap_page = AsHeapPage(page.value());
	if (!heap_page.has_value()) return std::unexpected(heap_page.error());

	auto tuple = heap_page.value().Get(slot_id);
	if (!tuple.has_value()) return std::unexpected(tuple.error());

	return std::vector<std::byte>(tuple.value().begin(), tuple.value().end());
}

Status TableHeap::Delete(RID rid) {
	page_id_t page_id = rid.page_id;
	slot_id_t slot_id = rid.slot;

	auto page = bpm_.FetchPageWrite(page_id);
	if (!page.has_value()) return page.error();

	auto heap_page = AsHeapPage(page.value());
	if (!heap_page.has_value()) return heap_page.error();

	auto st = heap_page.value().Delete(slot_id);
	if (st.ok()) {
		insert_hint_.store(page_id, std::memory_order_relaxed);
	}
	return st;
}

Result<RID> TableHeap::Update(RID rid, std::span<const std::byte> tuple) {
	page_id_t page_id = rid.page_id;
	slot_id_t slot_id = rid.slot;

	UpdateOutcome outcome{};
	{
		// SCOPED so the guard is gone before the relocation below. Insert and Delete take page
		// latches of their own — including, very likely, this page's, since insert_hint_ usually
		// points at the page a row was just updated or deleted on — and the frame latch is a
		// non-recursive shared_mutex. Holding it across those calls self-deadlocks.
		//
		// It would also make this the one place in the layer that holds two content latches at
		// once, which is what would force a lock-ordering rule onto everything else.
		auto page = bpm_.FetchPageWrite(page_id);
		if (!page.has_value()) return std::unexpected(page.error());

		auto heap_page = AsHeapPage(page.value());
		if (!heap_page.has_value()) return std::unexpected(heap_page.error());

		// Errors propagate unchanged: HeapPage::Update is careful to report an update to a
		// deleted RID as kNotFound rather than kCorruption, and flattening that here would throw
		// away the distinction it went to the trouble of preserving.
		auto updated = heap_page.value().Update(slot_id, tuple);
		if (!updated.has_value()) return std::unexpected(updated.error());
		outcome = updated.value();
	}

	// Cases 1 and 2: the slot index did not change, so the RID survives.
	if (outcome == UpdateOutcome::kSamePage) return rid;

	// kDoesNotFit. The page was left COMPLETELY untouched — case 3 is pure arithmetic run before
	// any mutation — so the old row is still live and the order of the next two steps is a real
	// choice. Insert first: if it fails, the table is unchanged and the caller still has a valid
	// row. Delete first would lose the row outright on a failed insert, and a duplicate is
	// recoverable where a vanished row is not. Postgres and InnoDB order it the same way.
	auto new_rid = Insert(tuple);
	if (!new_rid.has_value()) return std::unexpected(new_rid.error());

	// Past this point there is no way back: the new copy exists and cannot be withdrawn. If the
	// original cannot be removed the row is live in TWO places and every scan returns it twice,
	// so the failure has to be loud rather than swallowed.
	Status removed = Delete(rid);

	// kNotFound is the one benign failure: another thread deleted the original in the window
	// where this function held no latch. The row ends up deleted and reinserted, which is the
	// outcome that was wanted anyway.
	if (!removed.ok() && removed.code() != ErrorCode::kNotFound) {
		return std::unexpected(Status::Internal(
		    "update relocated the row but could not remove the original; it is now duplicated"));
	}

	return new_rid;
}

TableIterator TableHeap::Scan() {
	return TableIterator(bpm_, first_page_id_);
}

}  // namespace kernsql

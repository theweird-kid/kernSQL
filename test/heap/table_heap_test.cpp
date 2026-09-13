#include "heap/table_heap.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "buffer/pool_stats.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "heap/heap_page.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * TableHeap against a real file and a real buffer pool. No fakes: the layer's whole job is to
 * turn page ids into rows, so a test that stubs the pool tests nothing that can break.
 *
 * The assertions to reach for, in rough order of how much they are worth:
 *
 *   1. CheckInvariants() on every touched page after every mutating operation. It proves the
 *      accounting identity — tuple_data_start + sum(live lengths) + dead_bytes == PAGE_BODY_SIZE
 *      — at the operation that broke it rather than a thousand operations later.
 *   2. The bytes, not the status. A round trip that returns ok and the wrong tuple is the bug
 *      this layer is most likely to have.
 *   3. Old RIDs still resolving after a compaction. Slots never move IS the format's contract.
 *   4. Pool quiescence at teardown. A leaked pin is silent until the pool starves.
 */
class TableHeapTest : public ::testing::Test {
  protected:
	// Sixteen rather than the buffer pool suite's four. Insert's extension path holds TWO guards
	// at once (the old last page and the new one), so a concurrency test with N threads can want
	// 2N frames before kBufferPoolFull becomes a legitimate answer rather than a symptom.
	static constexpr std::size_t kFrames = 16;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_table_heap_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name());
		std::filesystem::remove(path_);

		OpenPool();

		auto created = TableHeap::Create(*bpm_);
		ASSERT_TRUE(created.has_value()) << created.error().message();
		heap_ = std::move(created.value());
	}

	void TearDown() override {
		// The heap first: an iterator inside a failed test can still hold a pin, and Shutdown()
		// verifies quiescence rather than arranging it.
		heap_.reset();
		if (bpm_) {
			EXPECT_TRUE(bpm_->Shutdown().ok());
		}
		bpm_.reset();
		dm_.reset();
		std::filesystem::remove(path_);
	}

	void OpenPool() {
		auto dm = DiskManager::Open(path_);
		ASSERT_TRUE(dm.has_value()) << dm.error().message();
		dm_ = std::move(dm.value());
		bpm_ = std::make_unique<BufferPoolManager>(*dm_, kFrames);
	}

	// Close everything and reopen the same file, then reopen the table from the two ids the
	// catalog will one day hold. The only way to prove anything reached disk.
	void Reopen(page_id_t first, page_id_t last) {
		heap_.reset();
		ASSERT_TRUE(bpm_->Shutdown().ok());
		bpm_.reset();
		dm_.reset();

		OpenPool();
		auto opened = TableHeap::Open(*bpm_, first, last);
		ASSERT_TRUE(opened.has_value()) << opened.error().message();
		heap_ = std::move(opened.value());
	}

	static std::span<const std::byte> Bytes(std::string_view s) {
		return std::as_bytes(std::span{s});
	}
	static std::string AsChars(std::span<const std::byte> bytes) {
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	// Distinct, self-identifying, fixed-width. A failure says WHICH row came back wrong instead
	// of only that some bytes differ.
	static std::string Row(int i) { return std::format("row-{:06d}", i); }

	// Insert or fail the test outright — a helper that returns a bad RID just moves the failure
	// somewhere less readable.
	RID MustInsert(std::string_view text) {
		auto rid = heap_->Insert(Bytes(text));
		EXPECT_TRUE(rid.has_value()) << rid.error().message();
		return rid.value_or(RID{});
	}

	std::string MustGet(RID rid) {
		auto row = heap_->Get(rid);
		EXPECT_TRUE(row.has_value()) << row.error().message();
		return row.has_value() ? AsChars(row.value()) : std::string{};
	}

	// Every live row, in scan order. Fails the test on a scan error rather than returning short.
	std::vector<std::pair<RID, std::string>> CollectAll() {
		std::vector<std::pair<RID, std::string>> rows;
		auto it = heap_->Scan();
		for (;;) {
			auto more = it.Next();
			if (!more.has_value()) {
				ADD_FAILURE() << "scan failed: " << more.error().message();
				return rows;
			}
			if (!more.value()) break;
			rows.emplace_back(it.Rid(), AsChars(it.Tuple()));
		}
		return rows;
	}

	// The page ids of the chain, in order, walked through the buffer pool rather than through
	// TableHeap — so a test can assert on the chain the ITERATOR would see even when TableHeap's
	// own idea of it is what is under suspicion.
	std::vector<page_id_t> ChainPages() {
		std::vector<page_id_t> pages;
		for (page_id_t id = heap_->FirstPageId(); id != INVALID_PAGE;) {
			auto guard = bpm_->FetchPageRead(id);
			if (!guard.has_value()) {
				ADD_FAILURE() << "fetch " << id << ": " << guard.error().message();
				return pages;
			}
			pages.push_back(id);
			id = guard.value().Header().next_page_id;
		}
		return pages;
	}

	// A page's accounting, read straight out of the body. The numbers the shell's `tdump` prints.
	HeapSubHeader SubHeaderOf(page_id_t page_id) {
		auto guard = bpm_->FetchPageRead(page_id);
		EXPECT_TRUE(guard.has_value());
		if (!guard.has_value()) return {};
		auto page = AsHeapPage(guard.value());
		EXPECT_TRUE(page.has_value());
		return page.has_value() ? page.value().Header() : HeapSubHeader{};
	}

	std::size_t ContiguousOf(page_id_t page_id) {
		auto guard = bpm_->FetchPageRead(page_id);
		EXPECT_TRUE(guard.has_value());
		if (!guard.has_value()) return 0;
		auto page = AsHeapPage(guard.value());
		EXPECT_TRUE(page.has_value());
		return page.has_value() ? page.value().Contiguous() : 0;
	}

	// Call after every mutating operation. Cheap, and it localises accounting bugs to the
	// operation that caused them.
	void ExpectInvariants(page_id_t page_id) {
		auto guard = bpm_->FetchPageRead(page_id);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		auto page = AsHeapPage(guard.value());
		ASSERT_TRUE(page.has_value()) << page.error().message();
		EXPECT_TRUE(page.value().CheckInvariants()) << "page " << page_id;
	}

	void ExpectAllInvariants() {
		for (page_id_t id : ChainPages()) ExpectInvariants(id);
	}

	// No frame still pinned. A stranded pin is invisible until the pool starves, so it has to be
	// asserted rather than waited for.
	void ExpectNoStrandedPins() {
		const PoolStats stats = bpm_->GetStats();
		EXPECT_EQ(stats.pinned_frames, 0U);
	}

	std::filesystem::path path_;
	std::unique_ptr<DiskManager> dm_;
	std::unique_ptr<BufferPoolManager> bpm_;
	std::unique_ptr<TableHeap> heap_;
};

// =================================================================================================
// Tier 1 — construction, identity, durability
// =================================================================================================

TEST_F(TableHeapTest, CreateStampsHeapAndInitsTheBody) {
	GTEST_SKIP() << "not written yet";
	// The page must come back as page_type == HEAP (AsHeapPage succeeding IS that assertion) and
	// as an INITIALISED empty page, not merely a zeroed one. The field that separates the two is
	// tuple_data_start: zeroed gives 0, initialised gives PAGE_BODY_SIZE. Assert it directly —
	// a zeroed body passes every other check and then underflows on the first insert.
	// Also: slot_count/live_count/dead_bytes all 0, next_page_id == INVALID_PAGE,
	// FirstPageId() == LastPageId(), and CheckInvariants().
}

TEST_F(TableHeapTest, CreateDoesNotTakeAReservedPage) {
	GTEST_SKIP() << "not written yet";
	// FirstPageId() must be neither META_PAGE_ID nor CATALOG_ROOT_PAGE_ID. Cheap, and it pins
	// down an assumption the catalog is about to depend on.
}

TEST_F(TableHeapTest, RowsSurviveAReopen) {
	GTEST_SKIP() << "not written yet";
	// Insert a handful, remember the RIDs and the two page ids, Reopen(first, last), then assert
	// every RID still resolves to the same bytes and a scan returns the same rows in the same
	// order. This is the only test that proves anything reached the file.
}

TEST_F(TableHeapTest, OpenValidatesNothingUntilTheFirstFetch) {
	GTEST_SKIP() << "not written yet";
	// Open() with ids that are not heap pages — META_PAGE_ID will do — must SUCCEED, and the
	// first Get/Scan must then fail with kCorruption from AsHeapPage. Documents the deliberate
	// choice not to read a page in the factory, so nobody "fixes" it later.
}

// =================================================================================================
// Tier 2 — one row's lifecycle
// =================================================================================================

TEST_F(TableHeapTest, InsertThenGetRoundTripsTheBytes) {
	GTEST_SKIP() << "not written yet";
	// Assert the CONTENT, not just that Get succeeded. Include a tuple with embedded NUL bytes:
	// the heap moves opaque blobs, and a length-vs-terminator bug passes every ASCII test.
}

TEST_F(TableHeapTest, InsertRejectsEmptyAndOversizedTuples) {
	GTEST_SKIP() << "not written yet";
	// Both kInvalidArgument, and — the part worth asserting — rejected WITHOUT touching a page:
	// slot_count and dead_bytes on the first page unchanged, so the validation really did happen
	// before the probe rather than at the end of it.
	// Boundary: MAX_TUPLE_SIZE exactly must succeed; MAX_TUPLE_SIZE + 1 must not.
}

TEST_F(TableHeapTest, GetOnADeletedRidIsNotFound) {
	GTEST_SKIP() << "not written yet";
	// Check the CODE is kNotFound, not merely that it failed. A RID handed out before a delete
	// gets an answer, never bytes — and never kCorruption, which would send a reader hunting a
	// nonexistent disk problem.
}

TEST_F(TableHeapTest, GetOnASlotThatNeverExistedIsNotFound) {
	GTEST_SKIP() << "not written yet";
	// RID{first_page_id, 9999}. Same code as a deleted slot: out of range and dead are one
	// answer from the caller's side.
}

TEST_F(TableHeapTest, DeleteIsNotIdempotent) {
	GTEST_SKIP() << "not written yet";
	// Second delete of the same RID is kNotFound, and dead_bytes must NOT move the second time —
	// a double-counted delete inflates dead_bytes and eventually breaks CheckInvariants.
}

// =================================================================================================
// Tier 3 — the chain and the scan
// =================================================================================================

TEST_F(TableHeapTest, InsertExtendsTheChainWhenThePageFills) {
	GTEST_SKIP() << "not written yet";
	// Insert until LastPageId() changes. Then assert: ChainPages() has exactly two entries, the
	// first page's next_page_id names the second, the second's is INVALID_PAGE, LastPageId()
	// equals the second, and EVERY rid collected on the way still resolves. The last part is
	// what catches a chain that grew correctly while losing rows.
}

TEST_F(TableHeapTest, ScanVisitsEveryLiveRowExactlyOnce) {
	GTEST_SKIP() << "not written yet";
	// Fill several pages, then compare CollectAll() against a std::set of what was inserted.
	// EXACTLY once matters as much as the count: put the RIDs in a set and assert no duplicates,
	// because a chain-walk bug that revisits a page shows up as a right-sized wrong answer.
}

TEST_F(TableHeapTest, ScanOnAnEmptyTableTerminatesImmediately) {
	GTEST_SKIP() << "not written yet";
	// A fresh table's first Next() returns false — no error, no special case at the call site.
	// Then a second Next() must ALSO return false rather than walking off the end.
}

TEST_F(TableHeapTest, ScanSkipsAFullyDeletedPage) {
	GTEST_SKIP() << "not written yet";
	// Fill three pages, delete every row on the middle one, and assert the scan returns exactly
	// the other two pages' rows. live_count == 0 is the skip that makes a drained table cheap to
	// scan; this is the test that it actually skips rather than stops.
}

TEST_F(TableHeapTest, IteratorRidNamesTheRowItJustReturned) {
	GTEST_SKIP() << "not written yet";
	// For each step, Get(it.Rid()) must equal it.Tuple(). That pair is what a B+tree build
	// consumes, so a Rid() that lags the cursor by one is a silent index corruption later.
}

TEST_F(TableHeapTest, IteratorTupleSurvivesThePageBoundary) {
	GTEST_SKIP() << "not written yet";
	// Across a page change, the span from the PREVIOUS Next() must still hold the previous row's
	// bytes until the next call replaces them — that is the reason tuple_ is a copy. Worth
	// running under ASan: the failure mode is reading an evicted frame, which is valid memory.
}

TEST_F(TableHeapTest, MovedFromIteratorIsExhausted) {
	GTEST_SKIP() << "not written yet";
	// Move-construct mid-scan; the source's Next() must return false, and the destination must
	// continue from where the source left off — not restart, not skip a row.
}

TEST_F(TableHeapTest, MoveAssignmentReleasesTheTargetsPin) {
	GTEST_SKIP() << "not written yet";
	// Two live iterators, move one onto the other, then ExpectNoStrandedPins() once both die.
	// Overwriting an engaged optional<ReadPageGuard> without dropping it strands a frame for the
	// life of the process, and nothing else in the suite would notice.
}

// =================================================================================================
// Tier 4 — space accounting and reuse
// =================================================================================================

TEST_F(TableHeapTest, DeleteRaisesDeadBytesWithoutFreeingContiguousSpace) {
	GTEST_SKIP() << "not written yet";
	// dead_bytes grows by exactly the tuple length; Contiguous() and tuple_data_start do not
	// move. This is the fact that makes compaction necessary, and asserting it stops anyone
	// "optimising" Delete into something that looks like it frees space.
}

TEST_F(TableHeapTest, InsertCompactsWhenOnlyReclaimableSpaceRemains) {
	GTEST_SKIP() << "not written yet";
	// Fill a page, delete enough scattered rows to clear the compaction threshold, then insert a
	// row that fits only after a rewrite. Assert it landed on THAT page (rid.page_id), that
	// dead_bytes fell to 0, and that Contiguous() grew. The whole free-space story in one test.
}

TEST_F(TableHeapTest, CompactionPreservesEveryExistingRid) {
	GTEST_SKIP() << "not written yet";
	// THE IMPORTANT ONE. Record every {rid, bytes} pair before the compaction above, then after
	// it assert every single rid still resolves to the same bytes. "Slots never move, tuple bytes
	// move freely" is the format's central promise, and compaction is the only place it can
	// break. A failure here invalidates every index that will ever be built on this heap.
}

TEST_F(TableHeapTest, InsertReusesADeadSlotIndex) {
	GTEST_SKIP() << "not written yet";
	// Delete a row from the middle of a page, insert another that fits, and assert the new RID
	// carries the SAME slot index. Then assert slot_count did not grow — the four bytes of the
	// slot entry are what was reclaimed.
	// And the counter-intuitive half: dead_bytes must NOT drop. Reusing a slot reclaims the
	// entry, never the tuple bytes; only Compact does that.
}

TEST_F(TableHeapTest, FreedSpaceIsReusedRatherThanAppended) {
	GTEST_SKIP() << "not written yet";
	// Fill two pages, drain the FIRST one, then insert. The row must land on the reused page and
	// LastPageId() must not move. Without the insert hint plus compact-on-probe the row would be
	// appended and the file would grow forever under churn — the bound DD-004 exists to fix.
}

// =================================================================================================
// Tier 5 — update
// =================================================================================================

TEST_F(TableHeapTest, UpdateInPlaceKeepsTheRid) {
	GTEST_SKIP() << "not written yet";
	// Shrinking and same-size updates both return the original RID and the new bytes read back.
	// For the shrink, assert dead_bytes grew by exactly the difference — those orphaned bytes are
	// referenced by no slot and no offset, so this counter is the only record they exist.
}

TEST_F(TableHeapTest, UpdateThatGrowsWithinThePageKeepsTheRid) {
	GTEST_SKIP() << "not written yet";
	// Growing but still fitting — with and without needing a compaction first. Both are
	// kSamePage, so the RID survives; assert the bytes and CheckInvariants after each.
}

TEST_F(TableHeapTest, UpdateRelocatesWhenThePageCannotHoldTheRow) {
	GTEST_SKIP() << "not written yet";
	// Fill a page, then update one row to something that cannot fit even after compaction.
	// Assert: returned RID differs from the input, the OLD rid is now kNotFound, the new one
	// holds the new bytes, and a scan sees the row EXACTLY ONCE. That last assertion is the one
	// that catches an unchecked delete leaving a duplicate behind.
}

TEST_F(TableHeapTest, UpdateOnADeletedRidIsNotFound) {
	GTEST_SKIP() << "not written yet";
	// kNotFound, not kCorruption and not kInternal — the code HeapPage::Update goes out of its
	// way to preserve, and the one most easily flattened on the way back up.
}

TEST_F(TableHeapTest, UpdateRejectsEmptyAndOversizedTuples) {
	GTEST_SKIP() << "not written yet";
	// kInvalidArgument, and the row must be UNCHANGED afterwards — a rejected update that
	// half-applied is worse than one that fails.
}

// =================================================================================================
// Tier 6 — concurrency. Run these under TSan.
// =================================================================================================

TEST_F(TableHeapTest, ConcurrentInsertsAllLandExactlyOnce) {
	GTEST_SKIP() << "not written yet";
	// N threads × M rows, each row self-identifying by thread and index. Afterwards a scan must
	// return exactly N×M rows with no duplicates and no losses, and every returned RID must be
	// distinct. Collect each thread's RIDs and assert the union has no repeats — two inserts
	// handed the same RID is a different bug from a lost row, and the row count alone cannot
	// tell them apart.
}

TEST_F(TableHeapTest, ConcurrentChainExtensionLeaksNoPage) {
	GTEST_SKIP() << "not written yet";
	// THE RACE THIS LAYER EXISTS TO GET RIGHT. Size the rows so the first page fills exactly as
	// the threads pile in — a std::barrier released right at the boundary is the reliable way.
	//
	// Two threads both seeing next_page_id == INVALID_PAGE both allocate, both write the link,
	// one overwrites the other, and a page is leaked: allocated, unreachable, holding rows that
	// no scan will ever return, with no error raised anywhere.
	//
	// So assert on the FILE, not just the rows: DiskManager::PageCount() must equal
	// ChainPages().size() plus the reserved pages. A row-count assertion alone passes while a
	// page leaks, because the leaked page's rows were never acknowledged to anyone.
}

TEST_F(TableHeapTest, ConcurrentInsertAndDeleteKeepPageAccountingSound) {
	GTEST_SKIP() << "not written yet";
	// Inserters and deleters on one table, then ExpectAllInvariants() at quiescence. dead_bytes,
	// live_count and tuple_data_start are updated by different operations under the same latch;
	// this is the test that the latch really covers all of them together.
}

TEST_F(TableHeapTest, CollectThenMutateDoesNotDeadlock) {
	GTEST_SKIP() << "not written yet";
	// Single-threaded, and it is a REGRESSION TEST FOR A HANG. Scan to completion collecting
	// RIDs, let the iterator die, THEN delete them; assert the survivors.
	//
	// The forbidden shape — deleting inside the loop — takes a write latch on the page the
	// iterator still holds a read latch on. std::shared_mutex is not recursive and does not
	// upgrade, so it blocks forever on the first match: one thread, no race, every run.
	//
	// Keep the ctest TIMEOUT meaningful for this target. A regression here does not fail, it
	// hangs, and a hung test blocks the whole run instead of reporting.
}

}  // namespace kernsql

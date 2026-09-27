#include "heap/table_heap.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
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

	// Row(i) is exactly this long for i < 1'000'000, and every fill below depends on it.
	static constexpr std::size_t kRowSize = 10;

	// How many Row()s one empty heap page holds: each costs its bytes plus a slot entry. 289 for
	// a 4064-byte body. InsertExtendsTheChainWhenThePageFills pins this against the real page, so
	// a format change fails there first rather than quietly skewing every test that fills.
	static constexpr std::size_t kRowsPerPage =
	    (PAGE_BODY_SIZE - HEAP_SUB_HEADER_SIZE) / (kRowSize + SLOT_SIZE);

	// What a page full of Row()s has left over — less than one more row.
	static constexpr std::size_t kFullPageSlack =
	    PAGE_BODY_SIZE - HEAP_SUB_HEADER_SIZE - kRowsPerPage * (kRowSize + SLOT_SIZE);

	// Mirrors COMPACT_THRESHOLD in table_heap.cpp, which is file-local.
	static constexpr std::size_t kCompactThreshold = PAGE_BODY_SIZE / 8;

	// META and the catalog root. Every other page in a fresh file belongs to some chain.
	static constexpr std::size_t kReservedPages = 2;

	using Rows = std::vector<std::pair<RID, std::string>>;
	using RowMap = std::map<RID, std::string>;  // also keeps the comma out of gtest macros

	// Insert Row(first) .. Row(first + count - 1) and return each with its RID. With no deletes in
	// between, rows land in order: row i is on page i / kRowsPerPage of the chain.
	Rows Fill(std::size_t count, int first = 0) {
		Rows rows;
		rows.reserve(count);
		for (std::size_t i = 0; i < count; ++i) {
			std::string text = Row(first + static_cast<int>(i));
			const RID rid = MustInsert(text);
			rows.emplace_back(rid, std::move(text));
		}
		return rows;
	}

	// The error code, or kOk. Lets a test assert WHICH failure it got in one line — the point of
	// most of the tier-2 tests.
	template <typename T>
	static ErrorCode CodeOf(const Result<T>& result) {
		return result.has_value() ? ErrorCode::kOk : result.error().code();
	}
	static ErrorCode CodeOf(const Status& status) { return status.code(); }

	static void ExpectSameAccounting(const HeapSubHeader& before, const HeapSubHeader& after) {
		EXPECT_EQ(after.slot_count, before.slot_count);
		EXPECT_EQ(after.live_count, before.live_count);
		EXPECT_EQ(after.tuple_data_start, before.tuple_data_start);
		EXPECT_EQ(after.dead_bytes, before.dead_bytes);
	}

	// Page one full, page two holding five rows, then every fifth row on page one deleted: 58
	// dead rows, 580 dead bytes — past kCompactThreshold, and scattered so a compaction has to
	// move nearly every surviving tuple. The last delete leaves the insert hint on page one.
	struct ScatteredFirstPage {
		Rows survivors;
		std::vector<RID> deleted;
	};
	ScatteredFirstPage FillThenScatterFirstPage() {
		ScatteredFirstPage out;
		const Rows rows = Fill(kRowsPerPage + 5);
		for (std::size_t i = 0; i < rows.size(); ++i) {
			if (i < kRowsPerPage && i % 5 == 0) {
				EXPECT_TRUE(heap_->Delete(rows[i].first).ok());
				out.deleted.push_back(rows[i].first);
			} else {
				out.survivors.push_back(rows[i]);
			}
		}
		return out;
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
	// The page must come back as page_type == HEAP (AsHeapPage succeeding IS that assertion) and
	// as an INITIALISED empty page, not merely a zeroed one. The field that separates the two is
	// tuple_data_start: zeroed gives 0, initialised gives PAGE_BODY_SIZE. Assert it directly —
	// a zeroed body passes every other check and then underflows on the first insert.
	// Also: slot_count/live_count/dead_bytes all 0, next_page_id == INVALID_PAGE,
	// FirstPageId() == LastPageId(), and CheckInvariants().

	auto first_page = heap_->FirstPageId();
	ASSERT_EQ(first_page, heap_->LastPageId());

	auto const page = bpm_->FetchPageRead(first_page);
	ASSERT_TRUE(page.has_value());
	ASSERT_EQ(PageType::HEAP, page.value().Header().page_type);

	auto heap_page = AsHeapPage(page.value());
	ASSERT_TRUE(heap_page.has_value());

	auto heap_sub_header = heap_page.value().Header();
	ASSERT_EQ(heap_sub_header.tuple_data_start, PAGE_BODY_SIZE);
	ASSERT_EQ(heap_sub_header.dead_bytes, 0);
	ASSERT_EQ(heap_sub_header.live_count, 0);
	ASSERT_EQ(heap_sub_header.slot_count, 0);

	ASSERT_TRUE(heap_page.value().CheckInvariants());
}

TEST_F(TableHeapTest, CreateDoesNotTakeAReservedPage) {
	// FirstPageId() must be neither META_PAGE_ID nor CATALOG_ROOT_PAGE_ID. Cheap, and it pins
	// down an assumption the catalog is about to depend on.

	auto first = heap_->FirstPageId();
	ASSERT_NE(first, META_PAGE_ID);
	ASSERT_NE(first, CATALOG_ROOT_PAGE_ID);
}

TEST_F(TableHeapTest, RowsSurviveAReopen) {
	// Insert a handful, remember the RIDs and the two page ids, Reopen(first, last), then assert
	// every RID still resolves to the same bytes and a scan returns the same rows in the same
	// order. This is the only test that proves anything reached the file.

	// Enough to span three pages at ~289 rows each, so the reopen has to follow next_page_id
	// links and trust a LastPageId() that moved — a single-page table proves neither.
	constexpr int kRows = 700;

	std::vector<RID> rids;
	for (int i = 0; i < kRows; ++i) rids.push_back(MustInsert(Row(i)));

	const auto before = CollectAll();
	ASSERT_EQ(before.size(), static_cast<std::size_t>(kRows));

	const page_id_t first = heap_->FirstPageId();
	const page_id_t last = heap_->LastPageId();
	ASSERT_NE(first, last) << "precondition: the chain must have grown past one page";

	ASSERT_NO_FATAL_FAILURE(Reopen(first, last));

	for (std::size_t i = 0; i < rids.size(); ++i) {
		EXPECT_EQ(MustGet(rids[i]), Row(static_cast<int>(i)))
		    << "rid {" << rids[i].page_id << ", " << rids[i].slot << "}";
	}
	EXPECT_EQ(CollectAll(), before);

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, OpenValidatesNothingUntilTheFirstFetch) {
	// Open() with ids that are not heap pages must SUCCEED, and the first Get/Scan must then fail
	// with kCorruption from AsHeapPage. Documents the deliberate choice not to read a page in the
	// factory, so nobody "fixes" it later.
	//
	// The catalog root, not META: the DiskManager refuses to read page 0 at all, so a META-rooted
	// table fails the FETCH with kInvalidArgument and never reaches AsHeapPage. The catalog root
	// is always present, always readable, and stamped CATALOG — a real page of the wrong type.
	// And Scan(), not only Get(): Get goes straight to rid.page_id and never consults the ids
	// Open() was given, so a Get-only test would pass through any TableHeap at all.
	{
		auto opened = TableHeap::Open(*bpm_, CATALOG_ROOT_PAGE_ID, CATALOG_ROOT_PAGE_ID);
		ASSERT_TRUE(opened.has_value()) << opened.error().message();
		TableHeap& bogus = *opened.value();

		{
			auto it = bogus.Scan();
			auto more = it.Next();
			ASSERT_FALSE(more.has_value()) << "a scan rooted at the catalog page returned rows";
			EXPECT_EQ(more.error().code(), ErrorCode::kCorruption) << more.error().message();
		}

		EXPECT_EQ(CodeOf(bogus.Get(RID{CATALOG_ROOT_PAGE_ID, 0})), ErrorCode::kCorruption);
	}
	ExpectNoStrandedPins();
}

// =================================================================================================
// Tier 2 — one row's lifecycle
// =================================================================================================

TEST_F(TableHeapTest, InsertThenGetRoundTripsTheBytes) {
	// Assert the CONTENT, not just that Get succeeded. Include a tuple with embedded NUL bytes:
	// the heap moves opaque blobs, and a length-vs-terminator bug passes every ASCII test.
	const RID plain = MustInsert("hello, heap");

	constexpr char kRaw[] = "nul\0in\0the\0middle";
	const std::string with_nuls(kRaw, sizeof(kRaw) - 1);  // keep the NULs, drop the terminator
	const RID nuls = MustInsert(with_nuls);

	// Every byte value once, 0x00 through 0xFF — nothing in the path may treat any value as
	// special.
	std::vector<std::byte> every_byte(256);
	for (std::size_t i = 0; i < every_byte.size(); ++i) every_byte[i] = static_cast<std::byte>(i);
	auto binary = heap_->Insert(every_byte);
	ASSERT_TRUE(binary.has_value()) << binary.error().message();

	EXPECT_EQ(MustGet(plain), "hello, heap");
	EXPECT_EQ(MustGet(nuls), with_nuls);
	EXPECT_EQ(MustGet(nuls).size(), with_nuls.size());

	auto back = heap_->Get(binary.value());
	ASSERT_TRUE(back.has_value()) << back.error().message();
	EXPECT_EQ(back.value(), every_byte);

	ExpectInvariants(heap_->FirstPageId());
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, InsertRejectsEmptyAndOversizedTuples) {
	// Both kInvalidArgument, and — the part worth asserting — rejected WITHOUT touching a page:
	// slot_count and dead_bytes on the first page unchanged, so the validation really did happen
	// before the probe rather than at the end of it.
	// Boundary: MAX_TUPLE_SIZE exactly must succeed; MAX_TUPLE_SIZE + 1 must not.
	const page_id_t first = heap_->FirstPageId();
	const HeapSubHeader before = SubHeaderOf(first);

	const std::string too_big(MAX_TUPLE_SIZE + 1, 'x');
	EXPECT_EQ(CodeOf(heap_->Insert(std::span<const std::byte>{})), ErrorCode::kInvalidArgument);
	EXPECT_EQ(CodeOf(heap_->Insert(Bytes(too_big))), ErrorCode::kInvalidArgument);

	ExpectSameAccounting(before, SubHeaderOf(first));
	EXPECT_EQ(heap_->LastPageId(), first);

	// Unchanged accounting shows no page was MODIFIED, not that none was fetched. This proves the
	// stronger claim: a table rooted at the catalog page fails any probe with kCorruption, so
	// getting kInvalidArgument back means the check ran before the first fetch.
	{
		auto opened = TableHeap::Open(*bpm_, CATALOG_ROOT_PAGE_ID, CATALOG_ROOT_PAGE_ID);
		ASSERT_TRUE(opened.has_value());
		EXPECT_EQ(CodeOf(opened.value()->Insert(std::span<const std::byte>{})),
		          ErrorCode::kInvalidArgument);
		EXPECT_EQ(CodeOf(opened.value()->Insert(Bytes(too_big))), ErrorCode::kInvalidArgument);
	}

	const std::string largest(MAX_TUPLE_SIZE, 'm');
	const RID rid = MustInsert(largest);
	EXPECT_EQ(MustGet(rid), largest);

	ExpectInvariants(first);
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, GetOnADeletedRidIsNotFound) {
	// Check the CODE is kNotFound, not merely that it failed. A RID handed out before a delete
	// gets an answer, never bytes — and never kCorruption, which would send a reader hunting a
	// nonexistent disk problem.
	const RID keep = MustInsert(Row(1));
	const RID gone = MustInsert(Row(2));
	ASSERT_TRUE(heap_->Delete(gone).ok());

	EXPECT_EQ(CodeOf(heap_->Get(gone)), ErrorCode::kNotFound);
	EXPECT_EQ(MustGet(keep), Row(1));

	ExpectInvariants(heap_->FirstPageId());
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, GetOnASlotThatNeverExistedIsNotFound) {
	// RID{first_page_id, 9999}. Same code as a deleted slot: out of range and dead are one
	// answer from the caller's side.
	const page_id_t first = heap_->FirstPageId();

	// Slot 0 of an EMPTY page is the off-by-one case: slot_count is 0, so even the first index
	// is out of range.
	EXPECT_EQ(CodeOf(heap_->Get(RID{first, 0})), ErrorCode::kNotFound);

	MustInsert(Row(1));
	EXPECT_EQ(CodeOf(heap_->Get(RID{first, 1})), ErrorCode::kNotFound);  // one past the end
	EXPECT_EQ(CodeOf(heap_->Get(RID{first, 9999})), ErrorCode::kNotFound);

	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, GetOnAnImpossiblePageIdIsInvalidArgument) {
	// A default-constructed RID names INVALID_PAGE, and one is easy to produce by accident — an
	// uninitialised RID in an executor, or MustInsert's own fallback. It must come back as
	// kInvalidArgument, and the failed fetch must leave no pin and no mapping behind.
	//
	// Regression test, 2026-09-27: before FetchFrame rejected these at the front door, both ids
	// took the miss path, claimed a frame, published a mapping, and then tripped
	// `assert(frame.page_id_ >= 1)` in AbandonLoad — aborting any debug build.
	EXPECT_EQ(CodeOf(heap_->Get(RID{})), ErrorCode::kInvalidArgument);
	EXPECT_EQ(CodeOf(heap_->Get(RID{META_PAGE_ID, 0})), ErrorCode::kInvalidArgument);

	ExpectNoStrandedPins();
	const PoolStats stats = bpm_->GetStats();
	EXPECT_EQ(stats.free_frames, stats.free_list_size);
}

TEST_F(TableHeapTest, DeleteIsNotIdempotent) {
	// Second delete of the same RID is kNotFound, and dead_bytes must NOT move the second time —
	// a double-counted delete inflates dead_bytes and eventually breaks CheckInvariants.
	const page_id_t first = heap_->FirstPageId();
	MustInsert(Row(1));
	const RID victim = MustInsert(Row(2));

	ASSERT_TRUE(heap_->Delete(victim).ok());
	const HeapSubHeader after_first = SubHeaderOf(first);
	EXPECT_EQ(after_first.dead_bytes, kRowSize);
	EXPECT_EQ(after_first.live_count, 1);

	EXPECT_EQ(CodeOf(heap_->Delete(victim)), ErrorCode::kNotFound);
	ExpectSameAccounting(after_first, SubHeaderOf(first));

	ExpectInvariants(first);
	ExpectNoStrandedPins();
}

// =================================================================================================
// Tier 3 — the chain and the scan
// =================================================================================================

TEST_F(TableHeapTest, InsertExtendsTheChainWhenThePageFills) {
	// Insert until LastPageId() changes. Then assert: ChainPages() has exactly two entries, the
	// first page's next_page_id names the second, the second's is INVALID_PAGE, LastPageId()
	// equals the second, and EVERY rid collected on the way still resolves. The last part is
	// what catches a chain that grew correctly while losing rows.
	const page_id_t first = heap_->FirstPageId();

	Rows rows;
	for (int i = 0; heap_->LastPageId() == first; ++i) {
		ASSERT_LT(i, 10'000) << "chain never grew";
		const std::string text = Row(i);
		rows.emplace_back(MustInsert(text), text);
	}
	const page_id_t second = heap_->LastPageId();

	// Pins the fixture's arithmetic to the real format: exactly kRowsPerPage fit, the next spilled.
	ASSERT_EQ(rows.size(), kRowsPerPage + 1);
	EXPECT_EQ(ContiguousOf(first), kFullPageSlack);
	EXPECT_EQ(rows.back().first, (RID{second, 0}));

	EXPECT_EQ(ChainPages(), (std::vector<page_id_t>{first, second}));
	{
		auto guard = bpm_->FetchPageRead(first);
		ASSERT_TRUE(guard.has_value());
		EXPECT_EQ(guard.value().Header().next_page_id, second);
	}
	{
		auto guard = bpm_->FetchPageRead(second);
		ASSERT_TRUE(guard.has_value());
		EXPECT_EQ(guard.value().Header().page_type, PageType::HEAP);
		EXPECT_EQ(guard.value().Header().next_page_id, INVALID_PAGE);
	}
	EXPECT_EQ(SubHeaderOf(second).live_count, 1);

	for (const auto& [rid, text] : rows) EXPECT_EQ(MustGet(rid), text);

	// Nothing allocated that the chain does not reach.
	EXPECT_EQ(static_cast<std::size_t>(dm_->PageCount()), kReservedPages + 2);

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, ScanVisitsEveryLiveRowExactlyOnce) {
	// Fill several pages, then compare CollectAll() against a std::set of what was inserted.
	// EXACTLY once matters as much as the count: put the RIDs in a set and assert no duplicates,
	// because a chain-walk bug that revisits a page shows up as a right-sized wrong answer.
	const Rows inserted = Fill(3 * kRowsPerPage + 17);  // three full pages and a partial fourth
	ASSERT_EQ(ChainPages().size(), 4U);

	const Rows scanned = CollectAll();
	ASSERT_EQ(scanned.size(), inserted.size());

	std::set<RID> rids;
	std::set<std::string> texts;
	for (const auto& [rid, text] : scanned) {
		EXPECT_TRUE(rids.insert(rid).second)
		    << "rid {" << rid.page_id << ", " << rid.slot << "} returned twice";
		texts.insert(text);
	}

	// Each scanned row must also carry the RID its insert was given, not just the right bytes.
	const RowMap expected(inserted.begin(), inserted.end());
	EXPECT_EQ(RowMap(scanned.begin(), scanned.end()), expected);
	EXPECT_EQ(texts.size(), inserted.size());

	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, ScanOnAnEmptyTableTerminatesImmediately) {
	// A fresh table's first Next() returns false — no error, no special case at the call site.
	// Then a second Next() must ALSO return false rather than walking off the end.
	auto it = heap_->Scan();

	auto first = it.Next();
	ASSERT_TRUE(first.has_value()) << first.error().message();
	EXPECT_FALSE(first.value());

	auto second = it.Next();
	ASSERT_TRUE(second.has_value()) << second.error().message();
	EXPECT_FALSE(second.value());

	EXPECT_FALSE(it.Rid().isValid());
	EXPECT_TRUE(it.Tuple().empty());

	// An exhausted iterator holds nothing, even while it is still alive.
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, ScanSkipsAFullyDeletedPage) {
	// Fill three pages, delete every row on the middle one, and assert the scan returns exactly
	// the other two pages' rows. live_count == 0 is the skip that makes a drained table cheap to
	// scan; this is the test that it actually skips rather than stops.
	const Rows inserted = Fill(3 * kRowsPerPage);
	const std::vector<page_id_t> chain = ChainPages();
	ASSERT_EQ(chain.size(), 3U);
	const page_id_t middle = chain[1];

	Rows expected;
	for (const auto& row : inserted) {
		if (row.first.page_id == middle) {
			ASSERT_TRUE(heap_->Delete(row.first).ok());
		} else {
			expected.push_back(row);
		}
	}
	ASSERT_EQ(SubHeaderOf(middle).live_count, 0);

	const Rows scanned = CollectAll();
	EXPECT_EQ(scanned, expected);  // same rows, same order — page one's, then page three's
	ASSERT_FALSE(scanned.empty());
	EXPECT_EQ(scanned.back().first.page_id, chain[2]) << "the scan stopped at the empty page";

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, IteratorRidNamesTheRowItJustReturned) {
	// For each step, Get(it.Rid()) must equal it.Tuple(). That pair is what a B+tree build
	// consumes, so a Rid() that lags the cursor by one is a silent index corruption later.
	//
	// Holes on purpose: with no dead slots, a Rid() that lags by one still names a live row, just
	// the wrong one — the holes are what make an off-by-one land on a dead slot and fail loudly.
	const Rows inserted = Fill(2 * kRowsPerPage + 5);
	RowMap live;
	for (std::size_t i = 0; i < inserted.size(); ++i) {
		if (i % 7 == 3) {
			ASSERT_TRUE(heap_->Delete(inserted[i].first).ok());
		} else {
			live.insert(inserted[i]);
		}
	}

	const Rows scanned = CollectAll();  // Rid() and Tuple() captured at the same step
	ASSERT_EQ(scanned.size(), live.size());
	for (const auto& [rid, text] : scanned) {
		EXPECT_EQ(MustGet(rid), text) << "rid {" << rid.page_id << ", " << rid.slot << "}";
		EXPECT_EQ(live.at(rid), text);
	}

	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, IteratorTupleSurvivesThePageBoundary) {
	// Across a page change, the span from the PREVIOUS Next() must still hold the previous row's
	// bytes until the next call replaces them — that is the reason tuple_ is a copy. Worth
	// running under ASan: the failure mode is reading an evicted frame, which is valid memory.
	//
	// That failure is silent by construction, so assert the cause rather than wait for a symptom:
	// at every step, Tuple() must point into the iterator and NOT into the frame holding the page.
	// A view into the frame is exactly what goes stale when the guard drops at the boundary.
	const Rows inserted = Fill(kRowsPerPage + 3);  // last row of page one, then onto page two
	const auto aliases_frame = [&](std::span<const std::byte> tuple, page_id_t page_id) {
		auto guard = bpm_->FetchPageRead(page_id);  // shared latch alongside the iterator's
		EXPECT_TRUE(guard.has_value());
		if (!guard.has_value()) return false;
		const auto body = guard.value().Body();
		const std::less<const std::byte*> before;  // total order on unrelated pointers
		return !before(tuple.data(), body.data()) &&
		       before(tuple.data(), body.data() + body.size());
	};

	auto it = heap_->Scan();
	for (const auto& [rid, text] : inserted) {
		auto more = it.Next();
		ASSERT_TRUE(more.has_value()) << more.error().message();
		ASSERT_TRUE(more.value());
		ASSERT_EQ(it.Rid(), rid);
		EXPECT_EQ(AsChars(it.Tuple()), text);
		EXPECT_FALSE(aliases_frame(it.Tuple(), rid.page_id))
		    << "Tuple() is a view into the frame for rid {" << rid.page_id << ", " << rid.slot
		    << "}";
	}
	ASSERT_NE(inserted[kRowsPerPage - 1].first.page_id, inserted[kRowsPerPage].first.page_id)
	    << "precondition: the scan must have crossed a page boundary";
}

TEST_F(TableHeapTest, MovedFromIteratorIsExhausted) {
	// Move-construct mid-scan; the source's Next() must return false, and the destination must
	// continue from where the source left off — not restart, not skip a row.
	const Rows inserted = Fill(kRowsPerPage + 10);
	constexpr std::size_t kSteps = 5;

	auto source = heap_->Scan();
	for (std::size_t i = 0; i < kSteps; ++i) ASSERT_TRUE(source.Next().value_or(false));

	TableIterator dest(std::move(source));

	// NOLINTBEGIN(bugprone-use-after-move) — the moved-from state IS the thing under test.
	auto from_source = source.Next();
	ASSERT_TRUE(from_source.has_value());
	EXPECT_FALSE(from_source.value());
	EXPECT_FALSE(source.Rid().isValid());
	// NOLINTEND(bugprone-use-after-move)

	// Still on the row the source was on...
	EXPECT_EQ(dest.Rid(), inserted[kSteps - 1].first);
	EXPECT_EQ(AsChars(dest.Tuple()), inserted[kSteps - 1].second);

	// ...and continues from the very next one, all the way across the page boundary.
	for (std::size_t i = kSteps; i < inserted.size(); ++i) {
		auto more = dest.Next();
		ASSERT_TRUE(more.has_value()) << more.error().message();
		ASSERT_TRUE(more.value()) << "ended early at row " << i;
		EXPECT_EQ(dest.Rid(), inserted[i].first);
		EXPECT_EQ(AsChars(dest.Tuple()), inserted[i].second);
	}
	EXPECT_FALSE(dest.Next().value_or(true));
}

TEST_F(TableHeapTest, MoveAssignmentReleasesTheTargetsPin) {
	// Two live iterators, move one onto the other, then ExpectNoStrandedPins() once both die.
	// Overwriting an engaged optional<ReadPageGuard> without dropping it strands a frame for the
	// life of the process, and nothing else in the suite would notice.
	//
	// Parked on DIFFERENT pages, so the stranded pin shows up the moment it happens as a second
	// pinned frame — two pins on one frame would only be caught at the very end.
	const Rows inserted = Fill(kRowsPerPage + 5);
	{
		auto target = heap_->Scan();
		ASSERT_TRUE(target.Next().value_or(false));  // row 0, page one

		auto source = heap_->Scan();
		for (std::size_t i = 0; i <= kRowsPerPage; ++i) ASSERT_TRUE(source.Next().value_or(false));
		ASSERT_EQ(source.Rid(), inserted[kRowsPerPage].first);  // first row of page two
		ASSERT_EQ(bpm_->GetStats().pinned_frames, 2U);

		target = std::move(source);
		EXPECT_EQ(bpm_->GetStats().pinned_frames, 1U) << "page one's pin survived the assignment";

		EXPECT_EQ(target.Rid(), inserted[kRowsPerPage].first);
		ASSERT_TRUE(target.Next().value_or(false));
		EXPECT_EQ(target.Rid(), inserted[kRowsPerPage + 1].first);
	}
	ExpectNoStrandedPins();
}

// =================================================================================================
// Tier 4 — space accounting and reuse
// =================================================================================================

TEST_F(TableHeapTest, DeleteRaisesDeadBytesWithoutFreeingContiguousSpace) {
	// dead_bytes grows by exactly the tuple length; Contiguous() and tuple_data_start do not
	// move. This is the fact that makes compaction necessary, and asserting it stops anyone
	// "optimising" Delete into something that looks like it frees space.
	const page_id_t first = heap_->FirstPageId();
	const Rows rows = Fill(3);

	const HeapSubHeader before = SubHeaderOf(first);
	const std::size_t contiguous_before = ContiguousOf(first);

	ASSERT_TRUE(heap_->Delete(rows[1].first).ok());  // the middle one: not at the low-water mark

	const HeapSubHeader after = SubHeaderOf(first);
	EXPECT_EQ(after.dead_bytes, before.dead_bytes + kRowSize);
	EXPECT_EQ(after.tuple_data_start, before.tuple_data_start);
	EXPECT_EQ(after.slot_count, before.slot_count);
	EXPECT_EQ(after.live_count, before.live_count - 1);
	EXPECT_EQ(ContiguousOf(first), contiguous_before);

	ExpectInvariants(first);
}

TEST_F(TableHeapTest, InsertCompactsWhenOnlyReclaimableSpaceRemains) {
	// Fill a page, delete enough scattered rows to clear the compaction threshold, then insert a
	// row that fits only after a rewrite. Assert it landed on THAT page (rid.page_id), that
	// dead_bytes fell to 0, and that Contiguous() grew. The whole free-space story in one test.
	//
	// Page two has plenty of room. That is what makes rid.page_id meaningful: without
	// compact-on-probe the row would simply land on page two instead.
	const page_id_t first = heap_->FirstPageId();
	const ScatteredFirstPage setup = FillThenScatterFirstPage();
	const page_id_t last = heap_->LastPageId();
	ASSERT_NE(last, first);

	const HeapSubHeader before = SubHeaderOf(first);
	ASSERT_EQ(ContiguousOf(first), kFullPageSlack);
	ASSERT_GE(before.dead_bytes, kCompactThreshold) << "precondition: below the threshold";

	const std::string wide(200, 'c');  // needs a compaction: 200 >> kFullPageSlack
	const RID rid = MustInsert(wide);

	EXPECT_EQ(rid.page_id, first) << "compact-on-probe did not fire; the row went elsewhere";
	EXPECT_EQ(rid.slot, setup.deleted.front().slot) << "expected the first dead slot to be reused";
	EXPECT_EQ(heap_->LastPageId(), last);
	EXPECT_EQ(MustGet(rid), wide);

	// Exact, not just "grew": the compaction handed back every dead byte, the insert took 200 of
	// them, and the reused slot cost no new entry.
	const HeapSubHeader after = SubHeaderOf(first);
	EXPECT_EQ(after.dead_bytes, 0);
	EXPECT_EQ(ContiguousOf(first), kFullPageSlack + before.dead_bytes - wide.size());
	EXPECT_EQ(after.slot_count, before.slot_count);

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, CompactionPreservesEveryExistingRid) {
	// THE IMPORTANT ONE. Record every {rid, bytes} pair before the compaction above, then after
	// it assert every single rid still resolves to the same bytes. "Slots never move, tuple bytes
	// move freely" is the format's central promise, and compaction is the only place it can
	// break. A failure here invalidates every index that will ever be built on this heap.
	const page_id_t first = heap_->FirstPageId();
	const ScatteredFirstPage setup = FillThenScatterFirstPage();

	const RID fresh = MustInsert(std::string(200, 'c'));
	ASSERT_EQ(fresh.page_id, first);
	ASSERT_EQ(SubHeaderOf(first).dead_bytes, 0) << "precondition: the compaction must have run";

	for (const auto& [rid, text] : setup.survivors) {
		EXPECT_EQ(MustGet(rid), text) << "rid {" << rid.page_id << ", " << rid.slot << "}";
	}

	// A deleted RID stays gone — except the one slot the new row just reused, which now names it.
	for (const RID& rid : setup.deleted) {
		if (rid == fresh) continue;
		EXPECT_EQ(CodeOf(heap_->Get(rid)), ErrorCode::kNotFound)
		    << "rid {" << rid.page_id << ", " << rid.slot << "} came back from the dead";
	}

	EXPECT_EQ(CollectAll().size(), setup.survivors.size() + 1);
	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, InsertReusesADeadSlotIndex) {
	// Delete a row from the middle of a page, insert another that fits, and assert the new RID
	// carries the SAME slot index. Then assert slot_count did not grow — the four bytes of the
	// slot entry are what was reclaimed.
	// And the counter-intuitive half: dead_bytes must NOT drop. Reusing a slot reclaims the
	// entry, never the tuple bytes; only Compact does that.
	const page_id_t first = heap_->FirstPageId();
	const Rows rows = Fill(5);
	ASSERT_TRUE(heap_->Delete(rows[2].first).ok());
	const HeapSubHeader before = SubHeaderOf(first);

	const RID reused = MustInsert(Row(100));

	EXPECT_EQ(reused, rows[2].first);
	const HeapSubHeader after = SubHeaderOf(first);
	EXPECT_EQ(after.slot_count, before.slot_count);
	EXPECT_EQ(after.live_count, before.live_count + 1);
	EXPECT_EQ(after.dead_bytes, before.dead_bytes) << "reusing a slot does not reclaim bytes";
	EXPECT_EQ(after.tuple_data_start, before.tuple_data_start - kRowSize);

	EXPECT_EQ(MustGet(reused), Row(100));
	ExpectInvariants(first);
}

TEST_F(TableHeapTest, FreedSpaceIsReusedRatherThanAppended) {
	// Fill two pages, drain the FIRST one, then insert. The row must land on the reused page and
	// LastPageId() must not move. Without the insert hint plus compact-on-probe the row would be
	// appended and the file would grow forever under churn — the bound DD-004 exists to fix.
	const page_id_t first = heap_->FirstPageId();
	const Rows rows = Fill(2 * kRowsPerPage);
	ASSERT_EQ(ChainPages().size(), 2U);
	const page_id_t last = heap_->LastPageId();
	const page_id_t pages_before = dm_->PageCount();

	for (const auto& [rid, text] : rows) {
		if (rid.page_id == first) ASSERT_TRUE(heap_->Delete(rid).ok());
	}

	const RID rid = MustInsert(Row(900'000));

	EXPECT_EQ(rid.page_id, first);
	EXPECT_EQ(heap_->LastPageId(), last);
	EXPECT_EQ(ChainPages().size(), 2U);
	EXPECT_EQ(dm_->PageCount(), pages_before) << "the file grew";
	EXPECT_EQ(MustGet(rid), Row(900'000));

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

// =================================================================================================
// Tier 5 — update
// =================================================================================================

TEST_F(TableHeapTest, UpdateInPlaceKeepsTheRid) {
	// Shrinking and same-size updates both return the original RID and the new bytes read back.
	// For the shrink, assert dead_bytes grew by exactly the difference — those orphaned bytes are
	// referenced by no slot and no offset, so this counter is the only record they exist.
	const page_id_t first = heap_->FirstPageId();
	const RID rid = MustInsert("0123456789abcdef");

	auto same_size = heap_->Update(rid, Bytes("fedcba9876543210"));
	ASSERT_TRUE(same_size.has_value()) << same_size.error().message();
	EXPECT_EQ(same_size.value(), rid);
	EXPECT_EQ(MustGet(rid), "fedcba9876543210");
	EXPECT_EQ(SubHeaderOf(first).dead_bytes, 0);

	auto shrunk = heap_->Update(rid, Bytes("short"));
	ASSERT_TRUE(shrunk.has_value()) << shrunk.error().message();
	EXPECT_EQ(shrunk.value(), rid);
	EXPECT_EQ(MustGet(rid), "short");
	EXPECT_EQ(SubHeaderOf(first).dead_bytes, 16 - 5);

	ExpectInvariants(first);
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, UpdateThatGrowsWithinThePageKeepsTheRid) {
	// Growing but still fitting — with and without needing a compaction first. Both are
	// kSamePage, so the RID survives; assert the bytes and CheckInvariants after each.
	const page_id_t first = heap_->FirstPageId();
	const Rows rows = Fill(kRowsPerPage);  // full: only kFullPageSlack contiguous bytes left
	for (std::size_t i = 10; i < 30; ++i) ASSERT_TRUE(heap_->Delete(rows[i].first).ok());
	ASSERT_EQ(SubHeaderOf(first).dead_bytes, 20 * kRowSize);

	// Slow path: 100 bytes cannot fit in the slack, but does after reclaiming the 200 dead bytes
	// plus the row's own 10. Afterwards: 10 + 200 + 10 - 100 contiguous, nothing dead.
	const std::string grown(100, 'g');
	auto slow = heap_->Update(rows[0].first, Bytes(grown));
	ASSERT_TRUE(slow.has_value()) << slow.error().message();
	EXPECT_EQ(slow.value(), rows[0].first);
	EXPECT_EQ(MustGet(rows[0].first), grown);
	EXPECT_EQ(SubHeaderOf(first).dead_bytes, 0) << "the slow path must have compacted";
	EXPECT_EQ(ContiguousOf(first), kFullPageSlack + 20 * kRowSize + kRowSize - grown.size());
	ExpectInvariants(first);

	// Fast path: 50 bytes now fits at the low-water mark, so the row relocates within the page
	// with no compaction, and its old 10 bytes become dead.
	const std::size_t contiguous = ContiguousOf(first);
	const std::string medium(50, 'm');
	auto fast = heap_->Update(rows[1].first, Bytes(medium));
	ASSERT_TRUE(fast.has_value()) << fast.error().message();
	EXPECT_EQ(fast.value(), rows[1].first);
	EXPECT_EQ(MustGet(rows[1].first), medium);
	EXPECT_EQ(SubHeaderOf(first).dead_bytes, kRowSize);
	EXPECT_EQ(ContiguousOf(first), contiguous - medium.size());
	ExpectInvariants(first);

	// Neighbours untouched by either rewrite.
	for (std::size_t i = 30; i < rows.size(); ++i)
		EXPECT_EQ(MustGet(rows[i].first), rows[i].second);
	EXPECT_EQ(heap_->LastPageId(), first);
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, UpdateRelocatesWhenThePageCannotHoldTheRow) {
	// Fill a page, then update one row to something that cannot fit even after compaction.
	// Assert: returned RID differs from the input, the OLD rid is now kNotFound, the new one
	// holds the new bytes, and a scan sees the row EXACTLY ONCE. That last assertion is the one
	// that catches an unchecked delete leaving a duplicate behind.
	const page_id_t first = heap_->FirstPageId();
	const Rows rows = Fill(kRowsPerPage);  // full, nothing dead: nowhere on this page to grow
	const RID old_rid = rows[0].first;

	const std::string wide(500, 'w');
	auto moved = heap_->Update(old_rid, Bytes(wide));
	ASSERT_TRUE(moved.has_value()) << moved.error().message();
	const RID new_rid = moved.value();

	EXPECT_NE(new_rid, old_rid);
	EXPECT_NE(new_rid.page_id, first);
	EXPECT_EQ(CodeOf(heap_->Get(old_rid)), ErrorCode::kNotFound);
	EXPECT_EQ(MustGet(new_rid), wide);

	const Rows scanned = CollectAll();
	EXPECT_EQ(scanned.size(), rows.size());
	std::size_t wide_seen = 0;
	std::size_t old_seen = 0;
	for (const auto& [rid, text] : scanned) {
		if (text == wide) ++wide_seen;
		if (text == rows[0].second) ++old_seen;
	}
	EXPECT_EQ(wide_seen, 1U);
	EXPECT_EQ(old_seen, 0U) << "the original survived the relocation — the row is duplicated";

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, UpdateOnADeletedRidIsNotFound) {
	// kNotFound, not kCorruption and not kInternal — the code HeapPage::Update goes out of its
	// way to preserve, and the one most easily flattened on the way back up.
	const page_id_t first = heap_->FirstPageId();
	MustInsert(Row(1));
	const RID gone = MustInsert(Row(2));
	ASSERT_TRUE(heap_->Delete(gone).ok());

	EXPECT_EQ(CodeOf(heap_->Update(gone, Bytes(Row(3)))), ErrorCode::kNotFound);
	EXPECT_EQ(CodeOf(heap_->Update(RID{first, 9999}, Bytes(Row(3)))), ErrorCode::kNotFound);

	// And the failed update must not have resurrected or relocated anything.
	EXPECT_EQ(CollectAll().size(), 1U);
	ExpectInvariants(first);
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, UpdateRejectsEmptyAndOversizedTuples) {
	// kInvalidArgument, and the row must be UNCHANGED afterwards — a rejected update that
	// half-applied is worse than one that fails.
	const page_id_t first = heap_->FirstPageId();
	const RID rid = MustInsert(Row(1));
	const HeapSubHeader before = SubHeaderOf(first);

	EXPECT_EQ(CodeOf(heap_->Update(rid, std::span<const std::byte>{})),
	          ErrorCode::kInvalidArgument);
	EXPECT_EQ(CodeOf(heap_->Update(rid, Bytes(std::string(MAX_TUPLE_SIZE + 1, 'x')))),
	          ErrorCode::kInvalidArgument);

	EXPECT_EQ(MustGet(rid), Row(1));
	ExpectSameAccounting(before, SubHeaderOf(first));
	EXPECT_EQ(heap_->LastPageId(), first);
	ExpectNoStrandedPins();
}

// =================================================================================================
// Tier 6 — concurrency. Run these under TSan.
// =================================================================================================

TEST_F(TableHeapTest, ConcurrentInsertsAllLandExactlyOnce) {
	// N threads × M rows, each row self-identifying by thread and index. Afterwards a scan must
	// return exactly N×M rows with no duplicates and no losses, and every returned RID must be
	// distinct. Collect each thread's RIDs and assert the union has no repeats — two inserts
	// handed the same RID is a different bug from a lost row, and the row count alone cannot
	// tell them apart.
	//
	// Eight threads fit the sixteen-frame pool: at most one thread holds two guards (extension is
	// serialised on the last page's latch), so the worst case is N + 1 frames.
	constexpr int kThreads = 8;
	constexpr int kPerThread = 500;  // ~14 pages, so the chain extends under contention

	std::vector<Rows> per_thread(kThreads);
	std::atomic<int> failures{0};
	std::barrier start(kThreads);
	{
		std::vector<std::jthread> threads;
		for (int t = 0; t < kThreads; ++t) {
			threads.emplace_back([&, t] {
				Rows& mine = per_thread[static_cast<std::size_t>(t)];
				start.arrive_and_wait();
				for (int i = 0; i < kPerThread; ++i) {
					std::string text = std::format("t{:02d}-{:06d}", t, i);  // kRowSize bytes
					auto rid = heap_->Insert(Bytes(text));
					if (!rid.has_value()) {
						++failures;
						continue;
					}
					mine.emplace_back(rid.value(), std::move(text));
				}
			});
		}
	}  // joined

	ASSERT_EQ(failures.load(), 0);

	RowMap acknowledged;
	for (const Rows& rows : per_thread) {
		for (const auto& [rid, text] : rows) {
			EXPECT_TRUE(acknowledged.emplace(rid, text).second)
			    << "rid {" << rid.page_id << ", " << rid.slot << "} handed to two inserts";
		}
	}
	ASSERT_EQ(acknowledged.size(), static_cast<std::size_t>(kThreads * kPerThread));

	const Rows scanned = CollectAll();
	EXPECT_EQ(scanned.size(), acknowledged.size());
	EXPECT_EQ(RowMap(scanned.begin(), scanned.end()), acknowledged);

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, ConcurrentChainExtensionLeaksNoPage) {
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
	//
	// Rounds rather than one burst: each round releases every thread at once from the barrier and
	// inserts slightly more than one page's worth between them, so every round crosses at least
	// one page boundary with all threads active — kRounds contended extensions, not one.
	constexpr int kThreads = 8;
	constexpr int kRounds = 40;
	constexpr int kPerRound = static_cast<int>(kRowsPerPage) / kThreads + 1;

	std::vector<Rows> per_thread(kThreads);
	std::atomic<int> failures{0};
	std::barrier round(kThreads);
	{
		std::vector<std::jthread> threads;
		for (int t = 0; t < kThreads; ++t) {
			threads.emplace_back([&, t] {
				Rows& mine = per_thread[static_cast<std::size_t>(t)];
				for (int r = 0; r < kRounds; ++r) {
					round.arrive_and_wait();
					for (int i = 0; i < kPerRound; ++i) {
						std::string text = std::format("{}-{:02d}-{:05d}", t, r, i);  // kRowSize
						auto rid = heap_->Insert(Bytes(text));
						if (!rid.has_value()) {
							++failures;
							continue;
						}
						mine.emplace_back(rid.value(), std::move(text));
					}
				}
			});
		}
	}  // joined

	ASSERT_EQ(failures.load(), 0);

	const std::vector<page_id_t> chain = ChainPages();
	EXPECT_EQ(static_cast<std::size_t>(dm_->PageCount()), chain.size() + kReservedPages)
	    << "a page was allocated that the chain does not reach";
	EXPECT_EQ(chain.back(), heap_->LastPageId());
	EXPECT_EQ(std::set<page_id_t>(chain.begin(), chain.end()).size(), chain.size())
	    << "the chain has a cycle or a repeated page";

	RowMap acknowledged;
	for (const Rows& rows : per_thread) acknowledged.insert(rows.begin(), rows.end());
	ASSERT_EQ(acknowledged.size(), static_cast<std::size_t>(kThreads * kRounds * kPerRound));

	const Rows scanned = CollectAll();
	EXPECT_EQ(RowMap(scanned.begin(), scanned.end()), acknowledged);

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, ConcurrentInsertAndDeleteKeepPageAccountingSound) {
	// Inserters and deleters on one table, then ExpectAllInvariants() at quiescence. dead_bytes,
	// live_count and tuple_data_start are updated by different operations under the same latch;
	// this is the test that the latch really covers all of them together.
	//
	// Deleters take disjoint shares of the even-indexed preloaded rows, so every delete must
	// succeed. Each delete also points the insert hint at its page, so inserters chase freed
	// space and compact-on-probe runs concurrently with deletes on the same pages.
	constexpr std::size_t kPreload = 4 * kRowsPerPage;
	constexpr int kDeleters = 4;
	constexpr int kInserters = 4;
	constexpr int kPerInserter = 400;

	Rows preloaded;
	for (std::size_t i = 0; i < kPreload; ++i) {
		std::string text = std::format("p-{:08d}", i);  // kRowSize bytes
		preloaded.emplace_back(MustInsert(text), std::move(text));
	}

	std::atomic<int> failures{0};
	std::barrier start(kDeleters + kInserters);
	{
		std::vector<std::jthread> threads;
		for (int d = 0; d < kDeleters; ++d) {
			threads.emplace_back([&, d] {
				start.arrive_and_wait();
				for (std::size_t i = 0; i < kPreload; i += 2) {
					if ((i / 2) % static_cast<std::size_t>(kDeleters) !=
					    static_cast<std::size_t>(d))
						continue;
					if (!heap_->Delete(preloaded[i].first).ok()) ++failures;
				}
			});
		}
		for (int w = 0; w < kInserters; ++w) {
			threads.emplace_back([&, w] {
				start.arrive_and_wait();
				for (int i = 0; i < kPerInserter; ++i) {
					auto rid = heap_->Insert(Bytes(std::format("i{}-{:07d}", w, i)));  // kRowSize
					if (!rid.has_value()) ++failures;
				}
			});
		}
	}  // joined

	ASSERT_EQ(failures.load(), 0);
	ExpectAllInvariants();

	// Content, as a multiset so a duplicated row is caught as well as a lost one. RIDs are not
	// compared: deleted slots are reused by the inserters, so a preloaded RID may now name a new
	// row, legitimately.
	std::multiset<std::string> expected;
	for (std::size_t i = 1; i < kPreload; i += 2) expected.insert(preloaded[i].second);
	for (int w = 0; w < kInserters; ++w) {
		for (int i = 0; i < kPerInserter; ++i) expected.insert(std::format("i{}-{:07d}", w, i));
	}

	std::multiset<std::string> actual;
	std::set<RID> rids;
	for (const auto& [rid, text] : CollectAll()) {
		actual.insert(text);
		EXPECT_TRUE(rids.insert(rid).second);
	}
	EXPECT_EQ(actual, expected);
	ExpectNoStrandedPins();
}

TEST_F(TableHeapTest, CollectThenMutateDoesNotDeadlock) {
	// Single-threaded, and it is a REGRESSION TEST FOR A HANG. Scan to completion collecting
	// RIDs, let the iterator die, THEN delete them; assert the survivors.
	//
	// The forbidden shape — deleting inside the loop — takes a write latch on the page the
	// iterator still holds a read latch on. std::shared_mutex is not recursive and does not
	// upgrade, so it blocks forever on the first match: one thread, no race, every run.
	//
	// Keep the ctest TIMEOUT meaningful for this target. A regression here does not fail, it
	// hangs, and a hung test blocks the whole run instead of reporting.
	const Rows inserted = Fill(2 * kRowsPerPage + 50);
	const auto doomed = [](std::string_view text) {
		return std::stoi(std::string(text.substr(4))) % 3 == 0;  // "row-NNNNNN"
	};

	std::vector<RID> matches;
	{
		auto it = heap_->Scan();
		for (;;) {
			auto more = it.Next();
			ASSERT_TRUE(more.has_value()) << more.error().message();
			if (!more.value()) break;
			if (doomed(AsChars(it.Tuple()))) matches.push_back(it.Rid());
		}
	}  // the iterator, and its read latch, die HERE — before the first delete

	ASSERT_FALSE(matches.empty());
	for (const RID& rid : matches) EXPECT_TRUE(heap_->Delete(rid).ok());

	Rows survivors;
	for (const auto& row : inserted) {
		if (!doomed(row.second)) survivors.push_back(row);
	}
	EXPECT_EQ(CollectAll(), survivors);

	ExpectAllInvariants();
	ExpectNoStrandedPins();
}

}  // namespace kernsql

#include "buffer/buffer_pool_manager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer/page_guard.hpp"
#include "buffer/pool_stats.hpp"
#include "common/page_header.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

class BufferPoolManagerTest : public ::testing::Test {
  protected:
	// Four frames, not sixteen. Reclaim, pool exhaustion and dirty eviction all have to be
	// reachable in a handful of operations, or every test spends its first twenty lines
	// manufacturing memory pressure and every failure message requires counting to sixteen.
	static constexpr std::size_t kFrames = 4;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_bpm_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name());
		std::filesystem::remove(path_);  // in case a prior crashed run left it behind

		auto dm = DiskManager::Open(path_);
		ASSERT_TRUE(dm.has_value()) << dm.error().message();
		dm_ = std::move(dm.value());
		bpm_ = std::make_unique<BufferPoolManager>(*dm_, kFrames);
	}

	void TearDown() override {
		// Shutdown() explicitly rather than letting ~BufferPoolManager's backstop run: the
		// backstop aborts on a failed flush, which would take down the whole test binary
		// instead of failing one case. Note it still asserts quiescence internally, so a test
		// that leaks a pin aborts here — AssertQuiesced() below is what turns that into a
		// readable failure at the point it happened.
		if (bpm_) {
			EXPECT_TRUE(bpm_->Shutdown().ok());
		}
		bpm_.reset();  // must die before dm_; it holds a DiskManager&
		dm_.reset();
		std::filesystem::remove(path_);
	}

	// Call at the END of every test. Most frame-accounting bugs are invisible in the result of
	// the operation that caused them and only surface much later as an unexplained
	// BufferPoolFull; this turns them into a failure at the point of the mistake.
	//
	// Only meaningful at quiescence — PoolStats is a sample, not a snapshot (see pool_stats.hpp).
	// In a single-threaded test that is automatic; in a concurrent one, call it after joining.
	void AssertQuiesced(std::string_view where = "") {
		const PoolStats s = bpm_->GetStats();
		SCOPED_TRACE(std::string("AssertQuiesced: ") + std::string(where));

		// The one that catches leaks: a frame in state Free that is not on the free list is
		// unreachable for the rest of the process's life.
		EXPECT_EQ(s.free_frames, s.free_list_size) << "a Free frame is missing from the free list";
		EXPECT_EQ(s.pinned_frames, 0u) << "a guard outlived its scope";
		EXPECT_EQ(s.loading_frames, 0u) << "Loading carries a pin by construction";
		EXPECT_EQ(s.failed_frames, 0u) << "Failed carries a pin by construction";
		EXPECT_EQ(s.free_frames + s.resident_frames, kFrames) << "a frame is in no known state";
		EXPECT_LE(s.evictable, s.resident_frames)
		    << "a non-resident frame is an eviction candidate";
	}

	// Allocates a page through DiskManager and writes `body` into it, bypassing the pool.
	//
	// It reads the page back before writing so the header AllocatePage stamped survives. That
	// is not incidental tidiness: FetchFrame validates page_type and the header's page_id on
	// every miss, so a seeded page whose header was zeroed is rejected as unallocated and the
	// test fails for a reason that has nothing to do with what it meant to check.
	page_id_t SeedPage(std::string_view body) {
		auto id = dm_->AllocatePage();
		EXPECT_TRUE(id.has_value()) << (id.has_value() ? "" : id.error().message());
		if (!id.has_value()) return INVALID_PAGE;

		std::array<std::byte, PAGE_SIZE> page{};
		EXPECT_TRUE(dm_->ReadPage(*id, page).ok());
		EXPECT_LE(body.size(), PAGE_BODY_SIZE);
		std::memcpy(page.data() + PAGE_HEADER_SIZE, body.data(), body.size());
		EXPECT_TRUE(dm_->WritePage(*id, page).ok());
		return *id;
	}

	// What is actually on disk, read straight through DiskManager. Deliberately bypasses the
	// pool: a test that reads back through the pool proves the cache is self-consistent, not
	// that anything was ever written.
	std::string ReadBodyFromDisk(page_id_t page_id) {
		std::array<std::byte, PAGE_SIZE> page{};
		EXPECT_TRUE(dm_->ReadPage(page_id, page).ok());
		const char* body = reinterpret_cast<const char*>(page.data() + PAGE_HEADER_SIZE);
		return std::string(body, ::strnlen(body, PAGE_BODY_SIZE));
	}

	static void WriteBody(WritePageGuard& guard, std::string_view text) {
		auto body = guard.MutableBody();
		std::fill(body.begin(), body.end(), std::byte{0});
		std::memcpy(body.data(), text.data(), text.size());
	}

	// std::byte is a distinct type on purpose: no implicit conversion to char or to an
	// integer, so "raw memory" can never be silently treated as text. That also means nothing
	// in the standard library will compare a span<const std::byte> -- or a std::byte* -- to a
	// string, and gtest's failure for that is a hundred lines of candidate overloads hiding one
	// real error line. Convert explicitly, here, once.
	//
	// Unlike ReadBody below this does NOT stop at a NUL, so it is what you want for comparing
	// an exact-length prefix.
	static std::string_view AsChars(std::span<const std::byte> bytes) {
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	static std::string ReadBody(std::span<const std::byte, PAGE_BODY_SIZE> body) {
		const char* p = reinterpret_cast<const char*>(body.data());
		return std::string(p, ::strnlen(p, PAGE_BODY_SIZE));
	}

	// Pins every frame in the pool by fetching kFrames distinct pages and keeping the guards
	// alive. Returns them so the caller controls when the pressure is released.
	std::vector<ReadPageGuard> FillPool() {
		std::vector<ReadPageGuard> guards;
		for (std::size_t i = 0; i < kFrames; ++i) {
			const page_id_t id = SeedPage("filler");
			auto g = bpm_->FetchPageRead(id);
			EXPECT_TRUE(g.has_value()) << (g.has_value() ? "" : g.error().message());
			if (g.has_value()) guards.push_back(std::move(g.value()));
		}
		return guards;
	}

	std::filesystem::path path_;
	std::unique_ptr<DiskManager> dm_;
	std::unique_ptr<BufferPoolManager> bpm_;  // declared after dm_, so destroyed before it
};

// --- Three starters, each with a known-failing history, so you can confirm the harness is
// --- actually exercising what it looks like it is.

TEST_F(BufferPoolManagerTest, FreshPoolIsFullyStocked) {
	const PoolStats s = bpm_->GetStats();
	EXPECT_EQ(s.capacity, kFrames);
	EXPECT_EQ(s.free_frames, kFrames);
	EXPECT_EQ(s.free_list_size, kFrames);
	EXPECT_EQ(s.resident_frames, 0u);
	EXPECT_EQ(s.evictable, 0u);
	AssertQuiesced("fresh pool");
}

TEST_F(BufferPoolManagerTest, WrittenPageReachesDiskOnFlush) {
	const page_id_t id = SeedPage("");

	{
		auto guard = bpm_->FetchPageWrite(id);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		WriteBody(guard.value(), "durable");
	}  // guard drops: dirty_epoch bumped, latch released, pin dropped

	EXPECT_EQ(ReadBodyFromDisk(id), "") << "nothing should reach disk before a flush";
	ASSERT_TRUE(bpm_->FlushPage(id).ok());
	EXPECT_EQ(ReadBodyFromDisk(id), "durable");

	AssertQuiesced("after flush");
}

TEST_F(BufferPoolManagerTest, FetchAfterDeleteIsRejected) {
	const page_id_t id = SeedPage("henlo");

	{
		auto guard = bpm_->FetchPageRead(id);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		EXPECT_EQ(ReadBody(guard->Body()), "henlo");
	}

	ASSERT_TRUE(bpm_->DeletePage(id).ok());

	// The bytes are still on disk — DeallocatePage only stamps the header FREE — so without
	// the miss-path check this silently returns "henlo" and re-caches a deallocated page.
	auto again = bpm_->FetchPageRead(id);
	EXPECT_FALSE(again.has_value()) << "a deleted page must not be fetchable";

	AssertQuiesced("after rejected fetch");
}

// --- Tier 1: deterministic, single-threaded. Most bugs get caught here. ---

// Seed a page with known bytes through DiskManager, fetch it, compare. The base case
// everything else assumes.
TEST_F(BufferPoolManagerTest, FetchedBytesMatchWhatIsOnDisk) {
	const std::string test_data = "verify bytes read from buffer pool";
	const page_id_t page_id = SeedPage(test_data);

	{
		auto page = bpm_->FetchPageRead(page_id);
		ASSERT_TRUE(page.has_value()) << page.error().message();
		EXPECT_EQ(AsChars(page->Body().subspan(0, test_data.size())), test_data);
	}  // scoped so the guard is released before the pool census below

	AssertQuiesced("after fetch");
}

// Pin all kFrames frames with live guards, then fetch a page that is not resident. Must fail
// with kBufferPoolFull -- and AssertQuiesced afterwards, because the interesting bug is not
// the error, it is a frame lost on the way to producing it.
TEST_F(BufferPoolManagerTest, ExhaustedPoolReturnsBufferPoolFull) {
	auto pages = FillPool();
	auto new_page = bpm_->NewPage();
	ASSERT_FALSE(new_page.has_value());
	ASSERT_EQ(new_page.error().code(), ErrorCode::kBufferPoolFull);
}

// Same setup, then drop one guard. The next fetch must succeed -- proving ReclaimFrame found
// the now-unpinned frame through the replacer rather than the free list.
TEST_F(BufferPoolManagerTest, ReleasingAGuardMakesAFrameReclaimable) {
	auto pages = FillPool();
	pages.pop_back();
	auto new_page = bpm_->NewPage();
	ASSERT_TRUE(new_page.has_value());
}

// Write a page, drop the guard, then force it out by fetching enough other pages. Its bytes
// must be on disk afterwards, read straight through DiskManager. This is the only test that
// exercises the dirty path of ReclaimFrame, which is the one that spans I/O.
TEST_F(BufferPoolManagerTest, DirtyVictimIsWrittenBackWhenEvicted) {
	GTEST_SKIP() << "TODO";
}

// The body must be all zeroes and the header must carry this page's own id with page_type
// ALLOCATED. The header half matters: NewPage is the one path that creates page contents
// without reading them, so a missing stamp is only discovered on a later miss.
TEST_F(BufferPoolManagerTest, NewPageIsZeroedAndItsHeaderIsStamped) {
	GTEST_SKIP() << "TODO";
}

// NewPage with every frame pinned. It acquires a frame BEFORE allocating a page id, so the
// failure path has nothing to unwind -- but assert the page count on disk did not grow, and
// AssertQuiesced.
TEST_F(BufferPoolManagerTest, NewPageOnFullPoolLeaksNothing) {
	GTEST_SKIP() << "TODO";
}

// DeletePage while a guard is alive must be refused, and refusal must change nothing: the
// page still fetchable, the frame still resident.
TEST_F(BufferPoolManagerTest, DeletePageOnAPinnedPageIsRejected) {
	GTEST_SKIP() << "TODO";
}

// After deleting, free_frames goes up by one and evictable goes DOWN by one. The second half
// is the assertion that matters -- DeletePage is the only place that pulls a frame out of the
// candidate set by hand, and forgetting it puts the frame in the free list and the replacer
// at once, so two threads can be handed the same frame.
TEST_F(BufferPoolManagerTest, DeletePageReturnsTheFrameAndLeavesTheCandidateSet) {
	GTEST_SKIP() << "TODO";
}

// Dirty several pages, call FlushAllPages once, verify every one of them on disk.
TEST_F(BufferPoolManagerTest, FlushAllPagesWritesEveryDirtyPage) {
	GTEST_SKIP() << "TODO";
}

// Two Shutdown() calls in a row: the second is a no-op returning OK, not a second flush and
// not an error.
TEST_F(BufferPoolManagerTest, ShutdownIsIdempotent) {
	GTEST_SKIP() << "TODO";
}

// Write through the pool, Shutdown, destroy both manager and DiskManager, reopen the same
// path, read back. The end-to-end durability claim -- and the only test that would catch
// Shutdown flushing but never fsyncing.
TEST_F(BufferPoolManagerTest, DataSurvivesShutdownAndReopen) {
	GTEST_SKIP() << "TODO";
}

// Fetching a page id past the end of the file must fail cleanly. The real assertion is
// AssertQuiesced: the frame acquired for the load has to come back.
TEST_F(BufferPoolManagerTest, FetchPastEndOfFileLeaksNothing) {
	GTEST_SKIP() << "TODO";
}

// --- Tier 2: the failed-load path. Nothing below has ever run. ---

// Trigger a read error without a mock: truncate the file underneath the pool with
// std::filesystem::resize_file, then fetch a page that used to exist. AbandonLoad runs.
// Assert the error surfaces and the frame is back -- that is the pin_count == 0 half of
// AbandonLoad's disposal, which was wrong once already.
TEST_F(BufferPoolManagerTest, FailedLoadReturnsErrorAndReclaimsTheFrame) {
	GTEST_SKIP() << "TODO";
}

// The same, with several threads fetching the SAME page so some are asleep on Loading when
// the loader publishes Failed. Exercises the waiter drain, UnpinPage's notify on Failed, and
// AbandonLoad's pin_count == 1 wait. Every thread must get an error, and the pool must be
// whole after joining. If the notify is ever dropped this test hangs rather than fails, so
// give it a timeout.
TEST_F(BufferPoolManagerTest, FailedLoadWakesEveryWaiter) {
	GTEST_SKIP() << "TODO";
}

// --- Tier 3: concurrency. Run under TSan; these prove nothing on their own. ---
//
// A stress test that passes a thousand times says nothing about the thousand-and-first
// interleaving. What makes them worth running is TSan, which checks happens-before rather
// than outcomes and so flags a race even in a run whose timing happened to work out.
// Assert AssertQuiesced() after joining, not just "did not crash".

// N threads fetching one page. All must see identical bytes, and only one frame may be used.
TEST_F(BufferPoolManagerTest, ConcurrentFetchesOfOnePageShareOneFrame) {
	GTEST_SKIP() << "TODO";
}

// N threads fetching randomly from a working set larger than the pool, so the replacer is
// under constant pressure. Assert every read returns that page's own seeded bytes -- a frame
// handed to two threads shows up as one thread reading another page's content.
TEST_F(BufferPoolManagerTest, ConcurrentFetchesUnderPressureNeverCrossPages) {
	GTEST_SKIP() << "TODO";
}

// N threads calling NewPage. Every returned page id must be distinct. This is the test that
// catches a duplicate page-table mapping, which today aborts inside ShardGuard::Insert.
TEST_F(BufferPoolManagerTest, ConcurrentNewPageReturnsDistinctPages) {
	GTEST_SKIP() << "TODO";
}

// One thread deleting while others fetch the same page. Each operation must either succeed
// or fail cleanly; nothing may return another page's bytes. Expect spurious DeletePage
// rejections -- that is documented behaviour, not a failure.
TEST_F(BufferPoolManagerTest, ConcurrentDeleteAndFetchStayConsistent) {
	GTEST_SKIP() << "TODO";
}

}  // namespace kernsql

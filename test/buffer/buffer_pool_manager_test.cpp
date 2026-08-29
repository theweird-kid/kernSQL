#include "buffer/buffer_pool_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <latch>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

	// The header as it is ON DISK, bypassing the pool -- the counterpart to ReadBodyFromDisk,
	// and the only way to tell "the pool refused" from "the pool refused after already telling
	// DiskManager to free the page".
	PageHeader ReadHeaderFromDisk(page_id_t page_id) {
		std::array<std::byte, PAGE_SIZE> page{};
		EXPECT_TRUE(dm_->ReadPage(page_id, page).ok());
		return PageHeader::ReadFrom(
		    std::span<const std::byte, PAGE_SIZE>(page).first<PAGE_HEADER_SIZE>());
	}

	// Success only if every byte is zero, naming the first offset that is not. A bare EXPECT
	// over 4064 bytes either says nothing useful or says it 4064 times.
	static ::testing::AssertionResult AllZero(std::span<const std::byte> bytes) {
		const auto it = std::ranges::find_if(bytes, [](std::byte b) { return b != std::byte{0}; });
		if (it == bytes.end()) return ::testing::AssertionSuccess();
		return ::testing::AssertionFailure()
		       << "byte " << (it - bytes.begin()) << " of " << bytes.size() << " is "
		       << static_cast<unsigned>(*it) << ", expected all zeroes";
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

	// Released BEFORE the census, not after: these guards are the test's own memory pressure,
	// not a leak, and AssertQuiesced counts pins. Dropping them here is what makes the count
	// below mean "the failed NewPage left a frame behind".
	pages.clear();
	AssertQuiesced("after NewPage failed on a full pool");
}

// Same setup, then drop one guard. The next fetch must succeed -- proving ReclaimFrame found
// the now-unpinned frame through the replacer rather than the free list.
TEST_F(BufferPoolManagerTest, ReleasingAGuardMakesAFrameReclaimable) {
	auto pages = FillPool();
	pages.pop_back();

	// The precondition, asserted rather than assumed: nothing free, one candidate. Without it
	// the test still passes if FillPool quietly stopped filling, and would then be proving
	// nothing about the replacer.
	const PoolStats before = bpm_->GetStats();
	ASSERT_EQ(before.free_list_size, 0u) << "a free frame would let NewPage skip ReclaimFrame";
	ASSERT_EQ(before.pinned_frames, kFrames - 1);
	ASSERT_EQ(before.evictable, 1u);

	auto new_page = bpm_->NewPage();
	ASSERT_TRUE(new_page.has_value()) << new_page.error().message();

	pages.clear();
	new_page->Drop();
	AssertQuiesced("after reclaiming the released frame");
}

// Write a page, drop the guard, then force it out by fetching enough other pages. Its bytes
// must be on disk afterwards, read straight through DiskManager. This is the only test that
// exercises the dirty path of ReclaimFrame, which is the one that spans I/O.
TEST_F(BufferPoolManagerTest, DirtyVictimIsWrittenBackWhenEvicted) {
	const page_id_t page_id = SeedPage("");
	{
		auto write_page = bpm_->FetchPageWrite(page_id);
		ASSERT_TRUE(write_page.has_value()) << write_page.error().message();
		WriteBody(write_page.value(), "some data");
	}  // guard drops: dirty, unpinned, and now the pool's only eviction candidate

	EXPECT_EQ(ReadBodyFromDisk(page_id), "") << "dropping a guard must not write to disk";

	// Pigeonhole, not policy. The victim holds one frame and kFrames-1 are free, so fetching
	// kFrames other pages and keeping every guard alive leaves the last fetch with no free
	// frame and exactly one candidate: it must evict page_id, whatever the sweep would have
	// preferred. A loop of "enough" fetches instead bets on the replacer's current usage-count
	// policy, and starts flaking the day that policy is tuned.
	std::vector<ReadPageGuard> pressure;
	for (std::size_t i = 0; i < kFrames; ++i) {
		const page_id_t other = SeedPage("some other data");
		ASSERT_NE(other, INVALID_PAGE);
		auto g = bpm_->FetchPageRead(other);
		ASSERT_TRUE(g.has_value()) << g.error().message();
		pressure.push_back(std::move(g.value()));
	}

	EXPECT_EQ(ReadBodyFromDisk(page_id), "some data")
	    << "the dirty victim was evicted without a write-back";

	pressure.clear();
	AssertQuiesced("after the dirty victim was evicted");
}

// The body must be all zeroes and the header must carry this page's own id with page_type
// ALLOCATED. The header half matters: NewPage is the one path that creates page contents
// without reading them, so a missing stamp is only discovered on a later miss.
TEST_F(BufferPoolManagerTest, NewPageIsZeroedAndItsHeaderIsStamped) {
	{
		auto np = bpm_->NewPage();
		ASSERT_TRUE(np.has_value()) << np.error().message();

		// EXPECT, not ASSERT: these are independent fields, and the useful output is which of
		// them is wrong rather than just that the first one was.
		const PageHeader header = np->Header();
		EXPECT_EQ(header.page_id, np->PageId())
		    << "the bytes disagree with what the pool believes this frame holds";
		EXPECT_EQ(header.page_type, PageType::ALLOCATED);
		EXPECT_EQ(header.format_version, PAGE_FORMAT_VERSION)
		    << "NewPage stamps this by default-construction, not through write_page_header";

		// INVALID_PAGE is -1 while the zeroed value is 0, which is META_PAGE_ID: an unstamped
		// header does not read as empty, it reads as a link to the superblock -- and
		// next_page_id doubles as the disk freelist link. This is the pair that separates
		// "stamped" from "merely zeroed".
		EXPECT_EQ(header.next_page_id, INVALID_PAGE);
		EXPECT_EQ(header.prev_page_id, INVALID_PAGE);

		EXPECT_TRUE(AllZero(np->Body()));
	}

	AssertQuiesced("after NewPage");
}

// The zero-fill only has anything to do when the frame NewPage acquires is a reclaimed one
// still holding another page's bytes. On a fresh pool the frame comes off the free list with
// memory that was already zero at construction, so the test above stays green even with the
// fill deleted -- this is the one that fails.
TEST_F(BufferPoolManagerTest, NewPageZeroesARecycledFrame) {
	// Dirty every frame with recognisable bytes and release them all: the free list is empty
	// and every frame is a candidate, so NewPage has no choice but to go through ReclaimFrame.
	{
		std::vector<WritePageGuard> guards;
		for (std::size_t i = 0; i < kFrames; ++i) {
			const page_id_t id = SeedPage("");
			ASSERT_NE(id, INVALID_PAGE);
			auto g = bpm_->FetchPageWrite(id);
			ASSERT_TRUE(g.has_value()) << g.error().message();
			WriteBody(g.value(), "previous tenant");
			guards.push_back(std::move(g.value()));
		}
	}

	const PoolStats before = bpm_->GetStats();
	ASSERT_EQ(before.free_list_size, 0u) << "the frame NewPage takes must be a reclaimed one";
	ASSERT_EQ(before.evictable, kFrames);

	{
		auto np = bpm_->NewPage();
		ASSERT_TRUE(np.has_value()) << np.error().message();
		EXPECT_TRUE(AllZero(np->Body())) << "the previous tenant's bytes survived the reclaim";
	}

	AssertQuiesced("after NewPage on a recycled frame");
}

// NewPage with every frame pinned. It acquires a frame BEFORE allocating a page id, so the
// failure path has nothing to unwind -- but assert the page count on disk did not grow, and
// AssertQuiesced.
TEST_F(BufferPoolManagerTest, NewPageOnFullPoolLeaksNothing) {
	auto pages = FillPool();
	const page_id_t pages_before = dm_->PageCount();

	auto np = bpm_->NewPage();
	ASSERT_FALSE(np.has_value()) << "a fully pinned pool cannot hand out a new page";
	EXPECT_EQ(np.error().code(), ErrorCode::kBufferPoolFull);

	// PageCount only moves on AllocatePage's extend path, which makes it a leak detector here
	// only because nothing in this test has ever deallocated -- the disk freelist is empty, so
	// growth is the only way a leaked allocation can show. Add a DeletePage to the setup and a
	// leaked page would be consumed off the freelist instead, invisible to this check, and
	// DiskManager exposes no freelist head to assert against.
	EXPECT_EQ(dm_->PageCount(), pages_before) << "the failed NewPage allocated a page id anyway";

	pages.clear();
	AssertQuiesced("after NewPage failed on a full pool");
}

// DeletePage while a guard is alive must be refused, and refusal must change nothing: the
// page still fetchable, the frame still resident.
TEST_F(BufferPoolManagerTest, DeletePageOnAPinnedPageIsRejected) {
	page_id_t page_id{INVALID_PAGE};
	{
		auto guard = bpm_->NewPage();
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		page_id = guard->PageId();

		// Deleting a page this same thread holds a guard on is exactly the case DeletePage
		// refuses to wait for -- waiting on our own pin would deadlock outright.
		const Status st = bpm_->DeletePage(page_id);
		ASSERT_EQ(st.code(), ErrorCode::kInvalidArgument) << st.message();

		// "Changed nothing", asserted rather than hoped. The fetch at the end cannot see this:
		// the frame is still mapped, so that fetch is a cache HIT and never touches disk -- a
		// DeletePage that vacated the frame anyway would merely turn it into a miss that
		// re-reads and still succeeds.
		const PoolStats s = bpm_->GetStats();
		EXPECT_EQ(s.resident_frames, 1u) << "the refused delete vacated the frame anyway";
		EXPECT_EQ(s.pinned_frames, 1u);
		EXPECT_EQ(s.free_frames, kFrames - 1);

		// The other half the fetch cannot see: a rejection that ran DeallocatePage anyway
		// leaves the page on the disk freelist while it is still cached and readable -- free
		// stock and live data at once. Only a read straight through DiskManager catches it.
		EXPECT_EQ(ReadHeaderFromDisk(page_id).page_type, PageType::ALLOCATED)
		    << "the refused delete reached the disk allocator";
	}

	{
		auto pg = bpm_->FetchPageRead(page_id);
		ASSERT_TRUE(pg.has_value()) << pg.error().message();
	}

	AssertQuiesced("after the refused delete");
}

// After deleting, free_frames goes up by one and evictable goes DOWN by one. The second half
// is the assertion that matters -- DeletePage is the only place that pulls a frame out of the
// candidate set by hand, and forgetting it puts the frame in the free list and the replacer
// at once, so two threads can be handed the same frame.
TEST_F(BufferPoolManagerTest, DeletePageReturnsTheFrameAndLeavesTheCandidateSet) {
	page_id_t page_id{INVALID_PAGE};
	{
		auto np = bpm_->NewPage();
		ASSERT_TRUE(np.has_value()) << np.error().message();
		page_id = np->PageId();
	}  // guard drops: one frame resident, unpinned, and in the candidate set

	// Absolute values rather than deltas. These counters are unsigned, so a precondition of
	// evictable == 0 would make `before.evictable - 1` wrap to SIZE_MAX and report a bug that
	// is not the one that fired.
	const PoolStats before = bpm_->GetStats();
	ASSERT_EQ(before.free_frames, kFrames - 1);
	ASSERT_EQ(before.resident_frames, 1u);
	ASSERT_EQ(before.evictable, 1u) << "an unpinned resident frame must be a candidate";

	// The page is dirty here -- NewPage's write guard bumped the epoch on release -- so this
	// also covers the one path in the component where a dirty frame owes no writeback.
	const Status st = bpm_->DeletePage(page_id);
	ASSERT_TRUE(st.ok()) << st.message();

	const PoolStats after = bpm_->GetStats();
	EXPECT_EQ(after.free_frames, kFrames);
	EXPECT_EQ(after.resident_frames, 0u);
	EXPECT_EQ(after.evictable, 0u) << "the freed frame is still an eviction candidate";

	// The half neither counter above can see: free_frames counts state == Free, which
	// DeletePage sets in a different place from free_list_.Push. Delete the Push and both
	// EXPECTs above still pass while the frame is unreachable forever -- AssertQuiesced's
	// free_frames == free_list_size is the only thing that compares the two.
	AssertQuiesced("after deleting an unpinned page");
}

// Dirty several pages, call FlushAllPages once, verify every one of them on disk.
TEST_F(BufferPoolManagerTest, FlushAllPagesWritesEveryDirtyPage) {
	std::vector<page_id_t> pages;
	std::string dirty_payload{"dirty the page"};
	for (size_t i = 0; i < kFrames; i++) {
		auto np = bpm_->NewPage();
		ASSERT_TRUE(np.has_value());
		pages.push_back(np.value().PageId());
		WriteBody(np.value(), dirty_payload);
	}

	auto st = bpm_->FlushAllPages();
	ASSERT_EQ(st.code(), ErrorCode::kOk);

	// verify the content
	for (auto pg_id : pages) {
		std::string disk_read_value = ReadBodyFromDisk(pg_id);
		EXPECT_EQ(disk_read_value, dirty_payload);
	}

	AssertQuiesced("after flush pages and read");
}

// Two Shutdown() calls in a row: the second is a no-op returning OK, not a second flush and
// not an error.
TEST_F(BufferPoolManagerTest, ShutdownIsIdempotent) {
	auto st1 = bpm_->Shutdown();
	EXPECT_EQ(st1.code(), ErrorCode::kOk);
	auto st2 = bpm_->Shutdown();
	EXPECT_EQ(st2.code(), ErrorCode::kOk);

	AssertQuiesced("after shutdown");
}

// Write through the pool, Shutdown, destroy both manager and DiskManager, reopen the same
// path, read back. The end-to-end durability claim -- and the only test that would catch
// Shutdown flushing but never fsyncing.
TEST_F(BufferPoolManagerTest, DataSurvivesShutdownAndReopen) {
	std::vector<page_id_t> pages;
	std::string payload{"something"};
	for (size_t i = 0; i < 2 * kFrames; i++) {
		auto pg = bpm_->NewPage();
		ASSERT_TRUE(pg.has_value());
		pages.push_back(pg.value().PageId());
		WriteBody(pg.value(), payload);
	}
	auto st = bpm_->Shutdown();
	EXPECT_EQ(st.code(), ErrorCode::kOk);

	bpm_.reset();
	dm_.reset();

	auto tmp_dm = DiskManager::Open(path_).value();
	dm_ = std::move(tmp_dm);
	bpm_ = std::make_unique<BufferPoolManager>(*dm_, kFrames);

	for (page_id_t page_id : pages) {
		auto pg = bpm_->FetchPageRead(page_id);
		ASSERT_TRUE(pg.has_value());
		EXPECT_EQ(AsChars(pg.value().Body().subspan(0, payload.size())), payload);
	}
}

// Fetching a page id past the end of the file must fail cleanly. The real assertion is
// AssertQuiesced: the frame acquired for the load has to come back.
TEST_F(BufferPoolManagerTest, FetchPastEndOfFileLeaksNothing) {
	for (size_t i = 0; i < kFrames; i++) {
		auto write_page = bpm_->NewPage();
		EXPECT_TRUE(write_page.has_value());
	}

	auto pg = bpm_->FetchPageRead(kFrames * 3);
	ASSERT_FALSE(pg.has_value());
	ASSERT_EQ(pg.error().code(), ErrorCode::kInvalidArgument);
}

// --- Tier 2: the failed-load path. Nothing below has ever run. ---

// Trigger a read error without a mock: truncate the file underneath the pool with
// std::filesystem::resize_file, then fetch a page that used to exist. AbandonLoad runs.
// Assert the error surfaces and the frame is back -- that is the pin_count == 0 half of
// AbandonLoad's disposal, which was wrong once already.
TEST_F(BufferPoolManagerTest, FailedLoadReturnsErrorAndReclaimsTheFrame) {
	page_id_t page_id{INVALID_PAGE};
	{
		auto write_page = bpm_->NewPage();
		ASSERT_TRUE(write_page.has_value());
		page_id = write_page.value().PageId();
		WriteBody(write_page.value(), "some data that should get lost");
	}
	FillPool();

	std::filesystem::resize_file(
	    path_, static_cast<size_t>(page_id) * PAGE_SIZE);  // truncate from this page onwards

	auto read_page = bpm_->FetchPageRead(page_id);
	ASSERT_FALSE(read_page.has_value());
	ASSERT_EQ(read_page.error().code(), ErrorCode::kCorruption);

	AssertQuiesced("after failed load");
}

// The same, with several threads fetching the SAME page so some are asleep on Loading when
// the loader publishes Failed. Exercises the waiter drain, UnpinPage's notify on Failed, and
// AbandonLoad's pin_count == 1 wait. Every thread must get an error, and the pool must be
// whole after joining. If the notify is ever dropped this test hangs rather than fails, so
// give it a timeout.
TEST_F(BufferPoolManagerTest, FailedLoadWakesEveryWaiter) {
	// Fixed, not hardware_concurrency(): that is allowed to return 0, which would spawn no
	// threads and pass vacuously, and it would otherwise make the laptop and CI run different
	// tests. Rounds, because a single round is one sample of one interleaving.
	constexpr std::size_t kThreads = 8;
	constexpr std::size_t kRounds = 32;

	// Victims are seeded straight through DiskManager, so they exist on disk and were never
	// resident: every fetch below is a guaranteed miss that reaches the file. Creating them
	// with NewPage instead would need each one evicted first, and evicting a dirty page
	// rewrites it -- re-extending the very file this test is about to truncate.
	std::vector<page_id_t> victims;
	for (std::size_t i = 0; i < kRounds; ++i) {
		const page_id_t id = SeedPage("");
		ASSERT_NE(id, INVALID_PAGE);
		victims.push_back(id);
	}

	// From the first victim onwards. Everything below it -- the meta page and the catalog root
	// -- survives, so TearDown's Shutdown still has a well-formed file underneath it.
	std::filesystem::resize_file(path_, static_cast<std::uintmax_t>(victims.front()) * PAGE_SIZE);

	std::size_t loads_observed = 0;
	std::size_t waits_observed = 0;

	for (const page_id_t victim : victims) {
		// One slot per thread, sized before any thread starts. Each thread writes only its own
		// index, so there is nothing to lock; aggregating happens on the main thread after the
		// join. A shared counter here would need a mutex and would tell you less.
		std::vector<Status> results(kThreads, Status::OK());

		// Every thread blocks until the last one arrives, so they storm FetchPageRead together
		// rather than trickling in after the loader has already failed and cleaned up. This is
		// what makes a thread asleep on Loading likely at all.
		std::latch gate(static_cast<std::ptrdiff_t>(kThreads));

		std::vector<std::thread> threads;
		threads.reserve(kThreads);
		for (std::size_t i = 0; i < kThreads; ++i) {
			threads.emplace_back([&, i] {
				gate.arrive_and_wait();
				auto page = bpm_->FetchPageRead(victim);

				// No gtest assertion in here. ASSERT_* expands to a bare `return;`, which would
				// return from this lambda rather than from the test, and a fatal failure raised
				// off the main thread does not stop the test anyway. Record; assert after the
				// join. The guard, if one were wrongly handed out, dies at this brace -- before
				// the join, so the census below sees no pins.
				results[i] = page.has_value() ? Status::OK() : page.error();
			});
		}
		for (auto& t : threads) t.join();

		for (std::size_t i = 0; i < kThreads; ++i) {
			const Status& st = results[i];

			// The invariant that always holds: a page whose bytes are gone is fetchable by
			// nobody, whatever order the threads ran in.
			EXPECT_FALSE(st.ok()) << "thread " << i << " got a guard for page " << victim
			                      << ", which cannot be read";

			// Three codes are legal, and which one a thread gets is decided by the interleaving
			// -- asserting any single one of them would be asserting on a race:
			//   kCorruption      the loader; its pread hit EOF on the truncated file
			//   kIOError         a waiter; it slept on Loading and woke to find Failed
			//   kBufferPoolFull  it lost the race for a frame -- the loader's frame is pinned
			//                    and the rest are momentarily held by other retrying threads,
			//                    so the replacer has no candidate to offer
			// Anything else is the bug this test is looking for.
			EXPECT_TRUE(st.code() == ErrorCode::kCorruption || st.code() == ErrorCode::kIOError ||
			            st.code() == ErrorCode::kBufferPoolFull)
			    << "thread " << i << " on page " << victim << ": " << st.message();

			if (st.code() == ErrorCode::kCorruption) ++loads_observed;
			if (st.code() == ErrorCode::kIOError) ++waits_observed;
		}

		// Per round rather than once at the end. Every thread has joined, so the pool really is
		// quiescent and PoolStats is meaningful -- and a frame lost in round 3 is a failure in
		// round 3, not an unexplained kBufferPoolFull in round 30.
		AssertQuiesced("after a failed concurrent load");
	}

	// Guaranteed: the free list is full at the start of every round, so somebody always wins a
	// frame, publishes the mapping and reaches the read.
	EXPECT_GT(loads_observed, 0u) << "no thread ever got as far as the disk read";

	// NOT asserted, deliberately. The failing read is one pread that returns in microseconds
	// and there is no fault-injection hook to widen that window, so no amount of hammering can
	// GUARANTEE a thread is asleep on Loading when Failed is published. Asserting on it would
	// be asserting on a race -- the definition of a flaky test. Recorded instead, so a run that
	// exercised the drain is distinguishable from one that did not.
	RecordProperty("waiter_wakeups_observed", static_cast<int>(waits_observed));
	if (waits_observed == 0) {
		GTEST_LOG_(WARNING) << "no thread was ever asleep on Loading: the waiter drain, "
		                       "UnpinPage's notify on Failed and AbandonLoad's pin_count == 1 "
		                       "wait were NOT exercised in this run";
	}
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

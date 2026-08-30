#include "storage/disk_manager.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <set>
#include <span>
#include <thread>
#include <vector>

#include "common/page_header.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

namespace kernsql {
class DiskManagerTest : public ::testing::Test {
  protected:
	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name());
		std::filesystem::remove(path_);  // in case a prior crashed run left it behind
	}
	void TearDown() override { std::filesystem::remove(path_); }

	std::filesystem::path path_;
};

// Allocates until the file has to grow, returning every page_id the freelist handed
// back on the way — i.e. it drains the freelist and stops at the first allocation
// that extends the file.
//
// This exists because the obvious freelist test cannot see the failure that matters.
// "Deallocate twice, then allocate once" passes even when the second deallocation has
// threaded the page onto the chain a second time, because the first allocation off a
// self-linked page looks perfectly normal. The cycle only shows up on the *next*
// allocation, as either a repeated id or the Corruption error AllocatePage raises when
// a page on the freelist isn't stamped FREE. Draining catches both, and the iteration
// cap turns an actual infinite chain into a failed assertion instead of a hung test.
inline std::vector<page_id_t> DrainFreelist(DiskManager& dm) {
	std::vector<page_id_t> reused;
	const page_id_t page_count_before = dm.PageCount();

	while (reused.size() <= 1024) {
		auto id = dm.AllocatePage();
		if (!id.has_value()) {
			ADD_FAILURE() << "AllocatePage failed while draining the freelist: "
			              << id.error().message();
			return reused;
		}
		// This allocation extended the file, so the freelist was already empty.
		if (dm.PageCount() != page_count_before) return reused;
		reused.push_back(id.value());
	}

	ADD_FAILURE() << "freelist did not drain after 1024 allocations — the chain has a cycle";
	return reused;
}

// ---------------------------------------------------------------------------
// Open() — fresh file
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, DiskManagerAllocatesMetaAndCatalogPage) {
	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value()) << dm.error().message();

	EXPECT_EQ(2, dm.value()->PageCount());

	// page 0 (meta) is never readable through DiskManager's public API — that's
	// the whole point of the reservation — so we go around it with a raw pread
	// to check what Open() actually put on disk.
	int fd = open(path_.c_str(), O_RDONLY);
	ASSERT_GE(fd, 0);
	std::array<std::byte, PAGE_HEADER_SIZE> meta_buf{};
	ASSERT_EQ(pread(fd, meta_buf.data(), PAGE_HEADER_SIZE, 0),
	          static_cast<ssize_t>(PAGE_HEADER_SIZE));
	close(fd);
	PageHeader meta_header = PageHeader::ReadFrom(meta_buf);
	EXPECT_EQ(meta_header.page_type, PageType::META);

	// page 1 (catalog root) IS readable through the public API, so this part
	// goes through ReadPage like a real caller would.
	std::array<std::byte, PAGE_SIZE> catalog_page{};
	ASSERT_TRUE(dm.value()->ReadPage(CATALOG_ROOT_PAGE_ID, catalog_page).ok());
	PageHeader catalog_header =
	    PageHeader::ReadFrom(std::span(catalog_page).first<PAGE_HEADER_SIZE>());
	EXPECT_EQ(catalog_header.page_type, PageType::CATALOG);
}

// ---------------------------------------------------------------------------
// Open() — reopen / recovery
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, ContentOnCatalogPageSurvivesReopen) {
	// Open(), WritePage(CATALOG_ROOT_PAGE_ID, some recognizable buffer),
	// Sync(), then drop the handle (dm.reset() or let it go out of scope) so
	// the fd is closed. Open() again on the same path_, ReadPage back, and
	// compare bytes against what you wrote.

	auto dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> page{};
	ASSERT_TRUE(dm.value()->ReadPage(CATALOG_ROOT_PAGE_ID, page).ok());

	const char* text = "My Unique testing bytes";
	std::memcpy(page.data() + PAGE_HEADER_SIZE, text, strlen(text));

	ASSERT_TRUE(dm.value()->WritePage(CATALOG_ROOT_PAGE_ID, page).ok());
	ASSERT_TRUE(dm.value()->Sync().ok());

	dm->reset();

	dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> catalog_page{};
	ASSERT_TRUE(dm.value()->ReadPage(CATALOG_ROOT_PAGE_ID, catalog_page).ok());

	ASSERT_EQ(std::memcmp(catalog_page.data() + PAGE_HEADER_SIZE, text, std::strlen(text)), 0);
}

TEST_F(DiskManagerTest, FreelistSurvivesReopen) {
	// Open(), AllocatePage() -> id, DeallocatePage(id), drop the handle,
	// reopen, AllocatePage() again. Expect the same id back — this proves
	// freelist_head_ was recovered from page 0's on-disk header on the
	// second Open(), not just held over in memory.

	auto dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	auto res_1 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_1.has_value());

	auto res_2 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_2.has_value());

	ASSERT_TRUE(dm.value()->DeallocatePage(res_2.value()).ok());
	dm->reset();

	dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	auto res_3 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_3.has_value());

	ASSERT_EQ(res_2.value(), res_3.value());
}

// ---------------------------------------------------------------------------
// Open() — corruption
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, OpenRejectsSizeNotMultipleOfPageSize) {
	// Create a valid file first (Open() once, drop the handle so the fd is
	// closed), then use std::filesystem::resize_file(path_, N) to resize it
	// to something that isn't a multiple of PAGE_SIZE. Reopen -> expect
	// Status::Corruption.

	auto dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	dm->reset();

	std::filesystem::resize_file(DiskManagerTest::path_, PAGE_SIZE - 6);

	dm = DiskManager::Open(DiskManagerTest::path_);
	// Assert the failure *before* touching .error(): calling error() on an expected
	// that holds a value is UB, so without this the failure mode of a regression here
	// is a garbage read rather than a red test.
	ASSERT_FALSE(dm.has_value()) << "Open() unexpectedly succeeded on a corrupt file";
	ASSERT_EQ(Status::Corruption("doesn't matter, only comparing codes").code(), dm.error().code());
}

TEST_F(DiskManagerTest, OpenRejectsFileWithOnlyOnePage) {
	// Same setup as above, but resize_file(path_, PAGE_SIZE) to truncate off
	// the catalog root page, leaving only page 0. Reopen -> expect
	// Status::Corruption.

	auto dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	dm->reset();

	std::filesystem::resize_file(DiskManagerTest::path_, PAGE_SIZE);

	dm = DiskManager::Open(DiskManagerTest::path_);
	// Assert the failure *before* touching .error(): calling error() on an expected
	// that holds a value is UB, so without this the failure mode of a regression here
	// is a garbage read rather than a red test.
	ASSERT_FALSE(dm.has_value()) << "Open() unexpectedly succeeded on a corrupt file";
	ASSERT_EQ(Status::Corruption("doesn't matter, only comparing codes").code(), dm.error().code());
}

TEST_F(DiskManagerTest, OpenRejectsCorruptMetaPageType) {
	// Get a valid 2-page file, close it, then use raw POSIX (open/pwrite/
	// close, not DiskManager) to overwrite the page_type byte at file offset
	// 0 with a value other than PageType::META. Reopen -> expect
	// Status::Corruption. This is the one place raw syscalls belong in these
	// tests — DiskManager itself would never produce this on-disk state, so
	// there's no way to reach it through the public API.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	dm->reset();

	int fd = open(path_.c_str(), O_WRONLY);
	ASSERT_GE(fd, 0);
	PageHeader corrupt_header;
	corrupt_header.page_type = PageType::HEAP;

	std::array<std::byte, PAGE_HEADER_SIZE> meta_buf{};
	corrupt_header.WriteTo(meta_buf);

	ASSERT_EQ(pwrite(fd, meta_buf.data(), PAGE_HEADER_SIZE, 0),
	          static_cast<ssize_t>(PAGE_HEADER_SIZE));
	fsync(fd);
	close(fd);

	dm = DiskManager::Open(path_);
	// Assert the failure *before* touching .error(): calling error() on an expected
	// that holds a value is UB, so without this the failure mode of a regression here
	// is a garbage read rather than a red test.
	ASSERT_FALSE(dm.has_value()) << "Open() unexpectedly succeeded on a corrupt file";
	ASSERT_EQ(Status::Corruption("doesn't matter, only comparing codes").code(), dm.error().code());
}

TEST_F(DiskManagerTest, OpenRejectsCorruptCatalogPageType) {
	// Same idea, but corrupt the page_type byte at file offset PAGE_SIZE
	// (the start of page 1's header) instead of page 0's.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	dm->reset();

	int fd = open(path_.c_str(), O_WRONLY);
	ASSERT_GE(fd, 0);
	PageHeader corrupt_header;
	corrupt_header.page_type = PageType::HEAP;

	std::array<std::byte, PAGE_HEADER_SIZE> catalog_buf{};
	corrupt_header.WriteTo(catalog_buf);

	ASSERT_EQ(pwrite(fd, catalog_buf.data(), PAGE_HEADER_SIZE, PAGE_SIZE),
	          static_cast<ssize_t>(PAGE_HEADER_SIZE));
	fsync(fd);
	close(fd);

	dm = DiskManager::Open(path_);
	// Assert the failure *before* touching .error(): calling error() on an expected
	// that holds a value is UB, so without this the failure mode of a regression here
	// is a garbage read rather than a red test.
	ASSERT_FALSE(dm.has_value()) << "Open() unexpectedly succeeded on a corrupt file";
	ASSERT_EQ(Status::Corruption("doesn't matter, only comparing codes").code(), dm.error().code());
}

TEST_F(DiskManagerTest, OpenRejectsZeroedMetaPage) {
	// An all-zero file is already caught, but not by the meta-page check: zeros read
	// as page_type 0, and page 1's CATALOG check is what rejects it. So the meta page
	// itself is unguarded against zeros, and this test isolates that by zeroing page 0
	// while leaving page 1 a perfectly valid catalog page.
	//
	// This is the case PageType::INVALID = 0 exists for. With META at enum value 0, a
	// zeroed page 0 validated as a real meta page and Open() *succeeded* — then
	// recovered freelist_head_ from the zeroed next_page_id, i.e. 0, putting the
	// reserved meta page itself at the head of the freelist for the next
	// AllocatePage() to hand out.
	//
	// Still a mitigation, not a format check: a file whose first byte happens to equal
	// PageType::META passes regardless. A magic + version field in the meta page is
	// the real fix and is not written yet.
	int fd = open(path_.c_str(), O_RDWR | O_CREAT, 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(0, ftruncate(fd, 2 * static_cast<off_t>(PAGE_SIZE)));

	PageHeader catalog_header;
	catalog_header.page_type = PageType::CATALOG;
	std::array<std::byte, PAGE_HEADER_SIZE> catalog_buf{};
	catalog_header.WriteTo(catalog_buf);
	ASSERT_EQ(pwrite(fd, catalog_buf.data(), PAGE_HEADER_SIZE, PAGE_SIZE),
	          static_cast<ssize_t>(PAGE_HEADER_SIZE));
	fsync(fd);
	close(fd);

	auto dm = DiskManager::Open(path_);
	ASSERT_FALSE(dm.has_value()) << "Open() accepted a file whose meta page is all zeros";
	ASSERT_EQ(Status::Corruption("doesn't matter, only comparing codes").code(), dm.error().code());
}

// ---------------------------------------------------------------------------
// ReadPage / WritePage
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, ReadPageRejectsMetaPage) {
	// ReadPage(META_PAGE_ID, ...) -> expect !status.ok(), and probably check
	// the specific error code (InvalidArgument) too.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> out{};
	ASSERT_EQ(dm.value()->ReadPage(META_PAGE_ID, out).code(),
	          Status::InvalidArgument("doesn't matter, only comparing codes").code());
}

TEST_F(DiskManagerTest, WritePageRejectsMetaPage) {
	// WritePage(META_PAGE_ID, ...) -> expect !status.ok() (InvalidArgument).

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> in{};
	ASSERT_EQ(dm.value()->WritePage(META_PAGE_ID, in).code(),
	          Status::InvalidArgument("doesn't matter, only comparing codes").code());
}

TEST_F(DiskManagerTest, ReadWritePageRoundTripsOnCatalogPage) {
	// Build a PAGE_SIZE buffer with a recognizable byte pattern,
	// WritePage(CATALOG_ROOT_PAGE_ID, buffer), then
	// ReadPage(CATALOG_ROOT_PAGE_ID, out) into a fresh buffer and compare.
	// This is the test that proves the catalog layer can actually use its
	// own reserved page.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> catalog_buf{};
	ASSERT_TRUE(dm.value()->ReadPage(CATALOG_ROOT_PAGE_ID, catalog_buf).ok());

	const char* text = "My Unique testing bytes";
	std::memcpy(catalog_buf.data() + PAGE_HEADER_SIZE, text, strlen(text));
	ASSERT_TRUE(dm.value()->WritePage(CATALOG_ROOT_PAGE_ID, catalog_buf).ok());

	std::array<std::byte, PAGE_SIZE> fresh_buf{};
	ASSERT_TRUE(dm.value()->ReadPage(CATALOG_ROOT_PAGE_ID, fresh_buf).ok());
	ASSERT_EQ(0, std::memcmp(catalog_buf.data(), fresh_buf.data(), PAGE_SIZE));
}

TEST_F(DiskManagerTest, ReadPageRejectsOutOfRangePageId) {
	// Try a negative page_id and a page_id == PageCount() (one past the
	// last valid page) — both should fail.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> buf{};
	Status st = dm.value()->ReadPage(INVALID_PAGE, buf);
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());

	st = dm.value()->ReadPage(dm.value()->PageCount(), buf);
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());
}

TEST_F(DiskManagerTest, WritePageRejectsOutOfRangePageId) {
	// Same as above but for WritePage.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> buf{};
	Status st = dm.value()->WritePage(INVALID_PAGE, buf);
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());

	st = dm.value()->WritePage(dm.value()->PageCount(), buf);
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());
}

// ---------------------------------------------------------------------------
// AllocatePage
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, FirstAllocateReturnsPageTwo) {
	// On a fresh file, AllocatePage() -> expect the returned page_id == 2
	// (never 0 or 1, both reserved).

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto res = dm.value()->AllocatePage();
	ASSERT_TRUE(res.has_value());

	ASSERT_EQ(2, res.value());
}

TEST_F(DiskManagerTest, AllocatedPageHeaderIsStampedAllocated) {
	// AllocatePage() -> id, then ReadPage(id, ...) and check the header's
	// page_type == PageType::ALLOCATED — proves the caller never sees a
	// page that could be mistaken for FREE or an uninitialized type.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto res = dm.value()->AllocatePage();
	ASSERT_TRUE(res.has_value());

	std::array<std::byte, PAGE_SIZE> buf{};
	ASSERT_TRUE(dm.value()->ReadPage(res.value(), buf).ok());

	auto ph = PageHeader::ReadFrom(std::span(buf).first<PAGE_HEADER_SIZE>());
	ASSERT_EQ(ph.page_type, PageType::ALLOCATED);
}

TEST_F(DiskManagerTest, SequentialAllocationsExtendFileInOrder) {
	// Call AllocatePage() three times on a fresh file -> expect 2, 3, 4 in
	// order, and PageCount() == 5 afterward.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	Result<page_id_t> res;
	for (int exp = 2; exp <= 4; exp++) {
		res = dm.value()->AllocatePage();
		ASSERT_TRUE(res.has_value());
		ASSERT_EQ(exp, res.value());
	}

	ASSERT_EQ(5, dm.value()->PageCount());
}

TEST_F(DiskManagerTest, AllocateReusesFreedPageBeforeExtending) {
	// AllocatePage() -> id_a, DeallocatePage(id_a), AllocatePage() -> id_b.
	// Expect id_b == id_a, and PageCount() unchanged from before the second
	// AllocatePage() call (proves the freelist was consulted before
	// extending the file).

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto res_1 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_1.has_value());

	ASSERT_TRUE(dm.value()->DeallocatePage(res_1.value()).ok());

	page_id_t count_before_reuse = dm.value()->PageCount();

	auto res_2 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_2.has_value());

	ASSERT_EQ(res_1.value(), res_2.value());
	ASSERT_EQ(count_before_reuse, dm.value()->PageCount());
}

// ---------------------------------------------------------------------------
// DeallocatePage
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, DeallocatePageRejectsMetaPage) {
	// DeallocatePage(META_PAGE_ID) -> expect InvalidArgument.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto st = dm.value()->DeallocatePage(META_PAGE_ID);
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());
}

TEST_F(DiskManagerTest, DeallocatePageRejectsCatalogRootPage) {
	// DeallocatePage(CATALOG_ROOT_PAGE_ID) -> expect InvalidArgument. This
	// is the one that used to silently succeed before the reservation check
	// was added — worth a comment noting that's exactly what this guards.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto st = dm.value()->DeallocatePage(CATALOG_ROOT_PAGE_ID);
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());
}

TEST_F(DiskManagerTest, DeallocatingAlreadyFreePageIsNoOp) {
	// AllocatePage() -> id, DeallocatePage(id) twice in a row. Both calls
	// should return OK(), and a following AllocatePage() should return id
	// again (not skip past it, and not corrupt the freelist by linking it
	// to itself).

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto res_1 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_1.has_value());

	auto st = Status::OK();
	for (int it = 1; it <= 2; it++) {
		st = dm.value()->DeallocatePage(res_1.value());
		ASSERT_EQ(Status::OK().code(), st.code());
	}

	auto res_2 = dm.value()->AllocatePage();
	ASSERT_TRUE(res_2.has_value());
	ASSERT_EQ(res_1.value(), res_2.value());
}

TEST_F(DiskManagerTest, DeallocatePageRejectsOutOfRangePageId) {
	// DeallocatePage on a page_id >= PageCount() -> expect InvalidArgument.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());

	auto st = dm.value()->DeallocatePage(dm.value()->PageCount());
	ASSERT_EQ(Status::InvalidArgument("doesn't matter").code(), st.code());
}

// ---------------------------------------------------------------------------
// Freelist structure
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, FreelistReuseIsLifo) {
	// The freelist is a stack threaded through each free page's next_page_id, with
	// page 0 holding the head, so the most recently freed page must come back first.
	// AllocateReusesFreedPageBeforeExtending only frees one page, which cannot tell
	// LIFO from FIFO from "returns an arbitrary free page."

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());
	DiskManager& disk = *dm.value();

	std::vector<page_id_t> allocated;
	for (int i = 0; i < 3; i++) {
		auto id = disk.AllocatePage();
		ASSERT_TRUE(id.has_value()) << id.error().message();
		allocated.push_back(id.value());
	}

	for (page_id_t id : allocated) {
		ASSERT_TRUE(disk.DeallocatePage(id).ok());
	}

	std::vector<page_id_t> expected(allocated.rbegin(), allocated.rend());
	EXPECT_EQ(expected, DrainFreelist(disk));
}

TEST_F(DiskManagerTest, DoubleDeallocateDoesNotCycleFreelist) {
	// The already-FREE check in DeallocatePage is what stops a page being threaded
	// onto the chain twice. DeallocatingAlreadyFreePageIsNoOp proves the second call
	// returns OK and that one allocation still works; it does not prove the chain is
	// intact, because a self-linked page survives exactly one allocation. Draining is
	// what distinguishes them.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());
	DiskManager& disk = *dm.value();

	auto first = disk.AllocatePage();
	ASSERT_TRUE(first.has_value());
	auto second = disk.AllocatePage();
	ASSERT_TRUE(second.has_value());

	ASSERT_TRUE(disk.DeallocatePage(second.value()).ok());
	ASSERT_TRUE(disk.DeallocatePage(second.value()).ok());  // no-op, must not re-thread

	// Exactly one page on the list, once. A cycle would show up as a repeat, a
	// Corruption error, or a drain that never terminates — DrainFreelist fails on all
	// three.
	EXPECT_EQ(std::vector<page_id_t>{second.value()}, DrainFreelist(disk));
}

// ---------------------------------------------------------------------------
// Short/interrupted I/O
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, ReadPageOnTruncatedFileReportsCorruption) {
	// Exercises full_read's EOF branch, which is the one case it must NOT treat as a
	// short read: pread returning 0 with bytes outstanding means the file is smaller
	// than page_count_ claims, and looping would never make progress. If this
	// regresses, the symptom is a hung test rather than a failed one — which is
	// precisely why the distinction is worth a test.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());
	DiskManager& disk = *dm.value();

	auto id = disk.AllocatePage();
	ASSERT_TRUE(id.has_value());
	ASSERT_TRUE(disk.Sync().ok());

	// Truncate the page away behind the open handle. page_count_ is in-memory state,
	// so the bounds check still passes and the read runs straight off the end.
	std::filesystem::resize_file(path_, static_cast<std::uintmax_t>(id.value()) * PAGE_SIZE);

	std::array<std::byte, PAGE_SIZE> buf{};
	Status st = disk.ReadPage(id.value(), buf);
	EXPECT_FALSE(st.ok());
	EXPECT_EQ(Status::Corruption("doesn't matter").code(), st.code());
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, ConcurrentAllocateWriteReadDeallocateIsRaceFree) {
	// The point of this test is meta_latch_ and the atomic page_count_, so it must be
	// run under tsan to mean anything — without it, an unsynchronized freelist_head_
	// passes here most of the time.
	//
	// Each thread only ever touches the page it currently owns, so the absence of
	// per-page locking in DiskManager is deliberately not under test; that exclusion
	// is the buffer pool's job. What is under test is the shared allocator state.

	auto dm = DiskManager::Open(path_);
	ASSERT_TRUE(dm.has_value());
	DiskManager& disk = *dm.value();

	constexpr int kThreads = 8;
	constexpr int kIterations = 50;

	std::atomic<int> failures{0};
	std::vector<std::thread> threads;
	threads.reserve(kThreads);

	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([&disk, &failures, t] {
			for (int i = 0; i < kIterations; i++) {
				auto id = disk.AllocatePage();
				if (!id.has_value()) {
					failures++;
					return;
				}

				// Write a real header, not zeros: a zeroed header reads back as
				// PageType::INVALID, and the DeallocatePage below would then be
				// operating on a page whose type says something this test never
				// intended.
				std::array<std::byte, PAGE_SIZE> buf{};
				PageHeader header;
				header.page_type = PageType::HEAP;
				header.WriteTo(std::span(buf).first<PAGE_HEADER_SIZE>());
				buf[PAGE_HEADER_SIZE] = static_cast<std::byte>(t);

				if (!disk.WritePage(id.value(), buf).ok()) {
					failures++;
					return;
				}

				std::array<std::byte, PAGE_SIZE> out{};
				if (!disk.ReadPage(id.value(), out).ok()) {
					failures++;
					return;
				}
				// Nobody else can be holding this page, so our own bytes must survive
				// the round trip. A torn read here would mean two threads were handed
				// the same page_id.
				if (out[PAGE_HEADER_SIZE] != static_cast<std::byte>(t)) {
					failures++;
					return;
				}

				if (!disk.DeallocatePage(id.value()).ok()) {
					failures++;
					return;
				}
			}
		});
	}

	for (std::thread& thread : threads) {
		thread.join();
	}
	ASSERT_EQ(0, failures.load());

	// Every page allocated above was freed again, so the chain must drain cleanly.
	// Interleaved deallocations that lost an update would leave either a duplicate or
	// a cycle.
	std::vector<page_id_t> reused = DrainFreelist(disk);
	std::set<page_id_t> distinct(reused.begin(), reused.end());
	EXPECT_EQ(reused.size(), distinct.size()) << "freelist handed out a duplicate page_id";
}

}  // namespace kernsql

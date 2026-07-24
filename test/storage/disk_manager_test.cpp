#include "storage/disk_manager.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>

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
	std::array<std::byte, PAGE_HEADER_SIZE> meta_buf;
	ASSERT_EQ(pread(fd, meta_buf.data(), PAGE_HEADER_SIZE, 0),
	          static_cast<ssize_t>(PAGE_HEADER_SIZE));
	close(fd);
	PageHeader meta_header = PageHeader::ReadFrom(meta_buf);
	EXPECT_EQ(meta_header.page_type, PageType::META);

	// page 1 (catalog root) IS readable through the public API, so this part
	// goes through ReadPage like a real caller would.
	std::array<std::byte, PAGE_SIZE> catalog_page;
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

	std::array<std::byte, PAGE_SIZE> page;
	ASSERT_TRUE(dm.value()->ReadPage(CATALOG_ROOT_PAGE_ID, page).ok());

	const char* text = "My Unique testing bytes";
	std::memcpy(page.data() + PAGE_HEADER_SIZE, text, strlen(text));

	ASSERT_TRUE(dm.value()->WritePage(CATALOG_ROOT_PAGE_ID, page).ok());
	ASSERT_TRUE(dm.value()->Sync().ok());

	dm->reset();

	dm = DiskManager::Open(DiskManagerTest::path_);
	ASSERT_TRUE(dm.has_value());

	std::array<std::byte, PAGE_SIZE> catalog_page;
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
	ASSERT_EQ(dm.error().code(), Status::Corruption("doesn't matter, only comparing codes").code());
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
	ASSERT_EQ(dm.error().code(), Status::Corruption("doesn't matter, only comparing codes").code());
}

TEST_F(DiskManagerTest, OpenRejectsCorruptMetaPageType) {
	// Get a valid 2-page file, close it, then use raw POSIX (open/pwrite/
	// close, not DiskManager) to overwrite the page_type byte at file offset
	// 0 with a value other than PageType::META. Reopen -> expect
	// Status::Corruption. This is the one place raw syscalls belong in these
	// tests — DiskManager itself would never produce this on-disk state, so
	// there's no way to reach it through the public API.

	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, OpenRejectsCorruptCatalogPageType) {
	// Same idea, but corrupt the page_type byte at file offset PAGE_SIZE
	// (the start of page 1's header) instead of page 0's.
	ASSERT_TRUE(false);
}

// ---------------------------------------------------------------------------
// ReadPage / WritePage
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, ReadPageRejectsMetaPage) {
	// ReadPage(META_PAGE_ID, ...) -> expect !status.ok(), and probably check
	// the specific error code (InvalidArgument) too.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, WritePageRejectsMetaPage) {
	// WritePage(META_PAGE_ID, ...) -> expect !status.ok() (InvalidArgument).
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, ReadWritePageRoundTripsOnCatalogPage) {
	// Build a PAGE_SIZE buffer with a recognizable byte pattern,
	// WritePage(CATALOG_ROOT_PAGE_ID, buffer), then
	// ReadPage(CATALOG_ROOT_PAGE_ID, out) into a fresh buffer and compare.
	// This is the test that proves the catalog layer can actually use its
	// own reserved page.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, ReadPageRejectsOutOfRangePageId) {
	// Try a negative page_id and a page_id == PageCount() (one past the
	// last valid page) — both should fail.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, WritePageRejectsOutOfRangePageId) {
	// Same as above but for WritePage.
	ASSERT_TRUE(false);
}

// ---------------------------------------------------------------------------
// AllocatePage
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, FirstAllocateReturnsPageTwo) {
	// On a fresh file, AllocatePage() -> expect the returned page_id == 2
	// (never 0 or 1, both reserved).
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, AllocatedPageHeaderIsStampedAllocated) {
	// AllocatePage() -> id, then ReadPage(id, ...) and check the header's
	// page_type == PageType::ALLOCATED — proves the caller never sees a
	// page that could be mistaken for FREE or an uninitialized type.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, SequentialAllocationsExtendFileInOrder) {
	// Call AllocatePage() three times on a fresh file -> expect 2, 3, 4 in
	// order, and PageCount() == 5 afterward.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, AllocateReusesFreedPageBeforeExtending) {
	// AllocatePage() -> id_a, DeallocatePage(id_a), AllocatePage() -> id_b.
	// Expect id_b == id_a, and PageCount() unchanged from before the second
	// AllocatePage() call (proves the freelist was consulted before
	// extending the file).
	ASSERT_TRUE(false);
}

// ---------------------------------------------------------------------------
// DeallocatePage
// ---------------------------------------------------------------------------

TEST_F(DiskManagerTest, DeallocatePageRejectsMetaPage) {
	// DeallocatePage(META_PAGE_ID) -> expect InvalidArgument.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, DeallocatePageRejectsCatalogRootPage) {
	// DeallocatePage(CATALOG_ROOT_PAGE_ID) -> expect InvalidArgument. This
	// is the one that used to silently succeed before the reservation check
	// was added — worth a comment noting that's exactly what this guards.
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, DeallocatingAlreadyFreePageIsNoOp) {
	// AllocatePage() -> id, DeallocatePage(id) twice in a row. Both calls
	// should return OK(), and a following AllocatePage() should return id
	// again (not skip past it, and not corrupt the freelist by linking it
	// to itself).
	ASSERT_TRUE(false);
}

TEST_F(DiskManagerTest, DeallocatePageRejectsOutOfRangePageId) {
	// DeallocatePage on a page_id >= PageCount() -> expect InvalidArgument.
	ASSERT_TRUE(false);
}

}  // namespace kernsql

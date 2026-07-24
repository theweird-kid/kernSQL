#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

#include "common/page_header.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

namespace kernsql {

// DiskManager owns a single on-disk file, split into fixed-size PAGE_SIZE pages
// addressed by page_id_t starting at 0. It is the only component that issues raw
// pread/pwrite calls; every layer above (buffer pool, index, etc.) goes through it
// rather than touching the file directly.
//
// page_id 0 (META_PAGE_ID) is permanently reserved as a superblock: its header
// stores the freelist head (in the PageHeader::next_page_id field) so that freed
// pages survive a close/reopen. It is never handed out by AllocatePage and every
// public method that takes a page_id rejects it.
//
// page_id 1 (CATALOG_ROOT_PAGE_ID) is permanently reserved as the catalog's root
// page, stamped PageType::CATALOG by Open(). Unlike page 0, it is not off-limits
// everywhere: ReadPage/WritePage allow it, since the catalog layer above needs to
// actually read and write its own root page. AllocatePage/DeallocatePage still
// reject it — it can never be handed out or freed, so its identity as the
// catalog's entry point is permanent for the lifetime of the file.
//
// Guarantees:
//  - AllocatePage/DeallocatePage are ordered so that a crash mid-operation can, at
//    worst, leak a page (it becomes unreachable but never corrupts the freelist
//    chain or produces a false corruption error on the next Open()).
//  - A page_id returned by AllocatePage always has an on-disk header stamped
//    PageType::ALLOCATED at the moment it's returned — it will never be mistaken
//    for FREE or for a real content type before the caller writes its own header.
//
// Explicitly NOT guaranteed:
//  - Thread safety. No method here is safe to call concurrently with another call
//    on the same DiskManager, including two calls touching different pages, since
//    freelist_head_/page_count_ are mutated without synchronization. Concurrency
//    control (per-page latching) belongs one layer up, in the buffer pool.
//  - Leak freedom. If a caller lets a page_id from AllocatePage go out of scope
//    without ever calling DeallocatePage, that page is gone for good — same
//    contract as malloc/free. DiskManager has no way to detect or reclaim it.
//  - Durability without an explicit Sync() call. Individual ReadPage/WritePage/
//    AllocatePage/DeallocatePage calls are not fsync'd; a crash can lose any
//    writes since the last Sync().
//  - Content integrity beyond the 24-byte PageHeader. DiskManager only ever reads
//    or writes a page's header fields itself; the remainder of a page's PAGE_SIZE
//    bytes is opaque to it and is not zeroed on deallocation, so a freed page's
//    body can still contain stale content until the next writer overwrites it.
class DiskManager {
  public:
	// Opens (creating if necessary) the file at `path`. Fails with Status::Corruption
	// if the file size isn't a multiple of PAGE_SIZE, if it has fewer than 2 pages,
	// or if page 0 isn't a valid META page or page 1 isn't a valid CATALOG page. On
	// a brand-new file, initializes page 0 as the meta page with an empty freelist
	// and page 1 as the catalog root. On an existing file, recovers freelist_head_
	// from page 0's stored value.
	static Result<std::unique_ptr<DiskManager>> Open(const std::filesystem::path& path);
	~DiskManager();

	DiskManager(const DiskManager&) = delete;
	DiskManager& operator=(const DiskManager&) = delete;
	DiskManager(DiskManager&&) = delete;
	DiskManager& operator=(DiskManager&&) = delete;

	// Reads exactly PAGE_SIZE bytes from `page_id` into `out`. Fails on an
	// out-of-range page_id or on META_PAGE_ID (the reserved meta page is not
	// readable through the public API). CATALOG_ROOT_PAGE_ID is readable like any
	// other page. Does not interpret or validate the bytes read — that's the
	// caller's responsibility.
	Status ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> out);

	// Writes exactly PAGE_SIZE bytes from `in` to `page_id`, overwriting the entire
	// page including whatever header DiskManager itself last wrote there (e.g. the
	// PageType::ALLOCATED stamp from AllocatePage). Fails on an out-of-range
	// page_id or on META_PAGE_ID. CATALOG_ROOT_PAGE_ID is writable like any other
	// page — the catalog layer owns its content.
	Status WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> in);

	// Returns a page_id the caller now owns: either reused from the freelist or
	// obtained by extending the file. The returned page's on-disk header is
	// PageType::ALLOCATED; its body is not initialized to anything meaningful.
	// The caller must eventually pair this with a DeallocatePage call, or the page
	// is leaked for the lifetime of the file.
	[[nodiscard]]
	Result<page_id_t> AllocatePage();

	// Returns `page_id` to the freelist. No-op if the page is already free. Fails
	// on an out-of-range page_id, on META_PAGE_ID, or on CATALOG_ROOT_PAGE_ID (the
	// catalog root can never be freed or reused, even though it's readable/writable
	// through ReadPage/WritePage). Does not clear the page's body — only its header
	// is overwritten to mark it FREE.
	Status DeallocatePage(page_id_t page_id);

	// fsyncs the underlying file descriptor. This is the only durability
	// mechanism DiskManager provides; no other method calls fsync on its own.
	Status Sync();

	// Returns the total number of pages in the file, including the reserved meta
	// and catalog root pages.
	page_id_t PageCount() const;

  private:
	DiskManager(int fd, std::filesystem::path path, page_id_t page_count);

	[[nodiscard]]
	bool valid_page(page_id_t page_id);

	// bounds check + rejects META_PAGE_ID; used by ReadPage/WritePage so callers can
	// never access the reserved superblock directly. Deliberately does NOT reject
	// CATALOG_ROOT_PAGE_ID — the catalog layer needs Read/Write access to its own
	// root page. DeallocatePage guards against CATALOG_ROOT_PAGE_ID separately.
	[[nodiscard]]
	Status validate_page_access(page_id_t page_id);

	[[nodiscard]]
	Result<PageHeader> read_page_header(page_id_t page_id);
	Status write_page_header(page_id_t page_id, const PageHeader& header);

	Status write_empty_page(page_id_t page_id);

	Status persist_freelist_head(page_id_t new_head);

	int fd_;
	std::filesystem::path path_;
	page_id_t freelist_head_{INVALID_PAGE};
	page_id_t page_count_{0};
};

}  // namespace kernsql

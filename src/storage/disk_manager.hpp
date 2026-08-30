#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
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
// Thread safety, precisely:
//  - AllocatePage/DeallocatePage are safe to call concurrently with each other and
//    with any other method here. Each holds meta_latch_ across its entire
//    read-modify-persist sequence, not just the freelist_head_ assignment: the
//    freelist walk and the page-0 head persist have to be atomic against a
//    concurrent allocation or deallocation. Two deallocations of the same page that
//    interleave would both pass the already-FREE check and thread the page onto the
//    list twice, producing a cycle — which surfaces much later as an allocation loop
//    that hands out the same page forever.
//  - ReadPage/WritePage/PageCount are deliberately latch-free, and stay that way.
//    pread/pwrite are atomic per call with respect to the file offset, and
//    page_count_ is atomic, so the bounds check races with nothing. Excluding two
//    callers from the *same* page is not this layer's problem — that is exactly what
//    the buffer pool's per-frame content latch is for (see DD-002).
//
// Explicitly NOT guaranteed:
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
	//
	// CONTRACT: `page_id` must not be resident in the buffer pool when this is
	// called. This method stamps the page's on-disk header FREE directly, so any
	// cached copy of that page still held in a frame is stale the instant this
	// returns — and a later flush of that frame would write the old header back over
	// the FREE stamp, resurrecting a page that is on the freelist and simultaneously
	// reachable as live data. Callers above this layer therefore never call this
	// directly; they go through BufferPoolManager::DeletePage, which vacates the
	// frame first and then calls here (see DD-002, "NewPage / DeletePage").
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

	// like valid_page, but also accepts page_id == page_count_ — the one-past-the-end
	// slot AllocatePage/Open() write into when extending the file by exactly one page.
	// Used by write_page_header/write_empty_page so a bad page_id (negative, or more
	// than one page past the current end) can never silently pwrite into or past
	// unrelated file content.
	[[nodiscard]]
	bool valid_write_target(page_id_t page_id);

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

	// Requires meta_latch_ held.
	Status persist_freelist_head(page_id_t new_head);

	// Byte offset of `page_id` in the file. Exists so the conversion is written
	// exactly once: every raw I/O in this class is a (PageOffset(id), span) pair, and
	// an offset computed ad hoc at each call site is a place to get the arithmetic
	// subtly wrong. The cast precedes the multiply on principle — the product is
	// already 64-bit here because PAGE_SIZE is std::size_t, but that is a property of
	// a constant in another header, not something this expression should depend on.
	// Callers are responsible for rejecting negative page_ids first (valid_page /
	// valid_write_target both do); a negative id here yields a negative offset.
	[[nodiscard]]
	static constexpr off_t PageOffset(page_id_t page_id) {
		return static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
	}

	// Every raw pread/pwrite in this class routes through these two. A short transfer
	// is legal for both syscalls and is not an error, and EINTR is a retry rather than
	// a failure — handling that at five separate call sites is five chances to get it
	// wrong. They also carry real errno text out, so an ENOSPC or EBADF is visible in
	// the Status instead of a generic "failed to write page".
	Status full_write(off_t off, std::span<const std::byte> buf);

	// As full_write, with one asymmetry: a zero-byte return from pread is EOF, not a
	// retry condition. It means the file is shorter than the caller's bounds check
	// believed, which is corruption; treating it as a short read and looping would
	// spin forever on a truncated file.
	Status full_read(off_t off, std::span<std::byte> buf);

	int fd_;
	std::filesystem::path path_;

	// Serializes the read-modify-persist sequences in AllocatePage/DeallocatePage.
	// See the thread-safety note in the class comment for why the scope is the whole
	// operation and not just the freelist_head_ store.
	std::mutex meta_latch_;

	// Guarded by meta_latch_.
	page_id_t freelist_head_{INVALID_PAGE};

	// Written only under meta_latch_, so the read-modify-write when AllocatePage
	// extends the file is serialized. Atomic rather than a plain int because it is
	// *read* without the latch, by valid_page/valid_write_target on the latch-free
	// ReadPage/WritePage path. Default (seq_cst) ordering is deliberate: the store
	// that publishes a new page must not be visible before the header write that
	// stamped it, or a concurrent reader passes the bounds check and reads a page
	// whose header has not been written yet. The load is a plain mov on x86 and the
	// store happens once per file extension, next to a syscall — the ordering is
	// free at this frequency.
	std::atomic<page_id_t> page_count_{0};
};

}  // namespace kernsql

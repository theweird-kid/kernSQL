#include "disk_manager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>

#include "common/logger.hpp"
#include "common/page_header.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

using namespace kernsql;

Result<std::unique_ptr<DiskManager>> DiskManager::Open(const std::filesystem::path& path) {
	int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
	// Fail fast if unable to open the file
	if (fd < 0) {
		LOG_DEBUG("failed to open file %s", path.c_str());
		return std::unexpected(Status::Internal("unable to open file at path: " + path.string()));
	}

	// check file stats
	struct stat buf;
	if (fstat(fd, &buf) < 0) {
		close(fd);
		LOG_DEBUG("failed to fstat file %s", path.c_str());
		return std::unexpected(Status::Internal("unable to stat file at path: " + path.string()));
	}
	off_t file_size = buf.st_size;

	// file corrupted
	if (file_size % static_cast<off_t>(PAGE_SIZE) != 0) {
		close(fd);
		LOG_DEBUG("file %s is corrupt", path.c_str());
		return std::unexpected(
		    Status::Corruption("the file at path " + path.string() + " is corrupt"));
	}

	page_id_t page_count = static_cast<page_id_t>(file_size / static_cast<off_t>(PAGE_SIZE));
	auto dm = std::unique_ptr<DiskManager>(new DiskManager(fd, path, page_count));

	if (page_count == 0) {
		// Brand-new file: reserve page 0 as the meta/superblock page.
		//
		// Every early return in this branch leaves a partially initialized file on
		// disk — one page long, or two pages with no freelist head persisted. The
		// next Open() sees page_count == 1 and reports Corruption rather than
		// silently repairing it. That is a deliberate v1 decision, not an oversight:
		// unwinding correctly would mean either unlinking a file the caller may not
		// have wanted us to delete, or a bootstrap-recovery path, and both are the
		// job of the WAL that does not exist yet. Crash-during-create is
		// unrecoverable-by-design until then; the file is safe to delete by hand.
		Status st = dm->write_empty_page(META_PAGE_ID);
		if (!st.ok()) {
			return std::unexpected(st);
		}
		// write meta page header
		PageHeader meta_header;
		meta_header.page_type = PageType::META;
		st = dm->write_page_header(META_PAGE_ID, meta_header);
		if (!st.ok()) {
			LOG_DEBUG("failed to write meta page header for %s", path.c_str());
			return std::unexpected(st);
		}
		// page 0 exists now — advance so write_empty_page/write_page_header will
		// accept page 1 as a valid append target next
		dm->page_count_ = 1;

		// reserve page 1 as the catalog head page
		st = dm->write_empty_page(CATALOG_ROOT_PAGE_ID);
		if (!st.ok()) {
			return std::unexpected(st);
		}
		// write catalog page header
		PageHeader catalog_header;
		catalog_header.page_type = PageType::CATALOG;
		st = dm->write_page_header(CATALOG_ROOT_PAGE_ID, catalog_header);
		if (!st.ok()) {
			LOG_DEBUG("failed to write catalog page header for %s", path.c_str());
			return std::unexpected(st);
		}

		// update page count
		dm->page_count_ = 2;

		// update freelist
		Status write_status = dm->persist_freelist_head(INVALID_PAGE);
		if (!write_status.ok()) {
			LOG_DEBUG("failed to write meta page header for %s", path.c_str());
			return std::unexpected(write_status);
		}
	} else if (page_count >= 2) {
		// existing file: page 0 must be the meta page; recover freelist_head_ from it
		auto meta = dm->read_page_header(META_PAGE_ID);
		if (!meta.has_value()) {
			return std::unexpected(meta.error());
		}
		if (meta.value().page_type != PageType::META) {
			LOG_DEBUG("file %s is corrupt: page 0 is not a meta page", path.c_str());
			return std::unexpected(Status::Corruption("the file at path " + path.string() +
			                                          " is corrupt: missing meta page"));
		}

		// validate catalog head page
		auto catalog_page = dm->read_page_header(CATALOG_ROOT_PAGE_ID);
		if (!catalog_page.has_value()) {
			return std::unexpected(catalog_page.error());
		}
		if (catalog_page.value().page_type != PageType::CATALOG) {
			LOG_DEBUG("file %s is corrupt: page 1 is not a catalog page", path.c_str());
			return std::unexpected(Status::Corruption("the file at path " + path.string() +
			                                          " is corrupt: missing catalog page"));
		}

		dm->freelist_head_ = meta.value().next_page_id;
	} else {
		LOG_DEBUG("db file corrupted!");
		return std::unexpected(Status::Corruption("db file is corrputed"));
	}

	LOG_INFO("Opening file %s", path.c_str());
	return dm;
}

DiskManager::DiskManager(int fd, std::filesystem::path path, page_id_t page_count)
    : fd_(fd), path_(std::move(path)), page_count_(page_count) {}

DiskManager::~DiskManager() {
	close(this->fd_);
	LOG_DEBUG("closing file %s", this->path_.c_str());
}

Status DiskManager::Sync() {
	if (fsync(this->fd_) < 0) {
		return Status::IOError("failed to flush to disk");
	}
	return Status::OK();
}

bool DiskManager::valid_page(page_id_t page_id) {
	return page_id >= 0 && page_id < this->page_count_;
}

bool DiskManager::valid_write_target(page_id_t page_id) {
	return page_id >= 0 && page_id <= this->page_count_;
}

Status DiskManager::validate_page_access(page_id_t page_id) {
	if (page_id == META_PAGE_ID) {
		return Status::InvalidArgument(std::format(
		    "page {} is the reserved meta page and cannot be accessed directly", page_id));
	}
	if (!valid_page(page_id)) {
		return Status::InvalidArgument(
		    std::format("invalid page id {}, page id < {}", page_id, this->page_count_.load()));
	}
	return Status::OK();
}

Status DiskManager::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> out) {
	if (Status s = validate_page_access(page_id); !s.ok()) {
		return s;
	}

	// No latch: pread is atomic per call with respect to the file offset, and the
	// bounds check above reads an atomic page_count_. Two callers touching the same
	// page is the buffer pool's problem, not ours.
	return full_read(PageOffset(page_id), out);
}

Status DiskManager::WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> in) {
	if (Status s = validate_page_access(page_id); !s.ok()) {
		return s;
	}

	// Latch-free for the same reason as ReadPage.
	return full_write(PageOffset(page_id), in);
}

Result<page_id_t> DiskManager::AllocatePage() {
	// Held for the whole operation, not just the freelist_head_ store: the read of
	// the head, the walk to its successor, the page-0 persist and the local update
	// have to be one indivisible step, or two allocators both read the same head and
	// both hand it out.
	std::scoped_lock lock(this->meta_latch_);

	PageHeader allocated_header;
	allocated_header.page_type = PageType::ALLOCATED;
	allocated_header.next_page_id = INVALID_PAGE;
	allocated_header.prev_page_id = INVALID_PAGE;

	if (freelist_head_ != INVALID_PAGE) {
		// Allocate Page from Free list
		page_id_t free_page = freelist_head_;
		auto ph = read_page_header(free_page);
		if (!ph.has_value()) {
			return std::unexpected(ph.error());
		}
		if (ph.value().page_type != PageType::FREE) {
			return std::unexpected(Status::Corruption(
			    std::format("page {} on freelist has unexpected page_type", free_page)));
		}
		page_id_t next_free = ph.value().next_page_id;

		// persist the advanced freelist head on page 0 *before* stamping free_page
		// as allocated: page 0 must never reference a page that isn't actually FREE
		// on disk, so the head pointer moves off free_page first
		Status meta_status = persist_freelist_head(next_free);
		if (!meta_status.ok()) {
			LOG_DEBUG("failed to persist freelist head while allocating page %d", free_page);
			return std::unexpected(meta_status);
		}

		Status write_status = write_page_header(free_page, allocated_header);
		if (!write_status.ok()) {
			LOG_DEBUG("failed to stamp allocated header for page %d", free_page);
			return std::unexpected(write_status);
		}

		freelist_head_ = next_free;
		return free_page;
	} else {
		// Allocate NEW Page by extending the file
		page_id_t new_page = this->page_count_;

		Status st = write_empty_page(new_page);
		if (!st.ok()) {
			return std::unexpected(st);
		}

		// Write Header
		st = write_page_header(new_page, allocated_header);
		if (!st.ok()) {
			LOG_DEBUG("failed to stamp allocated header for new page %d", new_page);
			return std::unexpected(st);
		}

		// Publish last. Until this store lands the page fails valid_page(), so a
		// concurrent ReadPage cannot observe a page whose header has not been
		// stamped yet — and an error on either write above leaves page_count_
		// untouched, so the half-written slot is simply retried by the next
		// allocation rather than becoming visible.
		this->page_count_ = new_page + 1;
		return new_page;
	}
}

Status DiskManager::DeallocatePage(page_id_t page_id) {
	// Taken before the already-FREE check below, not after it. That check is a read
	// of on-disk state that the rest of this function then acts on, so it is the
	// start of the read-modify-write, not a precondition outside it: two concurrent
	// deallocations of the same page would otherwise both observe a non-FREE header,
	// both proceed, and thread the page onto the freelist twice. The result is a
	// cycle in the chain, which does not fail here — it fails much later as an
	// allocation loop that returns the same page forever.
	std::scoped_lock lock(this->meta_latch_);

	if (page_id == CATALOG_ROOT_PAGE_ID) {
		LOG_DEBUG("Can't deallocate page %d: reserved catalog root page", page_id);
		return Status::InvalidArgument(std::format(
		    "page {} is the reserved catalog root page and cannot be deallocated", page_id));
	}
	if (Status s = validate_page_access(page_id); !s.ok()) {
		LOG_DEBUG("Can't deallocate page %d", page_id);
		return s;
	}
	// read page header
	auto ph = read_page_header(page_id);
	if (!ph.has_value()) {
		LOG_DEBUG("failed to read header for page %d during deallocate", page_id);
		return ph.error();
	}
	if (ph.value().page_type == PageType::FREE) return Status::OK();  // no-op on already Free Page

	// write page_id's FREE header *before* page 0 is made to point at it: page 0
	// must never reference a page that isn't actually FREE on disk yet
	PageHeader free_header;
	free_header.page_type = PageType::FREE;
	free_header.next_page_id = this->freelist_head_;
	free_header.prev_page_id = INVALID_PAGE;

	Status write_status = write_page_header(page_id, free_header);
	if (!write_status.ok()) {
		LOG_DEBUG("failed to write free header for page %d", page_id);
		return write_status;
	}

	Status meta_status = persist_freelist_head(page_id);
	if (!meta_status.ok()) {
		LOG_DEBUG("failed to persist freelist head while deallocating page %d", page_id);
		return meta_status;
	}

	this->freelist_head_ = page_id;
	return Status::OK();
}

Result<PageHeader> DiskManager::read_page_header(page_id_t page_id) {
	if (!valid_page(page_id)) {
		return std::unexpected(Status::InvalidArgument("invalid page"));
	}
	std::array<std::byte, PAGE_HEADER_SIZE> buf;
	if (Status s = full_read(PageOffset(page_id), buf); !s.ok()) {
		return std::unexpected(s);
	}

	return PageHeader::ReadFrom(buf);
}

Status DiskManager::write_page_header(page_id_t page_id, const PageHeader& header) {
	if (!valid_write_target(page_id)) {
		return Status::InvalidArgument(std::format("invalid page id {} for write, page id <= {}",
		                                           page_id, this->page_count_.load()));
	}
	// Identity is stamped HERE, not by callers. Five sites write headers (both reserved pages
	// in Open, AllocatePage, DeallocatePage, persist_freelist_head) and every one of them owes
	// the same two fields. A page whose header does not carry its own id is indistinguishable
	// from a page that landed at the wrong offset, so a single forgetful caller silently
	// disables the check for that page forever. One place that knows beats five that remember.
	PageHeader stamped = header;
	stamped.page_id = page_id;
	stamped.format_version = PAGE_FORMAT_VERSION;

	std::array<std::byte, PAGE_HEADER_SIZE> buf;
	stamped.WriteTo(buf);
	if (Status s = full_write(PageOffset(page_id), buf); !s.ok()) {
		LOG_DEBUG("failed to write page header for page %d", page_id);
		return s;
	}
	return Status::OK();
}

Status DiskManager::write_empty_page(page_id_t page_id) {
	if (!valid_write_target(page_id)) {
		return Status::InvalidArgument(std::format("invalid page id {} for write, page id <= {}",
		                                           page_id, this->page_count_.load()));
	}
	std::array<std::byte, PAGE_SIZE> empty_buf{};
	if (Status s = full_write(PageOffset(page_id), empty_buf); !s.ok()) {
		LOG_DEBUG("failed to write empty page %d", page_id);
		return s;
	}
	return Status::OK();
}

Status DiskManager::persist_freelist_head(page_id_t new_head) {
	PageHeader meta_header;
	meta_header.page_type = PageType::META;
	meta_header.next_page_id = new_head;
	meta_header.prev_page_id = INVALID_PAGE;
	return write_page_header(META_PAGE_ID, meta_header);
}

page_id_t DiskManager::PageCount() const {
	// No latch needed: page_count_ is atomic precisely so this and the bounds checks
	// on the ReadPage/WritePage path stay off meta_latch_.
	return page_count_.load();
}

Status DiskManager::full_write(off_t off, std::span<const std::byte> buf) {
	std::size_t done = 0;
	while (done < buf.size()) {
		ssize_t n =
		    pwrite(this->fd_, buf.data() + done, buf.size() - done, off + static_cast<off_t>(done));
		if (n < 0) {
			// Capture errno before anything else can clobber it — std::format and
			// Status construction are both allowed to make library calls.
			int err = errno;
			if (err == EINTR) continue;
			return Status::IOError(
			    std::format("pwrite at offset {} failed after {} of {} bytes: {}", off, done,
			                buf.size(), std::system_category().message(err)));
		}
		done += static_cast<std::size_t>(n);
	}
	return Status::OK();
}

Status DiskManager::full_read(off_t off, std::span<std::byte> buf) {
	std::size_t done = 0;
	while (done < buf.size()) {
		ssize_t n =
		    pread(this->fd_, buf.data() + done, buf.size() - done, off + static_cast<off_t>(done));
		if (n < 0) {
			int err = errno;
			if (err == EINTR) continue;
			return Status::IOError(std::format("pread at offset {} failed after {} of {} bytes: {}",
			                                   off, done, buf.size(),
			                                   std::system_category().message(err)));
		}
		if (n == 0) {
			// EOF with bytes still outstanding. Unlike a short read this is terminal:
			// the file is shorter than the caller's bounds check believed, so looping
			// would never make progress. Corruption rather than IOError — nothing
			// went wrong at the syscall level, the file is simply not the size the
			// page count says it is.
			return Status::Corruption(std::format(
			    "unexpected EOF at offset {}: wanted {} bytes, got {}", off, buf.size(), done));
		}
		done += static_cast<std::size_t>(n);
	}
	return Status::OK();
}

#include "disk_manager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <span>

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
		// brand new file: reserve page 0 as the meta/superblock page
		std::array<std::byte, PAGE_SIZE> empty_buf{};
		if (pwrite(fd, empty_buf.data(), PAGE_SIZE, 0) != static_cast<ssize_t>(PAGE_SIZE)) {
			LOG_DEBUG("failed to initialize meta page for %s", path.c_str());
			return std::unexpected(Status::IOError("unable to initialize meta page"));
		}
		dm->page_count_ = 1;

		Status write_status = dm->persist_freelist_head(INVALID_PAGE);
		if (!write_status.ok()) {
			LOG_DEBUG("failed to write meta page header for %s", path.c_str());
			return std::unexpected(write_status);
		}
	} else {
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
		dm->freelist_head_ = meta.value().next_page_id;
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

Status DiskManager::validate_page_access(page_id_t page_id) {
	if (page_id == META_PAGE_ID) {
		return Status::InvalidArgument(std::format(
		    "page {} is the reserved meta page and cannot be accessed directly", page_id));
	}
	if (!valid_page(page_id)) {
		return Status::InvalidArgument(
		    std::format("invalid page id {}, page id < {}", page_id, this->page_count_));
	}
	return Status::OK();
}

Status DiskManager::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> out) {
	if (Status s = validate_page_access(page_id); !s.ok()) {
		return s;
	}

	ssize_t status =
	    pread(this->fd_, out.data(), PAGE_SIZE, static_cast<off_t>(page_id * PAGE_SIZE));
	if (status != static_cast<ssize_t>(PAGE_SIZE)) {
		return Status::IOError("failed to read page");
	}

	return Status::OK();
}

Status DiskManager::WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> in) {
	if (Status s = validate_page_access(page_id); !s.ok()) {
		return s;
	}

	ssize_t status =
	    pwrite(this->fd_, in.data(), PAGE_SIZE, static_cast<off_t>(page_id * PAGE_SIZE));
	if (status != static_cast<ssize_t>(PAGE_SIZE)) {
		return Status::IOError("failed to write to page");
	}

	return Status::OK();
}

Result<page_id_t> DiskManager::AllocatePage() {
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
		std::array<std::byte, PAGE_SIZE> empty_buf{};
		if (pwrite(this->fd_, empty_buf.data(), PAGE_SIZE,
		           static_cast<off_t>(page_count_ * PAGE_SIZE)) !=
		    static_cast<ssize_t>(PAGE_SIZE)) {
			return std::unexpected(Status::IOError("unable to allocate page"));
		}

		Status write_status = write_page_header(page_count_, allocated_header);
		if (!write_status.ok()) {
			LOG_DEBUG("failed to stamp allocated header for new page %d", page_count_);
			return std::unexpected(write_status);
		}
		return this->page_count_++;
	}
}

Status DiskManager::DeallocatePage(page_id_t page_id) {
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
	ssize_t status = pread(this->fd_, buf.data(), PAGE_HEADER_SIZE, page_id * PAGE_SIZE);
	if (status != static_cast<ssize_t>(PAGE_HEADER_SIZE)) {
		return std::unexpected(Status::IOError("unable to read page header"));
	}

	return PageHeader::ReadFrom(buf);
}

Status DiskManager::write_page_header(page_id_t page_id, const PageHeader& header) {
	std::array<std::byte, PAGE_HEADER_SIZE> buf;
	header.WriteTo(buf);
	if (pwrite(this->fd_, buf.data(), PAGE_HEADER_SIZE, static_cast<off_t>(page_id * PAGE_SIZE)) !=
	    static_cast<ssize_t>(PAGE_HEADER_SIZE)) {
		return Status::Internal(std::format("unable to write page header for page {}", page_id));
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
	return page_count_;
}

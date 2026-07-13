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
#include <print>
#include <span>

#include "common/page_header.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

using namespace kernsql;

Result<std::unique_ptr<DiskManager>> DiskManager::Open(const std::filesystem::path& path) {
	int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
	// Fail fast if unable to open the file
	if (fd < 0) {
		return std::unexpected(Status::Internal("unable to open file at path: " + path.string()));
	}

	// check file stats
	struct stat buf;
	if (fstat(fd, &buf) < 0) {
		close(fd);
		return std::unexpected(Status::Internal("unable to stat file at path: " + path.string()));
	}
	off_t file_size = buf.st_size;

	// file corrupted
	if (file_size % static_cast<off_t>(PAGE_SIZE) != 0) {
		close(fd);
		return std::unexpected(
		    Status::Corruption("the file at path " + path.string() + " is corrupt"));
	}

	page_id_t page_count = static_cast<page_id_t>(file_size / static_cast<off_t>(PAGE_SIZE));
	auto dm = new DiskManager(fd, path, page_count);
	return std::unique_ptr<DiskManager>(dm);
}

DiskManager::DiskManager(int fd, std::filesystem::path path, page_id_t page_count)
    : fd_(fd), path_(std::move(path)), page_count_(page_count) {}

DiskManager::~DiskManager() {
	close(this->fd_);
	std::println("closed file at path: {}", this->path_.string());
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

Status DiskManager::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> out) {
	if (!valid_page(page_id)) {
		return Status::InvalidArgument(
		    std::format("invalid page id {}, page id < {}", page_id, this->page_count_));
	}

	ssize_t status =
	    pread(this->fd_, out.data(), PAGE_SIZE, static_cast<off_t>(page_id * PAGE_SIZE));
	if (status != static_cast<ssize_t>(PAGE_SIZE)) {
		return Status::IOError("failed to read page");
	}

	return Status::OK();
}

Status DiskManager::WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> in) {
	if (!valid_page(page_id)) {
		return Status::InvalidArgument(
		    std::format("invalid page id {}, page id < {}", page_id, this->page_count_));
	}

	ssize_t status =
	    pwrite(this->fd_, in.data(), PAGE_SIZE, static_cast<off_t>(page_id * PAGE_SIZE));
	if (status != static_cast<ssize_t>(PAGE_SIZE)) {
		return Status::IOError("failed to write to page");
	}

	return Status::OK();
}

Result<page_id_t> DiskManager::AllocatePage() {
	if (freelist_head_ != INVALID_PAGE) {
		page_id_t free_page = freelist_head_;
		auto ph = read_page_header(free_page);
		if (ph.has_value()) {
			if (ph.value().page_type != PageType::FREE) {
				return std::unexpected(Status::Corruption(
				    std::format("page {} on freelist has unexpected page_type", free_page)));
			}
			// update freelist_head
			freelist_head_ = ph.value().next_page_id;
			return free_page;
		}
		return std::unexpected(ph.error());
	} else {
		std::array<std::byte, PAGE_SIZE> empty_buf{};
		if (pwrite(this->fd_, empty_buf.data(), PAGE_SIZE,
		           static_cast<off_t>(page_count_ * PAGE_SIZE)) !=
		    static_cast<ssize_t>(PAGE_SIZE)) {
			return std::unexpected(Status::IOError("unable to allocate page"));
		}
		// TODO: Write Page as Free Page, since the zeroed page acts like PageType::META
		return this->page_count_++;
	}
}

Status DiskManager::DeallocatePage(page_id_t page_id) {}

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

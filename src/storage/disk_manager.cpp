#include "disk_manager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <print>

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

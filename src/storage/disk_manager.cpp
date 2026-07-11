#include "disk_manager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <expected>
#include <filesystem>
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

Status DiskManager::ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> out) {}
Status DiskManager::WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> in) {}

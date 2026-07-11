#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

#include "common/status.hpp"
#include "common/types.hpp"

namespace kernsql {

class DiskManager {
  public:
	static Result<std::unique_ptr<DiskManager>> Open(const std::filesystem::path& path);
	~DiskManager();
	DiskManager(const DiskManager&) = delete;
	DiskManager& operator=(const DiskManager&) = delete;
	DiskManager(DiskManager&&) = delete;
	DiskManager& operator=(DiskManager&&) = delete;

	Status ReadPage(page_id_t page_id, std::span<std::byte, PAGE_SIZE> out);
	Status WritePage(page_id_t page_id, std::span<const std::byte, PAGE_SIZE> in);
	[[nodiscard]] Result<page_id_t> AllocatePage();
	Status DeallocatePage(page_id_t page_id);
	Status Sync();
	page_id_t PageCount() const;

  private:
	DiskManager(int fd, std::filesystem::path path, page_id_t page_count);

	int fd_;
	std::filesystem::path path_;
	page_id_t page_count_{0};
};

}  // namespace kernsql

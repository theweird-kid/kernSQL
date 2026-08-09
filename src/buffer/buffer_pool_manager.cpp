#include "buffer_pool_manager.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>

#include "buffer/freelist.hpp"
#include "buffer/replacer.hpp"

using namespace kernsql;

BufferPoolManager::BufferPoolManager(DiskManager& disk_manager, std::size_t capacity)
    : frames_(std::make_unique<Frame[]>(capacity)),
      capacity_(capacity),
      free_list_(capacity),
      replacer_(capacity),
      disk_manager_(disk_manager) {
	assert(capacity > 0);
}

BufferPoolManager::~BufferPoolManager() {}

void BufferPoolManager::UnpinPage(frame_id_t frame_id) {
	auto& frame = frames_[frame_id];

	bool evictable{false};
	{
		std::lock_guard lock(frame.mtx_);
		frame.pin_count_ = std::max(frame.pin_count_ - 1, 0);
		evictable = (frame.pin_count_ == 0);
	}

	if (evictable) replacer_.SetEvictable(frame_id, true);
}

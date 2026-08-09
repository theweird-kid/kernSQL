#include "buffer_pool_manager.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "buffer/frame.hpp"
#include "buffer/freelist.hpp"
#include "buffer/replacer.hpp"
#include "common/logger.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

using namespace kernsql;

BufferPoolManager::BufferPoolManager(DiskManager& disk_manager, std::size_t capacity)
    : frames_(std::make_unique<Frame[]>(capacity)),
      capacity_(capacity),
      free_list_(capacity),
      replacer_(capacity),
      disk_manager_(disk_manager) {
	assert(capacity > 0);
}

BufferPoolManager::~BufferPoolManager() {
	if (shutdown_) return;
	LOG_INFO("Shutdown() was never called; flushing from ~BufferPoolManager()");
	Status st = Shutdown();
	if (!st.ok()) {
		// %.*s, not %s: message() is a std::string_view — a pointer/length pair, not a
		// null-terminated char*. Passing it to a printf-style variadic is undefined even though
		// the view happens to be backed by a std::string today.
		const auto msg = st.message();
		LOG_INFO("shutdown flush failed in ~BufferPoolManager, aborting: %.*s",
		         static_cast<int>(msg.size()), msg.data());
		std::abort();
	}
}

Status BufferPoolManager::FlushPage(page_id_t page_id) {
	frame_id_t frame_id;
	{
		auto shard = page_table_.AcquireShard(page_id);
		auto found = shard.Find(page_id);
		if (!found) return Status::OK();  // nothing cached so no Flush required
		frame_id = *found;

		auto& frame = FrameAt(frame_id);
		std::lock_guard lock(frame.mtx_);
		if (frame.state_ != FrameState::Resident) return Status::OK();
		frame.pin_count_ += 1;
	}

	Status st = FlushFrame(frame_id, page_id);
	UnpinPage(frame_id);
	return st;
}

Status BufferPoolManager::FlushAllPages() {
	Status st = Status::OK();
	const auto n = static_cast<frame_id_t>(capacity_);

	for (frame_id_t frame_id = 0; frame_id < n; ++frame_id) {
		auto& frame = FrameAt(frame_id);

		page_id_t page_id;
		{
			std::lock_guard lock(frame.mtx_);
			if (frame.state_ != FrameState::Resident) continue;
			page_id = frame.page_id_;
			frame.pin_count_ += 1;
		}

		auto s = FlushFrame(frame_id, page_id);
		UnpinPage(frame_id);
		if (st.ok() && !s.ok()) st = s;  // cache the first error
	}
	return st;
}

Status BufferPoolManager::FlushFrame(frame_id_t frame_id, page_id_t page_id) {
	auto& frame = FrameAt(frame_id);
	auto st = Status::OK();
	if (!frame.IsDirty()) return st;

	uint64_t D;
	{
		auto rd_latch = frame.AcquireRead();
		D = frame.SampleDirtyEpoch();
		st = disk_manager_.WritePage(page_id, frame.data_);
	}
	if (!st.ok()) return st;
	frame.MarkFlushed(D);

	return st;
}

Status BufferPoolManager::Shutdown() {
	if (shutdown_) return Status::OK();
	shutdown_ = true;  // the attempt has been made

	LOG_INFO("BufferPoolManager::Shutdown() starting");

	// Verify that each frame is unpinned and not on FrameState::Loading
	const auto n = static_cast<frame_id_t>(capacity_);
	for (frame_id_t frame_id = 0; frame_id < n; frame_id++) {
		auto& frame = FrameAt(frame_id);
		{
			std::lock_guard lock(frame.mtx_);
			// LOG_INFO, and ahead of the assert: LOG_DEBUG and assert are both gated on NDEBUG,
			// so pairing them yields a report that is unreachable in debug (the assert aborts
			// first) and compiled out in release. A quiescence violation means a guard outlived
			// the pool — undefined behaviour already in flight — so it has to be diagnosable in
			// both builds.
			if (frame.pin_count_ != 0 || frame.state_ == FrameState::Loading) {
				LOG_INFO("frame %d is not quiesced: pin_count = %d, state = %s", frame_id,
				         frame.pin_count_, FrameStateName(frame.state_));
			}
			assert(frame.pin_count_ == 0 && frame.state_ != FrameState::Loading);
		}
	}

	Status st = FlushAllPages();
	Status sync = disk_manager_.Sync();  // unconditional
	if (st.ok()) st = sync;
	return st;
}

void BufferPoolManager::UnpinPage(frame_id_t frame_id) {
	auto& frame = FrameAt(frame_id);

	bool evictable{false};
	{
		std::lock_guard lock(frame.mtx_);

		if (frame.state_ == FrameState::Failed) frame.cv_.notify_all();
		assert(frame.pin_count_ > 0);

		evictable = ((frame.pin_count_ == 1) && (frame.state_ == FrameState::Resident));
		frame.pin_count_ -= 1;
		if (frame.pin_count_ < 0) {
			LOG_INFO("frame %d pin count is dropped to %d", frame_id, frame.pin_count_);
		}
	}

	if (evictable) replacer_.SetEvictable(frame_id, true);
}

#include "buffer_pool_manager.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
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

Result<frame_id_t> BufferPoolManager::FetchFrame(page_id_t page_id) {
	// A loop, not straight-line code: the miss path releases the shard lock to acquire a frame,
	// and another thread can publish this same page in that window. The only correct response is
	// to hand our frame back and start over from the lookup.
	for (;;) {
		// -1 rather than left indeterminate: every path below assigns it before use, and if one
		// ever stops doing so, FrameAt's bounds assert fires immediately instead of silently
		// indexing an unrelated frame.
		frame_id_t frame_id{-1};

		// Whether *we* are the pin that took this frame from 0 to 1. Only that thread owes the
		// replacer a SetEvictable(false); a frame already pinned by someone else left the
		// candidate set when they pinned it.
		bool from_zero{false};

		// Declared HERE, outside the block below, and this placement is the whole trick. The
		// lookup must hold the shard lock and the metadata mutex simultaneously — dropping the
		// shard lock before pinning would let a reclaimer take the frame between Find() and the
		// increment. But the Loading wait must NOT hold the shard lock (DD-002: never block on a
		// condvar while holding one), and ShardGuard exposes no early unlock. Declaring `meta`
		// first means the ShardGuard dies at the end of the block while this lock lives on.
		//
		// unique_lock rather than lock_guard because cv_.wait() requires one.
		std::unique_lock<std::mutex> meta;
		{
			auto shard = page_table_.AcquireShard(page_id);
			auto found = shard.Find(page_id);
			if (found) {
				frame_id = *found;
				auto& frame = FrameAt(frame_id);
				meta = std::unique_lock(frame.mtx_);  // shard -> metadata, the canonical order

				// Pin BEFORE inspecting state_ — the pin invariant, not an optimization. The
				// pin makes AbandonLoad (which waits for pin_count == 1, its own) unable to
				// dispose of this frame, so its identity is fixed across the wait below and
				// nothing has to be re-validated on wake.
				from_zero = (frame.pin_count_ == 0);
				frame.pin_count_ += 1;
			}
		}  // shard released here; `meta` survives if we hit

		// owns_lock() is the hit/miss discriminator: we take the metadata mutex only on a hit.
		if (meta.owns_lock()) {
			auto& frame = FrameAt(frame_id);

			// Predicate form handles both a spurious wakeup and the already-published case.
			frame.cv_.wait(meta, [&] { return frame.state_ != FrameState::Loading; });

			// Free is unreachable: a frame is unmapped in the same critical section that turns it
			// Free, so the shard lookup above could not have returned it — and our pin has kept
			// it that way ever since.
			assert(frame.state_ == FrameState::Resident || frame.state_ == FrameState::Failed);

			if (frame.state_ == FrameState::Failed) {
				// The loader's read failed and it owns disposal; we only drop the pin we took.
				// unlock() first is mandatory, not tidiness: UnpinPage takes this same mutex and
				// std::mutex is not recursive. UnpinPage is also what notifies the loader that a
				// waiter has left, which is how AbandonLoad's pin_count == 1 wait terminates.
				meta.unlock();
				UnpinPage(frame_id);
				return std::unexpected(
				    Status::IOError(std::format("unable to fetch frame {}", frame_id)));
			}

			// Release before touching the replacer — DD-002 forbids calling into it while holding
			// a frame's metadata mutex. This admits a brief window where the frame is still marked
			// evictable despite being pinned; that is harmless because reclaim re-validates
			// pin_count == 0 under the metadata mutex and simply declines.
			meta.unlock();
			replacer_.RecordAccess(frame_id);
			if (from_zero) replacer_.SetEvictable(frame_id, false);
			return frame_id;
		}

		// --- Cache miss. We reach here holding NO lock, which is required rather than incidental:
		// AcquireFrame may reclaim, and re-validating a victim needs the *victim's* shard lock,
		// about which our own shard lock says nothing.

		auto found_frame = AcquireFrame();
		if (!found_frame.has_value()) {
			// kBufferPoolFull, propagated. Nothing was acquired, so nothing is owed.
			return std::unexpected(found_frame.error());
		}
		// Free, unmapped, unpinned, and out of both the free list and the candidate set — private
		// to this thread until we publish it below.
		frame_id = found_frame.value();

		{
			auto shard = page_table_.AcquireShard(page_id);

			// Look up AGAIN. We had no shard lock across AcquireFrame, so another thread may have
			// published this page in the meantime. Loading it twice would give two frames the same
			// identity.
			auto found = shard.Find(page_id);
			if (found.has_value()) {
				// Lost the race. Our frame goes back to the FREE LIST, not the replacer: it holds
				// no page, and a Free frame is stock rather than an eviction candidate. Pushing
				// under the shard lock is safe — the free-list lock is a leaf that nothing is ever
				// held across, so no cycle is possible.
				free_list_.Push(frame_id);
				continue;
			}

			// Publish the mapping BEFORE the read. This is what makes a concurrent fetcher of this
			// page take the hit path and sleep on Loading instead of starting a second, redundant
			// read of the same page into a second frame.
			shard.Insert(page_id, frame_id);
			auto& frame = FrameAt(frame_id);

			// A scoped lock, deliberately NOT the function-scope `meta`: that one outlives this
			// block, so assigning to it would leave the metadata mutex held across the read below
			// — and the re-lock after the read would then deadlock against ourselves.
			std::lock_guard lock(frame.mtx_);
			frame.page_id_ = page_id;
			frame.state_ = FrameState::Loading;
			frame.pin_count_ = 1;  // a store, not an increment: the frame was Free and ours alone
			frame.ResetEpochs();   // stale epochs from the previous occupant would read as dirty
		}  // both released

		// No lock and no content latch. Safe because the frame is Loading: no guard for it can
		// exist (guards only come from this function returning), and every other fetcher of this
		// page is asleep on the condvar rather than touching data_.
		auto& frame = FrameAt(frame_id);
		auto st = disk_manager_.ReadPage(page_id, frame.data_);
		if (!st.ok()) {
			// The loader owns disposal. AbandonLoad erases the mapping, publishes Failed, waits
			// for the waiters' pins to drain, and returns the frame to the free list. Do not unpin
			// or push here as well — that would be a double release.
			AbandonLoad(frame_id, page_id);
			return std::unexpected(st);
		}

		{
			// Publish the contents. notify_all under the mutex so no waiter can check the
			// predicate and sleep between our store and the notification.
			std::lock_guard lock(frame.mtx_);
			frame.state_ = FrameState::Resident;
			frame.cv_.notify_all();
		}

		// No SetEvictable(false) on this path: the frame came from the free list or a reclaim, so
		// it was never in the candidate set to begin with.
		replacer_.RecordAccess(frame_id);
		return frame_id;  // pin_count == 1, and it is the caller's
	}
}

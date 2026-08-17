#include "buffer_pool_manager.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
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

Result<frame_id_t> BufferPoolManager::AcquireFrame() {
	auto frame_id = free_list_.TryPop();
	if (frame_id.has_value()) return frame_id.value();
	return ReclaimFrame();
}

Result<frame_id_t> BufferPoolManager::ReclaimFrame() {
	for (;;) {
		auto victim = replacer_.Evict();
		if (!victim.has_value()) {
			return std::unexpected(Status::BufferPoolFull("buffer full"));
		}

		auto& frame = FrameAt(victim.value());

		// Peek the victim's identity. A shard is keyed by page_id, which lives under the
		// metadata mutex — and the canonical order forbids taking a shard lock while holding
		// one. So read it here, release, and lock in order below. This value is nothing but a
		// hint about WHICH shard to take; every predicate tested here is tested again under
		// both locks, page_id_ included.
		page_id_t peeked{INVALID_PAGE};
		{
			std::lock_guard lock(frame.mtx_);
			if (frame.state_ != FrameState::Resident || frame.pin_count_ != 0)
				continue;  // Delete page raced this thread
			peeked = frame.page_id_;
		}

		// Scoped, because ShardGuard has no early unlock: the dirty path below must reach its
		// flush with the shard lock already gone, and the end of this block is the only thing
		// that releases it. Falling out of the block therefore means exactly one thing — the
		// victim was dirty and we pinned it. Every other outcome returns or continues from
		// inside.
		{
			auto shard = page_table_.AcquireShard(peeked);
			std::lock_guard lock(frame.mtx_);

			// Re-validate: the metadata mutex was released across the peek, so the frame may
			// have been re-pinned, deleted, or repurposed into a different page since. A
			// declined victim is a normal outcome, not an error — it stays out of the
			// candidate set until its next unpin re-adds it.
			if (frame.state_ != FrameState::Resident || frame.pin_count_ != 0 ||
			    frame.page_id_ != peeked)
				continue;

			// Implied rather than checked: mappings change only under the shard lock we now
			// hold, and every path that erases peeked's mapping leaves Resident in that same
			// critical section. A mismatch means an invariant broke elsewhere.
			assert(shard.Find(peeked) == victim);

			// The clean-check is exact, not conservative. Nothing can newly dirty the frame —
			// that needs a guard, which needs the mapping we hold the shard lock for — and
			// nothing already dirtied is missed, since a guard's release bumps dirty_epoch_
			// strictly before it decrements pin_count_.
			if (!frame.IsDirty()) {
				// The entire reclaim, with no I/O in it: one critical section, so no other
				// thread can observe a frame mid-reclaim and there is nothing to cancel.
				shard.Erase(peeked);
				frame.page_id_ = INVALID_PAGE;
				frame.state_ = FrameState::Free;
				frame.ResetEpochs();  // stale epochs would read as dirty for the next occupant
				return victim.value();
			}

			// Dirty. Pin across the writeback: the pin, not any lock, is what fixes the
			// frame's identity while we hold no lock at all. The mapping stays live, so a
			// concurrent fetcher of this page takes the hit path and pins — and we see
			// pin_count_ > 1 at re-validation below and decline.
			frame.pin_count_ += 1;
		}  // both released

		Status st = FlushFrame(victim.value(), peeked);
		if (!st.ok()) {
			// Deliberately NOT another trip round the loop. UnpinPage hands this frame back to
			// the candidate set on its 1->0 transition, so the next Evict() can return it
			// straight back to us — flush, fail, repeat. A persistently unwritable page would
			// turn reclaim into a livelock rather than an error, so drop the pin and let the
			// caller's fetch fail with the I/O error it actually hit.
			UnpinPage(victim.value());
			return std::unexpected(st);
		}

		{
			auto shard = page_table_.AcquireShard(peeked);
			std::lock_guard lock(frame.mtx_);

			// Same checks, with two changes: pin_count_ == 1 is now the condition, that one
			// being ours, and IsDirty() is asked again because a writer can have dirtied the
			// page while the write was in flight.
			if (frame.state_ == FrameState::Resident && frame.page_id_ == peeked &&
			    frame.pin_count_ == 1 && !frame.IsDirty()) {
				assert(shard.Find(peeked) == victim);
				shard.Erase(peeked);
				frame.page_id_ = INVALID_PAGE;
				frame.state_ = FrameState::Free;
				frame.ResetEpochs();

				// Our own pin dies as part of the same stamp that unmaps the frame, NOT
				// through UnpinPage. This is the narrow exception to "UnpinPage is the only
				// place a pin is decremented" — that rule is about releasing a pin while the
				// frame keeps its identity. Here the frame is becoming free-list stock, and
				// UnpinPage would publish it to the replacer as an eviction candidate on the
				// 1->0 transition, so TryPop and Evict could hand the same frame to two
				// threads. AbandonLoad drops its pin the same way and for the same reason.
				frame.pin_count_ = 0;
				return victim.value();
			}
		}  // both released

		// Lost it during the writeback — someone pinned it, or dirtied it again. Wasted work,
		// nothing corrupted (DD-002, "Known behaviour"). UnpinPage only after both locks are
		// gone: it may call into the replacer, and DD-002 keeps the replacer lock off any path
		// that already holds a shard lock or a metadata mutex.
		UnpinPage(victim.value());
	}
}

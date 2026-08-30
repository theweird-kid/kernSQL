#include "buffer_pool_manager.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <mutex>
#include <span>

#include "buffer/frame.hpp"
#include "buffer/freelist.hpp"
#include "buffer/pool_stats.hpp"
#include "buffer/replacer.hpp"
#include "common/logger.hpp"
#include "common/page_header.hpp"
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

PoolStats BufferPoolManager::GetStats() const {
	PoolStats stats{};
	stats.capacity = capacity_;

	const auto n = static_cast<frame_id_t>(capacity_);
	for (frame_id_t frame_id = 0; frame_id < n; ++frame_id) {
		const auto& frame = FrameAt(frame_id);

		// One frame's mutex at a time, released before the next is taken. Never two at once,
		// and no shard lock anywhere near this — a census that held locks across frames would
		// be a new lock-ordering edge for a diagnostic, which is a bad trade.
		std::lock_guard lock(frame.mtx_);
		switch (frame.state_) {
			case FrameState::Free:
				++stats.free_frames;
				break;
			case FrameState::Loading:
				++stats.loading_frames;
				break;
			case FrameState::Resident:
				++stats.resident_frames;
				break;
			case FrameState::Failed:
				++stats.failed_frames;
				break;
		}
		if (frame.pin_count_ > 0) ++stats.pinned_frames;
	}

	// AFTER the loop, deliberately: DD-002 forbids calling into the free list or the replacer
	// while holding a frame's metadata mutex. Both take their own leaf lock, so reading them
	// here adds no ordering constraint.
	stats.free_list_size = free_list_.Size();
	stats.evictable = replacer_.EvictableCount();

	return stats;
}

void BufferPoolManager::UnpinPage(frame_id_t frame_id) {
	auto& frame = FrameAt(frame_id);

	std::lock_guard lock(frame.mtx_);

	if (frame.state_ == FrameState::Failed) frame.cv_.notify_all();
	assert(frame.pin_count_ > 0);

	const bool evictable = ((frame.pin_count_ == 1) && (frame.state_ == FrameState::Resident));
	frame.pin_count_ -= 1;
	if (frame.pin_count_ < 0) {
		LOG_INFO("frame %d pin count is dropped to %d", frame_id, frame.pin_count_);
	}

	// Under the metadata mutex, and it is the ONE exception to DD-002's "never call into the
	// replacer while holding a frame's metadata mutex" (see "Lock ordering", which now names it).
	//
	// Publishing this outside the mutex splits the decision from the act, and DeletePage fits in
	// the gap: it takes the mutex, sees the pin_count == 0 we just produced, vacates the frame,
	// calls SetEvictable(false) -- which removes nothing, because our `true` has not landed yet --
	// and pushes the frame to the free list. Our `true` then arrives at a frame that is now free
	// stock, leaving it on the free list and in the candidate set at once. That is exactly the
	// double membership DeletePage's own comment says must never exist, and only ReclaimFrame's
	// re-validation of `state == Resident` keeps it from becoming two threads on one frame.
	//
	// The 0->1 direction in FetchFrame needs no such treatment and keeps the deferred form. It is
	// not symmetric: the only thing that ADDS a frame to the candidate set is an unpin, and in
	// that window the caller holds the pin, so no unpin can happen and no straggler can undo the
	// SetEvictable(false).
	//
	// Safe as an exception because Replacer is a strict leaf -- every method takes only its own
	// mutex and never reaches back into a frame, a shard, or the free list. The edge this adds is
	// metadata -> replacer, and nothing anywhere goes the other way.
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

		// Validate what came back BEFORE publishing it. Effectively free: the 4 KB read already
		// happened, so this is two comparisons against bytes already in cache. Deliberately not
		// done on the hit path — a resident page was validated when it missed.
		//
		// FREE catches a fetch of a deallocated page. That is a caller bug (nothing should hold
		// a page id after deleting it), but without the check the page is silently re-cached,
		// and the next AllocatePage to recycle that id finds a stale mapping — two frames
		// claiming one page. It narrows rather than closes the window: a fetch that lands
		// between DeletePage's mapping erase and its DeallocatePage still reads the old header
		// and passes. The full guarantee has to come from the layer above never asking for a
		// page it deleted.
		//
		// A page_id mismatch catches a different class: a misdirected write, an off-by-one in
		// page arithmetic, a torn extend, a file copied at the wrong offset. The offset says
		// which page we asked for and the header says which page the bytes think they are —
		// only two independently derived answers can disagree, which is the whole reason the
		// id is stored despite being implied by the offset.
		const auto header = PageHeader::ReadFrom(std::span(frame.data_).first<PAGE_HEADER_SIZE>());
		if (header.page_type == PageType::FREE || header.page_id != page_id) {
			Status bad =
			    (header.page_type == PageType::FREE)
			        ? Status::InvalidArgument(std::format("page {} is not allocated", page_id))
			        : Status::Corruption(std::format("page {} header claims to be page {}", page_id,
			                                         header.page_id));
			// Same disposal path as a read error: the frame is Loading, mapped and pinned, and
			// may have waiters asleep on it. Waiters get the generic Failed-path error rather
			// than this one — a pre-existing property of that path, not new here.
			AbandonLoad(frame_id, page_id);
			return std::unexpected(bad);
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

void BufferPoolManager::AbandonLoad(frame_id_t frame_id, page_id_t page_id) {
	{
		auto shard = page_table_.AcquireShard(page_id);
		shard.Erase(page_id);
	}

	auto& frame = FrameAt(frame_id);
	{
		std::unique_lock<std::mutex> lock(frame.mtx_);
		frame.state_ = FrameState::Failed;
		frame.cv_.notify_all();

		assert(frame.page_id_ >= 1);
		frame.cv_.wait(lock, [&] { return frame.pin_count_ == 1; });

		frame.page_id_ = INVALID_PAGE;
		frame.state_ = FrameState::Free;
		frame.pin_count_ = 0;
		frame.ResetEpochs();
	}

	free_list_.Push(frame_id);
}

Result<ReadPageGuard> BufferPoolManager::FetchPageRead(page_id_t page_id) {
	auto frame_found = FetchFrame(page_id);
	if (!frame_found.has_value()) return std::unexpected(frame_found.error());

	auto& frame = FrameAt(frame_found.value());

	return ReadPageGuard(this, &frame, frame_found.value(), page_id, frame.AcquireRead());
}

Result<WritePageGuard> BufferPoolManager::FetchPageWrite(page_id_t page_id) {
	auto frame_found = FetchFrame(page_id);
	if (!frame_found.has_value()) return std::unexpected(frame_found.error());

	auto& frame = FrameAt(frame_found.value());

	return WritePageGuard(this, &frame, frame_found.value(), page_id, frame.AcquireWrite());
}

Result<WritePageGuard> BufferPoolManager::NewPage() {
	auto frame_found = AcquireFrame();
	if (!frame_found.has_value()) {
		return std::unexpected(frame_found.error());
	}

	auto page_allocated = disk_manager_.AllocatePage();
	if (!page_allocated.has_value()) {
		free_list_.Push(frame_found.value());  // cleanup the acquired frame
		return std::unexpected(page_allocated.error());
	}

	page_id_t page_id = page_allocated.value();

	auto& frame = FrameAt(frame_found.value());
	{
		// The frame is unreachable through the page table and the replacer, so no other
		// FETCHER can find it -- but GetStats walks every frame BY INDEX and reads state_ and
		// pin_count_ under this mutex, so "private to this thread" does not cover it. Without
		// the lock these three stores race a concurrent census. FetchFrame's miss path takes
		// the same lock for the same stores.
		std::lock_guard lock(frame.mtx_);
		frame.page_id_ = page_id;
		frame.state_ = FrameState::Resident;
		frame.pin_count_ = 1;
	}

	// data_ needs no lock: nothing can reach this frame's bytes until the mapping is published
	// below, and a guard only ever comes from a successful page-table lookup.
	frame.data_.fill(std::byte{0});

	// Stamp the page's own identity. Zeroing left page_id at 0 — which is META_PAGE_ID — and a
	// page that lies about which page it is defeats the entire point of storing the id.
	// DiskManager stamps this on every header it writes; NewPage is the one path that produces
	// page contents without ever reading them, so it owes the same. page_type is ALLOCATED, the
	// "handed out but not yet given a purpose" state (InnoDB spells it FIL_PAGE_TYPE_ALLOCATED),
	// matching what AllocatePage already stamped on disk. Everything past the header stays the
	// caller's business (DD-003) — this is identity, not semantics.
	PageHeader header;
	header.page_type = PageType::ALLOCATED;
	header.page_id = page_id;
	header.WriteTo(std::span(frame.data_).first<PAGE_HEADER_SIZE>());
	frame.ResetEpochs();

	{
		auto shard = page_table_.AcquireShard(page_id);
		shard.Insert(page_id, frame_found.value());
	}

	replacer_.RecordAccess(frame_found.value());

	return WritePageGuard(this, &frame, frame_found.value(), page_id, frame.AcquireWrite());
}

Status BufferPoolManager::DeletePage(page_id_t page_id) {
	// Checked BEFORE anything destructive. DeallocatePage rejects these two ids, and reaching
	// that rejection at the end of this function would mean we had already vacated the frame —
	// evicting the catalog root from the pool and returning an error, for a call that could
	// never have succeeded. Failing here leaves nothing to undo.
	if (page_id == META_PAGE_ID || page_id == CATALOG_ROOT_PAGE_ID) {
		return Status::InvalidArgument(
		    std::format("page {} is reserved and cannot be deleted", page_id));
	}

	// Held for the WHOLE operation, deallocation included, rather than released after the erase.
	// That is the fix for the stale-mapping race (DD-002, "Known behaviour"): between erasing the
	// mapping and stamping the header FREE, a fetcher that missed could publish a fresh Loading
	// mapping for this page, read a header still marked ALLOCATED, pass the miss-path validation
	// at FetchFrame's read check, and leave the pool caching a page that is on the disk freelist.
	// The next AllocatePage then hands that id out again and the new owner's fetch is a cache hit
	// on the dead page's bytes. Holding the shard is the whole guarantee: no new mapping for this
	// page can be published until the header says FREE, and then the validation rejects it.
	//
	// This is the second named exception in DD-002's "Lock ordering": it blocks on I/O while
	// holding a shard lock. Affordable because the I/O is bounded and this path is not hot —
	// DeallocatePage is three syscalls with no fsync, and it already serializes every deleter and
	// allocator in the process on DiskManager's global meta_latch_, so the shard lock adds only
	// the other pages in this one shard, for a span the call already spends. New ordering edge,
	// shard -> DiskManager::meta_latch_, closes no cycle: DiskManager knows nothing about the
	// page table and can never reach back for a shard.
	auto shard = page_table_.AcquireShard(page_id);

	// -1 means "not cached" — the page may legitimately have no frame, in which case there is
	// nothing to vacate and only the disk side needs doing.
	frame_id_t frame_id{-1};
	{
		auto found = shard.Find(page_id);

		if (found.has_value()) {
			frame_id = *found;
			auto& frame = FrameAt(frame_id);
			std::lock_guard lock(frame.mtx_);  // shard -> metadata, the canonical order

			// Reject rather than wait. Waiting would deadlock outright when the caller is
			// deleting a page it still holds a guard on, and nothing at this layer detects
			// that. It also fires spuriously while a reclaimer holds a transient pin across a
			// dirty victim's writeback (DD-002, "Known behaviour") — that case is retryable and
			// this one is not, and a single status cannot tell the caller which it hit.
			//
			// The pin check subsumes a state check: Loading and Failed frames both carry at
			// least the loader's own pin, so neither can reach the vacate below.
			if (frame.pin_count_ != 0) {
				return Status::InvalidArgument(
				    std::format("page {} is pinned ({} holders) and cannot be deleted", page_id,
				                frame.pin_count_));
			}
			assert(frame.state_ == FrameState::Resident);

			// Dirty contents are discarded WITHOUT a flush, deliberately. Flushing would write
			// bytes into a page we are about to stamp FREE, and race that stamp while doing it.
			// This is the one place in the component where a dirty frame owes no writeback.
			frame.page_id_ = INVALID_PAGE;
			frame.state_ = FrameState::Free;
			frame.ResetEpochs();
			shard.Erase(page_id);
		}
	}  // frame metadata mutex released; the shard lock is still held, deliberately

	if (frame_id != -1) {
		// Mandatory, and the only place in the component that needs it. A Resident frame with
		// pin_count == 0 is in the replacer's candidate set by definition, so pushing it to the
		// free list while it is still a candidate would leave it in both structures — and
		// TryPop and Evict could then hand the same frame to two threads. Every other path that
		// frees a frame gets one that already left the set: Evict removed ReclaimFrame's, and
		// AbandonLoad's was never in it.
		replacer_.SetEvictable(frame_id, false);
	}

	// Vacated first, which is what satisfies DeallocatePage's not-resident contract
	// (disk_manager.hpp). It stamps the on-disk header FREE directly, and a later flush of a
	// still-resident frame would write the old header back over that stamp — leaving a page
	// that is on the disk freelist and reachable as live data at the same time. Once the frame
	// is Free, FlushAllPages skips it, so no flush can ever reach this page again.
	//
	// Still under the shard lock. See the acquisition above for why that matters.
	Status st = disk_manager_.DeallocatePage(page_id);

	// Returned regardless of the deallocation result. The frame is already Free, unmapped and
	// out of the candidate set, so it is usable stock either way; withholding it because the
	// disk call failed would shrink the pool permanently for a reason that has nothing to do
	// with the frame.
	if (frame_id != -1) free_list_.Push(frame_id);

	return st;
}

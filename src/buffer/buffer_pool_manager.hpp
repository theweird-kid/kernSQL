#pragma once

#include <cassert>
#include <cstddef>
#include <memory>

#include "buffer/frame.hpp"
#include "buffer/freelist.hpp"
#include "buffer/page_guard.hpp"
#include "buffer/page_table.hpp"
#include "buffer/pool_stats.hpp"
#include "buffer/replacer.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

// The caching layer over DiskManager (DD-002). Hands out RAII page guards, never raw frames.
//
// The guards live in page_guard.hpp with this class forward-declared, and call the private
// UnpinPage on release; they are friends of this class. UnpinPage is deliberately not
// public — two owners of one pin is a double-decrement waiting to happen.
//
// There are no background threads. A miss pops free_list_, and when that is empty it reclaims
// a frame inline: replacer_.Evict() picks a victim, then one critical section under that
// victim's page-table shard lock and metadata mutex re-validates it (Resident, unpinned,
// clean, still mapped here) and turns it Free. A dirty victim is pinned, flushed with no lock
// held, and then re-validated. Because the reclaim itself performs no I/O, no other thread can
// observe a frame mid-reclaim and nothing needs to cancel it.
class BufferPoolManager {
  public:
	// Allocates the frame array. free_list_ stocks itself with every frame_id in its own
	// constructor, so a fully-stocked free list is an invariant of that type rather than
	// something this constructor has to remember to establish. Definitions still to be written.
	BufferPoolManager(DiskManager& disk_manager, std::size_t capacity);

	// A backstop, NOT the durability mechanism — that is Shutdown(), below.
	//
	// If Shutdown() already ran, this does nothing. If it did not, the caller has a bug: a
	// destructor cannot return a Status or throw, so it is structurally the wrong place to decide
	// what a failed flush means. It logs, flushes best-effort, and aborts rather than silently
	// discarding dirty pages.
	//
	// Requires quiescence, which it verifies rather than arranges (see Shutdown()). Nothing to
	// join or signal — the pool owns no threads.
	~BufferPoolManager();

	BufferPoolManager(const BufferPoolManager&) = delete;
	BufferPoolManager& operator=(const BufferPoolManager&) = delete;
	BufferPoolManager(BufferPoolManager&&) = delete;
	BufferPoolManager& operator=(BufferPoolManager&&) = delete;

	// --- The frozen public API (DD-002, "Page guards") ---

	// Pin `page_id` and return it latched in the named mode. A hit pins before it waits, so the
	// frame's identity is provably unchanged across the wait; a miss acquires a frame, publishes
	// the mapping as Loading, and reads with no lock held. Errors: kIOError from the read,
	// kBufferPoolFull when every frame is pinned.
	//
	// Two entry points rather than one mode flag: the mode is what picks the guard type, so a
	// runtime parameter would force a return type that can hold either.
	[[nodiscard]] Result<ReadPageGuard> FetchPageRead(page_id_t page_id);
	[[nodiscard]] Result<WritePageGuard> FetchPageWrite(page_id_t page_id);

	// Allocates a page on disk and returns it zeroed and write-latched. No Loading phase — there
	// is nothing on disk to read — and no re-lookup race, since a page_id nobody has seen yet
	// cannot already be mapped.
	[[nodiscard]] Result<WritePageGuard> NewPage();

	// Writes the page back if dirty. Pins for the duration, so it is safe against a concurrent
	// reclaim. Not an error if the page is absent or already clean.
	Status FlushPage(page_id_t page_id);

	// A sequential loop over every frame. Best-effort and NOT atomic across frames: a page
	// dirtied after its own frame has been visited is not flushed, so this is a checkpoint
	// primitive only in the presence of external quiescence. Returns the first error, having
	// still attempted the rest.
	Status FlushAllPages();

	// Drops the page from the pool and deallocates it on disk. Dirty contents are discarded
	// without a flush — the page is going away. Rejects with kInvalidArgument if the frame is
	// pinned rather than waiting for the pin to clear, which makes deleting a page you still
	// hold a guard on an error instead of a deadlock. See "Known behaviour" in DD-002: this can
	// also fail spuriously against a reclaimer's transient pin.
	Status DeletePage(page_id_t page_id);

	// The durable shutdown operation (DD-002, "Shutdown and destruction"). Idempotent — a second
	// call is a no-op, not an error.
	//
	// Verifies quiescence (every frame unpinned and not Loading), runs FlushAllPages(), then
	// DiskManager::Sync(). Both steps matter: FlushAllPages only moves page bytes into the OS
	// page cache via pwrite, which is not durability.
	//
	// Returns Status rather than living in the destructor precisely so a failed flush is the
	// caller's decision — exit non-zero, refuse to mark the database cleanly closed — instead of
	// a log line nobody reads.
	//
	// Quiescence is a PRECONDITION this verifies, not one it arranges. Destroying or shutting
	// down a pool another thread is still using is undefined regardless of what happens here: a
	// live guard holds a BufferPoolManager* that dangles the moment the pool goes away, and its
	// later Drop() unpins through freed memory. The owner establishes quiescence by joining every
	// worker thread — guards are stack-scoped, so an unwound stack has released every pin, and
	// join() supplies the synchronizes-with edge (DD-003). A non-zero pin here is therefore proof
	// that a guard outlived the pool; assert on it rather than working around it.
	[[nodiscard]] Status Shutdown();

	// Pool census. See PoolStats above for its two caveats — O(capacity), and a sample rather
	// than a snapshot.
	[[nodiscard]] PoolStats GetStats() const;

  private:
	// The guards construct themselves through this class's private constructors and call
	// UnpinPage below on release, so the friendship goes both ways.
	friend class ReadPageGuard;
	friend class WritePageGuard;

	// Steps 3-5 of the frozen guard release sequence (see page_guard.hpp). Under the frame's
	// metadata mutex: notify all waiters if the frame is Failed, note whether this is a 1->0
	// transition on a Resident frame, decrement. Release the mutex, and only then call
	// replacer_.SetEvictable(frame_id, true) if it transitioned — never while still holding the
	// metadata mutex, per DD-002's lock ordering.
	//
	// Both halves of that condition are load-bearing. Testing the post-decrement value instead of
	// the transition marks a frame evictable on a spurious unpin at pin_count == 0; testing the
	// pin without the state admits a Free frame — one already on the free list — into the
	// candidate set, so TryPop and Evict can hand the same frame to two threads.
	//
	// The Failed notify is why this must be the ONLY place a pin is ever decremented, including
	// on the failed-load path where a waiter drops the pin it took before sleeping. A second
	// decrement site is a site that has to remember the notify, and forgetting it parks the
	// loader on the condvar forever.
	//
	// Underflow is a caller bug, not a condition to absorb: assert in debug, and log at a level
	// that survives NDEBUG. In release the count goes negative and the frame is stuck for good —
	// never 1->0 again, so never re-evictable, and DeletePage rejects it forever. That is a
	// capacity leak rather than corruption, which is the only direction safe to be wrong in.
	//
	// Private on purpose. Two owners of one pin, a guard and a caller, is a double-decrement
	// waiting to happen, so nothing above this layer ever unpins by hand.
	void UnpinPage(frame_id_t frame_id);

	// The whole of FetchPage — hit path, miss path, Loading wait, failed-load handling — with
	// the content latch deliberately left un-acquired. Returns a frame that is pinned, Resident,
	// and already through RecordAccess/SetEvictable(false); the caller acquires the latch in its
	// own mode and wraps the result in the matching guard.
	//
	// The split is where it is because DD-002's ordering demands it: the latch is taken last,
	// strictly after both the metadata mutex and the replacer call are done with. That leaves
	// nothing mode-specific in the protocol itself, so FetchPageRead and FetchPageWrite differ
	// only in their final two lines rather than duplicating the miss path.
	//
	// The caller owns the returned pin from the moment this succeeds: every path out of it,
	// including the latch acquisition throwing, must unpin or the frame is stranded.
	[[nodiscard]] Result<frame_id_t> FetchFrame(page_id_t page_id);

	// Miss step 1: free_list_.TryPop(), else ReclaimFrame(). Returns a frame in state Free that
	// is private to this thread — unmapped, unpinned, and out of both the free list and the
	// replacer's candidate set.
	[[nodiscard]] Result<frame_id_t> AcquireFrame();

	// The inline reclaim loop of DD-002, "Reclaiming a frame". Entered holding no other lock,
	// because re-validating a victim needs the *victim's* shard lock, which the caller's own
	// shard lock says nothing about. Loops over victims until one re-validates; kBufferPoolFull
	// when the replacer has no candidate left.
	[[nodiscard]] Result<frame_id_t> ReclaimFrame();

	// The failed-load path. Named for what it is — the loader's obligation, not a general
	// cleanup any thread may run. Erases the mapping, publishes Failed, then waits for
	// pin_count to fall to 1 (its own) before resetting to Free and pushing to the free list,
	// NOT to the replacer: a Free frame is not a candidate, it is stock.
	//
	// Exactly one thread per Loading episode may call this, and it must be the thread that
	// published Loading. Waiters observing Failed drop their pins and return the error; they
	// dispose of nothing.
	void AbandonLoad(frame_id_t frame_id, page_id_t page_id);

	// The five-step flush sequence (DD-002). Caller must already hold a pin — this takes the
	// shared content latch and issues I/O, and an unpinned frame can be repurposed underneath
	// both. Every flush path in this class routes through here so the epoch sampling and the
	// CAS-upward on flushed_epoch_ exist in exactly one place.
	Status FlushFrame(frame_id_t frame_id, page_id_t page_id);

	[[nodiscard]] Frame& FrameAt(frame_id_t frame_id) {
		assert(frame_id >= 0 && static_cast<std::size_t>(frame_id) < capacity_);
		return frames_[static_cast<std::size_t>(frame_id)];
	}

	[[nodiscard]] const Frame& FrameAt(frame_id_t frame_id) const {
		assert(frame_id >= 0 && static_cast<std::size_t>(frame_id) < capacity_);
		return frames_[static_cast<std::size_t>(frame_id)];
	}

	// One contiguous allocation made once at construction, per DD-002: Frame owns mutexes
	// and a condition_variable, so it is neither copyable nor movable and cannot live in a
	// container that might reallocate.
	std::unique_ptr<Frame[]> frames_;
	std::size_t capacity_;

	FreeList free_list_;
	PageTable page_table_;
	Replacer replacer_;
	DiskManager& disk_manager_;

	// Makes Shutdown() idempotent and tells the destructor whether it is the backstop path.
	// Deliberately a plain bool, not an atomic: Shutdown() runs only under the quiescence it
	// verifies, so there is no second thread to race with. An atomic here would advertise a
	// thread-safety this operation does not have and cannot have.
	bool shutdown_{false};
};

}  // namespace kernsql

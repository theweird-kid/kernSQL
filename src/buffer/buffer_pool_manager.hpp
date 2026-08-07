#pragma once

#include <cstddef>
#include <memory>

#include "buffer/frame.hpp"
#include "buffer/freelist.hpp"
#include "buffer/page_table.hpp"
#include "buffer/replacer.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

// The caching layer over DiskManager (DD-002). Hands out RAII page guards, never raw frames.
//
// Public API, frozen in DD-002 — still to be implemented:
//
//   Result<ReadPageGuard>  FetchPageRead(page_id_t page_id);
//   Result<WritePageGuard> FetchPageWrite(page_id_t page_id);
//   Result<WritePageGuard> NewPage();
//   Status FlushPage(page_id_t page_id);
//   Status FlushAllPages();
//   Status DeletePage(page_id_t page_id);
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
	// Allocates the frame array and stocks free_list_ with every frame_id. Definitions still
	// to be written.
	BufferPoolManager(DiskManager& disk_manager, std::size_t capacity);

	// Flushes every dirty frame. Nothing to join or signal — the pool owns no threads.
	~BufferPoolManager();

	BufferPoolManager(const BufferPoolManager&) = delete;
	BufferPoolManager& operator=(const BufferPoolManager&) = delete;
	BufferPoolManager(BufferPoolManager&&) = delete;
	BufferPoolManager& operator=(BufferPoolManager&&) = delete;

  private:
	// The guards construct themselves through this class's private constructors and call
	// UnpinPage below on release, so the friendship goes both ways.
	friend class ReadPageGuard;
	friend class WritePageGuard;

	// Steps 3-5 of the frozen guard release sequence (see page_guard.hpp): take the frame's
	// metadata mutex, decrement pin_count guarding against underflow, note a 1->0 transition,
	// release the mutex, and only then call replacer_.SetEvictable(frame_id, true) — never
	// while still holding the metadata mutex, per DD-002's lock ordering.
	//
	// Private on purpose. Two owners of one pin, a guard and a caller, is a double-decrement
	// waiting to happen, so nothing above this layer ever unpins by hand.
	void UnpinPage(frame_id_t frame_id);

	// One contiguous allocation made once at construction, per DD-002: Frame owns mutexes
	// and a condition_variable, so it is neither copyable nor movable and cannot live in a
	// container that might reallocate.
	std::unique_ptr<Frame[]> frames_;
	std::size_t capacity_;

	FreeList free_list_;
	PageTable page_table_;
	Replacer replacer_;
	DiskManager& disk_manager_;
};

}  // namespace kernsql

#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "buffer/frame.hpp"
#include "common/types.hpp"

namespace kernsql {

// BufferPoolManager returns guards and the guards call back into it on release, so the
// dependency is genuinely circular. It is broken the standard way: forward-declare here, and
// define the release path out of line in page_guard.cpp, which includes the manager's header.
class BufferPoolManager;

// A ReadPageGuard/WritePageGuard owns a pin and a content latch together, and releases both
// when it dies (DD-002, "Page guards"). The pool never hands out a raw Frame*: a single missed
// unpin on an early-return or exception path does not fail loudly, it permanently pins one
// frame and silently shrinks the pool for the process's lifetime, with a symptom that shows up
// arbitrarily far from the leak.
//
// Guards are latched from birth, in the mode the caller named at fetch time. There is no
// pin-only guard and no read->write upgrade — a shared->unique upgrade has to drop the shared
// side, and anything the caller established while holding the read latch is void across that
// gap. Code that discovers it needs write access must drop the guard, re-fetch for write, and
// re-validate.
//
// RELEASE SEQUENCE (frozen in DD-002 — the order is load-bearing, do not rearrange):
//
//   1. WritePageGuard only: bump dirty_epoch.
//   2. Release the content latch.
//   3. Metadata mutex; decrement pin_count; note a 1->0 transition.
//   4. Release the metadata mutex.
//   5. If it transitioned, SetEvictable(frame_id, true).
//
// Steps 3-5 live in BufferPoolManager::UnpinPage, which is private and which these two classes
// are friends of. Step 1 precedes step 2 so a flusher holding the shared latch always observes
// an epoch that already accounts for every write it is about to capture; step 1 precedes step 3
// so a reclaimer that sees pin_count == 0 under the metadata mutex is guaranteed to see this
// guard's dirty bump too, which is what makes its clean-check exact rather than merely
// conservative. Between steps 2 and 3 the frame cannot be reclaimed: the pin is still held, and
// it is the pin, not the latch, that protects a frame's identity.

class ReadPageGuard {
  public:
	// Guards are only ever produced by a fetch, so there is no public constructor and no
	// default constructor — a guard that holds nothing is not a state any caller should be
	// able to create.
	ReadPageGuard() = delete;

	// Non-copyable: a copy would decrement the same pin twice.
	ReadPageGuard(const ReadPageGuard&) = delete;
	ReadPageGuard& operator=(const ReadPageGuard&) = delete;

	// Movable, since fetches return by value. The moved-from guard is left inert.
	ReadPageGuard(ReadPageGuard&& other) noexcept
	    : bpm_(std::exchange(other.bpm_, nullptr)),
	      frame_(std::exchange(other.frame_, nullptr)),
	      frame_id_(other.frame_id_),
	      page_id_(other.page_id_),
	      latch_(std::move(other.latch_)) {}

	ReadPageGuard& operator=(ReadPageGuard&& other) noexcept;

	~ReadPageGuard() { Drop(); }

	// Releases the pin and the latch, in the order documented above. Explicit because latch
	// crabbing has to release the parent before the child's scope ends, not at scope exit.
	// Idempotent, and implicitly called by the destructor — calling it twice, or letting a
	// dropped guard die, is well-defined and does nothing the second time.
	void Drop();

	// The page this guard holds. Stored by value at construction, deliberately never read back
	// from Frame::page_id_: that field is guarded by the frame's metadata mutex, so reading it
	// through an accessor that takes no lock is a data race (and a tsan report). It cannot
	// change while the guard lives anyway — a pinned frame is never repurposed.
	[[nodiscard]] page_id_t PageId() const { return page_id_; }

	// Valid for the guard's lifetime. Const-only: it is the type system, not a runtime mode
	// check, that stops a reader writing.
	[[nodiscard]] std::span<const std::byte, PAGE_SIZE> Data() const { return frame_->data_; }

  private:
	friend class BufferPoolManager;

	ReadPageGuard(BufferPoolManager* bpm, Frame* frame, frame_id_t frame_id, page_id_t page_id,
	              Frame::ReadLatch latch)
	    : bpm_(bpm),
	      frame_(frame),
	      frame_id_(frame_id),
	      page_id_(page_id),
	      latch_(std::move(latch)) {}

	// Null once dropped or moved from, which is exactly what makes Drop() idempotent and a
	// moved-from guard's destructor inert.
	BufferPoolManager* bpm_;
	Frame* frame_;
	frame_id_t frame_id_;
	page_id_t page_id_;
	Frame::ReadLatch latch_;
};

class WritePageGuard {
  public:
	WritePageGuard() = delete;

	WritePageGuard(const WritePageGuard&) = delete;
	WritePageGuard& operator=(const WritePageGuard&) = delete;

	WritePageGuard(WritePageGuard&& other) noexcept
	    : bpm_(std::exchange(other.bpm_, nullptr)),
	      frame_(std::exchange(other.frame_, nullptr)),
	      frame_id_(other.frame_id_),
	      page_id_(other.page_id_),
	      latch_(std::move(other.latch_)) {}

	WritePageGuard& operator=(WritePageGuard&& other) noexcept;

	~WritePageGuard() { Drop(); }

	// As ReadPageGuard::Drop, plus step 1: this bumps the frame's dirty_epoch before releasing
	// the latch. The bump is UNCONDITIONAL — holding a write guard is itself the declaration of
	// intent, and no attempt is made to track whether a write actually landed. That
	// over-approximates dirtiness, costing at most a redundant flush for a write guard that
	// wrote nothing. The precise alternative is an explicit MarkDirty() the caller must
	// remember, which reintroduces the exact class of bug guards exist to eliminate, with data
	// loss rather than a wasted write as its failure mode.
	void Drop();

	[[nodiscard]] page_id_t PageId() const { return page_id_; }

	[[nodiscard]] std::span<const std::byte, PAGE_SIZE> Data() const { return frame_->data_; }
	[[nodiscard]] std::span<std::byte, PAGE_SIZE> MutableData() { return frame_->data_; }

  private:
	friend class BufferPoolManager;

	WritePageGuard(BufferPoolManager* bpm, Frame* frame, frame_id_t frame_id, page_id_t page_id,
	               Frame::WriteLatch latch)
	    : bpm_(bpm),
	      frame_(frame),
	      frame_id_(frame_id),
	      page_id_(page_id),
	      latch_(std::move(latch)) {}

	BufferPoolManager* bpm_;
	Frame* frame_;
	frame_id_t frame_id_;
	page_id_t page_id_;
	Frame::WriteLatch latch_;
};

}  // namespace kernsql

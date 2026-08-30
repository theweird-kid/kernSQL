#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "buffer/frame.hpp"
#include "common/page_header.hpp"
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

	// The page BODY — everything after the 32-byte header. Deliberately not the whole page:
	// see WritePageGuard::MutableBody for why the header is unreachable as raw bytes.
	// Const-only here; it is the type system, not a runtime mode check, that stops a reader
	// writing.
	[[nodiscard]] std::span<const std::byte, PAGE_BODY_SIZE> Body() const {
		return std::span<const std::byte, PAGE_SIZE>(frame_->data_)
		    .subspan<PAGE_HEADER_SIZE, PAGE_BODY_SIZE>();
	}

	// The header, BY VALUE. A copy, so there is no reference to write through.
	//
	// Header().page_id and PageId() answer different questions and should always agree:
	// PageId() is what the buffer pool believes this frame holds, Header().page_id is what
	// the bytes claim. Disagreement means the page was clobbered after it was validated.
	[[nodiscard]] PageHeader Header() const {
		return PageHeader::ReadFrom(
		    std::span<const std::byte, PAGE_SIZE>(frame_->data_).first<PAGE_HEADER_SIZE>());
	}

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

	[[nodiscard]] std::span<const std::byte, PAGE_BODY_SIZE> Body() const {
		return std::span<const std::byte, PAGE_SIZE>(frame_->data_)
		    .subspan<PAGE_HEADER_SIZE, PAGE_BODY_SIZE>();
	}

	// The only mutable view a caller ever gets, and it starts AFTER the header.
	//
	// This is what makes the miss-path validation in FetchFrame trustworthy rather than
	// hopeful. If a caller could name all PAGE_SIZE bytes, then `memcpy(page, buf, PAGE_SIZE)`
	// — the most natural thing in the world to write in a heap or B+tree layer — would
	// overwrite page_id and page_type, and the page would fail validation on its next miss,
	// long after the code that broke it ran. Handing out only the body means that line cannot
	// be written. Same reasoning as guards existing at all: delete the class of bug rather
	// than document it.
	[[nodiscard]] std::span<std::byte, PAGE_BODY_SIZE> MutableBody() {
		return std::span(frame_->data_).subspan<PAGE_HEADER_SIZE, PAGE_BODY_SIZE>();
	}

	[[nodiscard]] PageHeader Header() const {
		return PageHeader::ReadFrom(
		    std::span<const std::byte, PAGE_SIZE>(frame_->data_).first<PAGE_HEADER_SIZE>());
	}

	// The header fields a layer above owns, exposed one at a time rather than as a whole
	// PageHeader. A SetHeader(const PageHeader&) would reopen exactly the hole this class
	// closes — a caller could write a page_id that disagrees with the frame's.
	//
	// Deliberately absent: page_id and format_version belong to DiskManager and NewPage,
	// checksum to whoever implements it, page_lsn to a WAL that does not exist. None of them
	// are a caller's to set, so none of them have a setter.
	void SetPageType(PageType type) {
		PageHeader header = Header();
		header.page_type = type;
		WriteHeader(header);
	}
	void SetFlags(uint8_t flags) {
		PageHeader header = Header();
		header.flags = flags;
		WriteHeader(header);
	}
	void SetNextPageId(page_id_t next) {
		PageHeader header = Header();
		header.next_page_id = next;
		WriteHeader(header);
	}
	void SetPrevPageId(page_id_t prev) {
		PageHeader header = Header();
		header.prev_page_id = prev;
		WriteHeader(header);
	}

  private:
	friend class BufferPoolManager;

	// Private, so the only way to write the header is through the typed setters above — which
	// is what keeps the fields a caller does not own untouchable.
	//
	// Deliberately not a template taking a lambda. Inside a member template, `.first<N>()` on
	// a CTAD-deduced span is treated as a dependent template name by GCC and needs a
	// `template` disambiguator; clang accepts it without. A portability trap is a bad price
	// for saving two lines per setter.
	void WriteHeader(const PageHeader& header) {
		header.WriteTo(std::span(frame_->data_).first<PAGE_HEADER_SIZE>());
	}

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

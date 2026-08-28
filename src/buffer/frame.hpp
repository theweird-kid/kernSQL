#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <shared_mutex>

#include "common/types.hpp"

namespace kernsql {

// A frame's lifecycle (DD-002, "Frame lifecycle"), guarded by Frame::mtx_. One enum rather
// than a set of booleans, so the illegal combinations are unrepresentable rather than merely
// unreachable, and every transition has exactly one owner.
//
// There is deliberately no Reclaiming state. Reclaim performs no I/O, so it completes inside a
// single critical section under the victim's shard lock and metadata mutex — no other thread
// can observe a frame mid-reclaim, so there is nothing to cancel.
enum class FrameState : uint8_t {
	// Holds no page: unmapped, unpinned, sitting on the free list.
	Free,
	// A disk read is in flight. Already mapped, so a concurrent fetcher of the same page finds
	// it and waits on cv_ rather than racing to load it independently; data_ is meaningless
	// until the loader publishes Resident.
	Loading,
	// Holds a valid page. In the Replacer's candidate set iff pin_count_ == 0.
	Resident,
	// The load errored. Already unmapped by the loader, which owns disposal — waiters observe
	// this, drop their pins and return the error without cleaning up themselves.
	Failed,
};

// The one place the state names live. std::formatter below delegates here rather than carrying
// its own switch, and the printf-style LOG_* macros need it because they cannot see a
// std::formatter at all — passing a FrameState for %s makes vsnprintf dereference the enum's
// value as a pointer. Returns a string literal, so it allocates nothing and is safe on an
// error path.
[[nodiscard]] constexpr const char* FrameStateName(FrameState state) {
	switch (state) {
		case FrameState::Free:
			return "Free";
		case FrameState::Loading:
			return "Loading";
		case FrameState::Resident:
			return "Resident";
		case FrameState::Failed:
			return "Failed";
	}
	return "Unknown";
}

// Frame is the in-memory container for one cached page (see DD-002). It owns two
// independent synchronization primitives, deliberately kept separate: `latch_` guards
// the page bytes themselves and can be held for a while (e.g. across a scan); `mtx_`
// guards only the small bookkeeping fields below it and is held for nanoseconds. Mixing
// the two into one lock would let a slow content access block a fast metadata check —
// see the Concurrency-Control note for why that produces convoys.
//
// Deliberately NOT stored here: `frame_id` (purely positional — the frame's own index
// in the pool array), `page_lsn` (already part of `data_`'s on-disk page header; a WAL
// concern, not a buffer-pool one), and `usage_count` (belongs to the Replacer's
// bookkeeping, not the frame's identity).
//
// Cache-line aligned. `data_` sits at offset 0 and is exactly PAGE_SIZE — a whole number of
// 64-byte lines — so this one annotation buys both separations that matter: the metadata below
// starts on a line of its own instead of sharing one with the tail of the page bytes, and
// `sizeof(Frame)` rounds up to a multiple of 64, so frame N's metadata cannot share a line with
// frame N+1's page bytes either. Without it a thread writing page bytes and a thread probing an
// unrelated frame's pin count bounce the same line between cores for no reason. Postgres gets
// this separation structurally by keeping BufferDescriptors and BufferBlocks in two arrays; this
// is the cheap version of the same idea, costing 16 bytes of tail padding per frame
// (sizeof goes 4272 -> 4288, or 0.4%).
//
// The pool's `new Frame[n]` honours it because C++17 routes over-aligned types through
// operator new[](size_t, align_val_t) — this silently did nothing before C++17.
struct alignas(64) Frame {
	// The PAGE_SIZE bytes currently resident in this frame. Guarded by `latch_`.
	std::array<std::byte, PAGE_SIZE> data_;

	// The alignas is redundant today — `data_` alone already lands this at a 64-byte offset —
	// but it is what actually pins the page-bytes/metadata split, so it is stated rather than
	// left as a consequence of the member order above.
	alignas(64) std::shared_mutex latch_;

	// --- Dirtiness: two monotonic counters, guarded by NEITHER lock (DD-002, "Dirtiness
	// as an epoch pair"). The frame is dirty iff dirty_epoch_ > flushed_epoch_.
	//
	// This is deliberately not a `bool is_dirty_` under mtx_. A flusher must hold the
	// shared content latch while it writes (or it captures a torn page), and if it then
	// dropped the latch before clearing the bool, a writer could dirty the page in the gap
	// and have its flag cleared a moment later — a lost update on disk. Keeping the bool
	// correct therefore required holding latch_ and mtx_ simultaneously, in the *reverse*
	// of this codebase's canonical lock order. With epochs there is nothing to clear, so
	// the two locks are never held together at all.
	//
	// Unlike Replacer::usage_count_, these are NOT heuristic counters — a missed dirty bump
	// loses data — so they use release/acquire rather than relaxed ordering. The cost is
	// nil on x86 and it keeps the reasoning local instead of leaning on the fact that mtx_
	// happens to order most of these accesses anyway.
	std::atomic<uint64_t> dirty_epoch_{0};
	std::atomic<uint64_t> flushed_epoch_{0};

	// --- Everything below is guarded by mtx_ ---
	//
	// mutable so that an observer which only *reads* frame state — BufferPoolManager::GetStats
	// — can still be const. Locking a mutex is not a logical mutation of the object, which is
	// exactly the case `mutable` exists for; FreeList::mtx_ is mutable for the same reason.
	mutable std::mutex mtx_;
	FrameState state_{FrameState::Free};
	page_id_t page_id_{INVALID_PAGE};  // INVALID_PAGE iff state_ == Free.
	int32_t pin_count_{0};             // a claim on this frame's identity — DD-002, "The pin
	                                   // invariant". Evictable only at 0.

	// Paired with mtx_. Two waits use it, and both hold a pin while they sleep: a miss waiter
	// blocks here while state_ == Loading, and on the failed-load path the loader blocks here
	// until pin_count_ falls to 1 (its own) so it can dispose of the frame itself.
	std::condition_variable cv_;

	Frame() = default;

	// Implied by owning a mutex/condition_variable/shared_mutex already, but stated
	// explicitly: DD-002 requires Frame to be non-copyable and non-movable, since it's
	// embedded inline in one fixed contiguous pool allocation made once at construction.
	Frame(const Frame&) = delete;
	Frame& operator=(const Frame&) = delete;
	Frame(Frame&&) = delete;
	Frame& operator=(Frame&&) = delete;

	// The concrete lock types are an implementation detail that will change once `latch_` is
	// replaced by a hybrid spin-then-block latch (see DD-002). Bind with `auto` where you can;
	// where you cannot — a page guard has to name the type to store one as a member — spell it
	// as Frame::ReadLatch / Frame::WriteLatch so the swap stays a one-line change here.
	//
	// Whatever replaces them must keep the same three operations the guards rely on: move
	// construction, `unlock()`, and a destructor that releases only if still held.
	using ReadLatch = std::shared_lock<std::shared_mutex>;
	using WriteLatch = std::unique_lock<std::shared_mutex>;

	[[nodiscard]] ReadLatch AcquireRead() { return ReadLatch(latch_); }
	[[nodiscard]] WriteLatch AcquireWrite() { return WriteLatch(latch_); }

	// --- Dirty-epoch protocol (DD-002). None of these take a lock. ---

	// Called by WritePageGuard's release, BEFORE it drops the content latch, so a flusher
	// holding the shared latch always observes an epoch that already accounts for every
	// write it is about to capture.
	void MarkDirty() { dirty_epoch_.fetch_add(1, std::memory_order_release); }

	[[nodiscard]] bool IsDirty() const {
		return dirty_epoch_.load(std::memory_order_acquire) >
		       flushed_epoch_.load(std::memory_order_acquire);
	}

	// Step 2 of the flush sequence: sample the epoch while holding the shared content latch,
	// hand the result to MarkFlushed() once the write completes.
	[[nodiscard]] uint64_t SampleDirtyEpoch() const {
		return dirty_epoch_.load(std::memory_order_acquire);
	}

	// Step 5: raise flushed_epoch_ to `observed`. Only ever moves upward, so concurrent
	// flushers cannot walk it backwards. A writer that dirtied the page after the latch was
	// released has already bumped dirty_epoch_ past `observed`, leaving the frame dirty —
	// which is the only direction that is safe to be wrong in.
	void MarkFlushed(uint64_t observed) {
		uint64_t current = flushed_epoch_.load(std::memory_order_relaxed);
		while (current < observed &&
		       !flushed_epoch_.compare_exchange_weak(current, observed, std::memory_order_release,
		                                             std::memory_order_relaxed)) {
		}
	}

	// Called when a frame is repurposed (miss path stamping it, a reclaimer turning it Free,
	// DeletePage vacating it) — always with mtx_ held and no other thread holding a pin, so a
	// plain store is enough.
	void ResetEpochs() {
		dirty_epoch_.store(0, std::memory_order_relaxed);
		flushed_epoch_.store(0, std::memory_order_relaxed);
	}
};

}  // namespace kernsql

// Specialize std::formatter for FrameState
template <>
struct std::formatter<kernsql::FrameState> {
	// Parses format specifiers (e.g., {:x}). We just accept standard empty {} bounds.
	constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

	// Formats the FrameState enum value into the output context. Delegates to FrameStateName so
	// the names exist in exactly one place — the LOG_* macros need the same mapping and cannot
	// reach a std::formatter.
	auto format(const kernsql::FrameState& state, format_context& ctx) const {
		return std::format_to(ctx.out(), "{}", kernsql::FrameStateName(state));
	}
};

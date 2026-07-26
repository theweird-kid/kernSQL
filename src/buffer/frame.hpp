#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "common/types.hpp"

namespace kernsql {

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
struct Frame {
	// The PAGE_SIZE bytes currently resident in this frame. Guarded by `latch_`.
	std::array<std::byte, PAGE_SIZE> data_;
	std::shared_mutex latch_;

	// --- Everything below is guarded by mtx_ ---
	std::mutex mtx_;
	page_id_t page_id_{INVALID_PAGE};  // INVALID_PAGE => frame is unused / on the free list.
	int32_t pin_count_{0};             // evictable only when this is 0.
	bool is_dirty_{false};             // modified since the last flush to disk.
	bool loading_{false};              // true while a disk read for this frame is in flight.
	std::condition_variable cv_;       // paired with mtx_; fetchers block on this while loading_.

	Frame() = default;

	// Implied by owning a mutex/condition_variable/shared_mutex already, but stated
	// explicitly: DD-002 requires Frame to be non-copyable and non-movable, since it's
	// embedded inline in one fixed contiguous pool allocation made once at construction.
	Frame(const Frame&) = delete;
	Frame& operator=(const Frame&) = delete;
	Frame(Frame&&) = delete;
	Frame& operator=(Frame&&) = delete;

	// Acquire the content latch. Always bind the result with `auto` — the concrete lock
	// type is an implementation detail that will change once `latch_` is replaced by a
	// hybrid spin-then-block latch (see DD-002); callers that never name the type stay
	// unaffected by that swap.
	[[nodiscard]] std::shared_lock<std::shared_mutex> AcquireRead() {
		return std::shared_lock(latch_);
	}
	[[nodiscard]] std::unique_lock<std::shared_mutex> AcquireWrite() {
		return std::unique_lock(latch_);
	}
};

}  // namespace kernsql

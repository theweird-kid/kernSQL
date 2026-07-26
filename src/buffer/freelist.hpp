#pragma once

#include <mutex>
#include <optional>
#include <stack>
#include <vector>

#include "common/types.hpp"

namespace kernsql {

// FreeList tracks frame_ids that have never held a page yet, or were just vacated by
// DeletePage — distinct from the Replacer, which tracks frames currently holding a page
// but unpinned (see DD-002, "Free list vs. replacer"). Popping is unconditionally O(1)
// and touches nothing else, which is what lets BufferPoolManager avoid running the
// clock sweep at all during warm-up, before every frame has held a page at least once.
//
// Guarded by its own plain mutex (DD-002 mechanism #5) — short critical section
// (push/pop one frame_id), touched only on the miss path.
class FreeList {
  public:
	// Fully populates the list with every index 0..capacity-1 up front, so a freshly
	// constructed FreeList is already fully stocked as an invariant of the type itself.
	// lifo_'s size never exceeds capacity for the pool's lifetime (frame_ids only ever
	// cycle between "free" and "in use," never duplicated or added to), so this
	// reserve() is the only allocation FreeList will ever need to do.
	explicit FreeList(std::size_t capacity) {
		lifo_.reserve(capacity);
		for (std::size_t i = 0; i < capacity; i++) {
			lifo_.push_back(static_cast<frame_id_t>(i));
		}
	}

	void Push(frame_id_t frame_id) {
		std::lock_guard<std::mutex> lock(mtx_);
		lifo_.push_back(frame_id);
	}

	// nullopt => list empty, caller falls through to the replacer.
	[[nodiscard]] std::optional<frame_id_t> Pop() {
		std::lock_guard<std::mutex> lock(mtx_);
		if (lifo_.empty()) return std::nullopt;
		frame_id_t elem = lifo_.back();
		lifo_.pop_back();
		return elem;
	}

  private:
	std::mutex mtx_;
	std::vector<frame_id_t> lifo_;
};

}  // namespace kernsql

#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "common/types.hpp"

namespace kernsql {

// FreeList tracks frame_ids in state FrameState::Free: never used yet, vacated by DeletePage,
// or just turned Free by a reclaimer — distinct from the Replacer, which tracks frames that
// hold a page but are unpinned (see DD-002, "Free list and replacer").
//
// It is a plain LIFO stack behind a plain mutex, and deliberately nothing more. An empty free
// list is a normal outcome rather than something to block on: a miss that finds it empty
// reclaims a frame inline through the Replacer instead. Nothing ever waits on this structure,
// so it has no producer/consumer sides to coordinate — hence no condition variable, no
// pool-exhausted signal, no watermarks and no shutdown handshake.
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

	// nullopt => empty right now. Not an error: it is the miss path's cue to reclaim a frame
	// itself, which is the only other source of free frames.
	[[nodiscard]] std::optional<frame_id_t> TryPop() {
		std::lock_guard<std::mutex> lock(mtx_);
		if (lifo_.empty()) return std::nullopt;
		frame_id_t frame_id = lifo_.back();
		lifo_.pop_back();
		return frame_id;
	}

	[[nodiscard]] std::size_t Size() const {
		std::lock_guard<std::mutex> lock(mtx_);
		return lifo_.size();
	}

  private:
	mutable std::mutex mtx_;
	std::vector<frame_id_t> lifo_;
};

}  // namespace kernsql

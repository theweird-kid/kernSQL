#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "common/types.hpp"

namespace kernsql {

// Clock-sweep eviction-candidate set: frames that currently hold a page but are unpinned.
//
// Threading note (DD-002, "Reclaiming a frame"): every method here is called from any thread.
// RecordAccess and SetEvictable fire on every access and every pin-count transition; Evict()
// is called by whichever thread missed and found the FreeList empty, and reclaims a frame
// inline. There is no background thread.
//
// Two concurrent Evict() callers are therefore expected, and are safe because Evict removes
// its victim from the candidate set before returning — so they cannot be handed the same
// frame. The caller then re-validates the victim under its page-table shard lock and metadata
// mutex, and a caller that declines the frame it was given is a normal outcome, not an error:
// the frame simply stays out of the candidate set until its next unpin re-adds it.
class Replacer {
  public:
	explicit Replacer(std::size_t capacity) : usage_count_(capacity), evictable_(capacity) {}

	// Bumps this frame's usage_count, capped at kMaxUsageCount. Called on every access
	// (hit or miss), independent of the frame's current evictable state.
	void RecordAccess(frame_id_t frame_id) {
		assert(static_cast<std::size_t>(frame_id) < usage_count_.size());
		auto& count = usage_count_[static_cast<std::size_t>(frame_id)];
		uint8_t old = count.load(std::memory_order_relaxed);
		while (old < kMaxUsageCount &&
		       !count.compare_exchange_weak(old, old + 1, std::memory_order_relaxed,
		                                    std::memory_order_relaxed)) {
		}
	}

	// Adds/removes frame_id from the eviction-candidate set. Idempotent.
	void SetEvictable(frame_id_t frame_id, bool evictable) {
		const auto index = static_cast<std::size_t>(frame_id);
		assert(index < evictable_.size());
		std::lock_guard<std::mutex> guard(mtx_);

		if (evictable_[index] == evictable) return;

		evictable_[index] = evictable;
		if (evictable)
			++evictable_count_;
		else
			--evictable_count_;
	}

	// Clock sweep: returns the first evictable frame found with usage_count == 0,
	// decrementing every evictable frame's usage_count as the hand passes it.
	// nullopt only when nothing is currently evictable; otherwise a victim is guaranteed
	// (bounded sweep + fallback, per DD-002 "Sweep semantics").
	[[nodiscard]] std::optional<frame_id_t> Evict() {
		std::lock_guard<std::mutex> guard(mtx_);
		if (evictable_count_ == 0) return std::nullopt;

		const std::size_t n = usage_count_.size();

		for (std::size_t scanned = 0; scanned < (kMaxUsageCount + 1) * n; ++scanned) {
			if (!evictable_[hand_]) {
				hand_ = (hand_ + 1) % n;
				continue;
			}

			if (usage_count_[hand_].load(std::memory_order_relaxed) == 0) return ClaimVictim();

			usage_count_[hand_].fetch_sub(1, std::memory_order_relaxed);
			hand_ = (hand_ + 1) % n;
		}

		// Bound exhausted — only reachable if concurrent RecordAccess keeps re-bumping
		// candidates as fast as the hand decrements them. Take the first evictable frame
		// regardless of count; membership can't change while mtx_ is held and
		// evictable_count_ > 0, so this finds one within n steps.
		while (!evictable_[hand_]) hand_ = (hand_ + 1) % n;
		return ClaimVictim();
	}

	// How many frames are currently eviction candidates. A diagnostic, not a decision input:
	// the answer is stale the moment the lock is released, so nothing may branch on it. Evict()
	// re-reads evictable_count_ under the same lock it evicts from, which is what makes *its*
	// use of the value sound.
	[[nodiscard]] std::size_t EvictableCount() const {
		std::lock_guard<std::mutex> guard(mtx_);
		return evictable_count_;
	}

  private:
	// Victim post-conditions (DD-002): leaves the candidate set and its usage_count resets
	// to 0 before Evict returns. Requires mtx_ held with hand_ on an evictable frame.
	frame_id_t ClaimVictim() {
		const std::size_t victim = hand_;

		evictable_[victim] = false;
		--evictable_count_;
		usage_count_[victim].store(0, std::memory_order_relaxed);

		hand_ = (hand_ + 1) % usage_count_.size();
		return static_cast<frame_id_t>(victim);
	}

	static constexpr uint8_t kMaxUsageCount = 3;  // resolved in DD-002

	mutable std::mutex mtx_;
	std::vector<std::atomic_uint8_t> usage_count_;  // indexed by frame_id
	std::vector<uint8_t> evictable_;                // 0/1 flags — deliberately not vector<bool>
	std::size_t hand_ = 0;                          // single global hand (resolved in DD-002)
	std::size_t evictable_count_ = 0;
};
}  // namespace kernsql

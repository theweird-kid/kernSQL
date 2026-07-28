#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "common/types.hpp"

namespace kernsql {
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
		assert(static_cast<std::size_t>(frame_id) < evictable_.size());
		std::lock_guard<std::mutex> guard(mtx_);

		if (evictable_[frame_id] == evictable) return;

		evictable_[frame_id] = evictable;
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

	std::mutex mtx_;
	std::vector<std::atomic_uint8_t> usage_count_;  // indexed by frame_id
	std::vector<uint8_t> evictable_;                // 0/1 flags — deliberately not vector<bool>
	std::size_t hand_ = 0;                          // single global hand (resolved in DD-002)
	std::size_t evictable_count_ = 0;
};
}  // namespace kernsql

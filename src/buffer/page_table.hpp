#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/logger.hpp"
#include "common/types.hpp"

namespace kernsql {
class PageTable {
  public:
	static constexpr std::size_t kNumShards = 16;  // fixed power of two (DD-002)

	// RAII handle: holds one shard's lock + gives access to that shard's map for as
	// long as it's alive. Non-copyable (holds a unique_lock), movable via guaranteed
	// copy elision on return (constructed in place at the call site, C++17).
	class ShardGuard {
	  public:
		[[nodiscard]] std::optional<frame_id_t> Find(page_id_t page_id) const {
			auto it = map_.find(page_id);
			if (it != map_.end()) return it->second;
			return std::nullopt;
		}

		// A duplicate key means two frames believe they hold the same page: writes land in
		// one, reads come from the other, and whichever flushes last wins. That is silent
		// data corruption, not a recoverable condition — so this survives NDEBUG rather than
		// compiling out into an emplace that quietly does nothing and leaves the stale
		// mapping in place. Same reasoning as ~BufferPoolManager aborting rather than
		// discarding dirty pages: a destructor, and this, are both the wrong place to decide
		// that corruption is survivable.
		void Insert(page_id_t page_id, frame_id_t frame_id) {
			assert(ShardIndex(page_id) == shard_index_);
			auto [it, inserted] = map_.emplace(page_id, frame_id);
			if (!inserted) {
				LOG_INFO("page %d is already mapped to frame %d; refusing to remap it to %d",
				         page_id, it->second, frame_id);
				std::abort();
			}
		}
		void Erase(page_id_t page_id) {
			assert(ShardIndex(page_id) == shard_index_);
			map_.erase(page_id);
		}

	  private:
		friend class PageTable;
		ShardGuard(std::mutex& mtx, std::unordered_map<page_id_t, frame_id_t>& map,
		           std::size_t shard_index)
		    : lock_(mtx), map_(map), shard_index_(shard_index) {}

		std::unique_lock<std::mutex> lock_;
		std::unordered_map<page_id_t, frame_id_t>& map_;
		std::size_t shard_index_;  // for the assert in Find/Insert/Erase — never changes after
		                           // construction.
	};

	// The common case: acquire the shard page_id hashes to.
	[[nodiscard]] ShardGuard AcquireShard(page_id_t page_id) {
		std::size_t shard_idx = ShardIndex(page_id);
		return AcquireShardByIndex(shard_idx);
	}

	// For the cross-shard case (DD-002 lock ordering: acquire both shards ascending,
	// re-validate) — acquiring by raw index is what makes "ascending order" expressible
	// at all, since the two page_ids involved hash to different shards by definition.
	[[nodiscard]] ShardGuard AcquireShardByIndex(std::size_t shard_index) {
		return ShardGuard(mutexes_[shard_index], maps_[shard_index], shard_index);
	}

	// Exposed so BufferPoolManager can compute which two indices it needs, and in what
	// order, before acquiring either.
	[[nodiscard]] static std::size_t ShardIndex(page_id_t page_id) {
		return static_cast<std::size_t>(page_id) & (kNumShards - 1);
	}

  private:
	std::array<std::mutex, kNumShards> mutexes_;
	std::array<std::unordered_map<page_id_t, frame_id_t>, kNumShards> maps_;
};
}  // namespace kernsql

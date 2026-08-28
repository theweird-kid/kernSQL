#pragma once

#include <cstddef>

namespace kernsql {

// A census of the buffer pool, for diagnostics and tests. Two properties to respect:
//
//   1. Producing it is O(capacity) and takes every frame's metadata mutex in turn. Fine for a
//      16-frame test pool, real work for a 262144-frame one. A diagnostic, never a hot path —
//      the same way pg_buffercache walks Postgres's BufferDescriptors.
//   2. It is a SAMPLE, not a snapshot. By the time the walk reaches frame 5000, frame 0 has
//      moved on, so no cross-frame invariant holds unless the pool is quiescent. A caller that
//      wants to assert on these numbers must establish quiescence first, exactly as Shutdown()
//      requires.
//
// At quiescence the invariants worth asserting are:
//   free_frames == free_list_size          a Free frame not on the list is unreachable
//                                          forever, which is the leak class that has bitten
//                                          this component twice
//   loading_frames == failed_frames == 0   both states carry a pin by construction
//   pinned_frames == 0
struct PoolStats {
	size_t capacity{0};
	size_t free_frames{0};     // state == Free
	size_t loading_frames{0};  // nonzero at quiescence is itself a bug
	size_t resident_frames{0};
	size_t failed_frames{0};  // ditto
	size_t pinned_frames{0};
	size_t free_list_size{0};
	size_t evictable{0};  // replacer's candidate count
};
}  // namespace kernsql

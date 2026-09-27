#include "b_plus_tree_page.hpp"

#include <cassert>

using namespace kernsql;

NodeSubHeader ConstLeafNode::Header() const {
	return NodeSubHeader::ReadFrom(body_.first<NODE_SUB_HEADER_SIZE>());
}

LeafEntry ConstLeafNode::EntryAt(uint16_t index) const {
	// Both bounds. Count() comes from the page itself, so a corrupted count would pass the first
	// check and read past the body into the rest of the frame array — valid memory, invisible to
	// ASan. The second turns that into an immediate failure.
	assert(index < Count());
	assert(index < NODE_CAPACITY);
	return LeafEntry::ReadFrom(body_.subspan(NodeEntryOffset(index)).first<NODE_ENTRY_SIZE>());
}

uint16_t ConstLeafNode::LowerBound(index_key_t key) const {
	// Half-open [lo, hi): everything left of lo is < key, everything from hi on is >= key. The
	// loop shrinks the gap until they meet, and the meeting point is the answer — including
	// Count() when every key is smaller, and 0 for an empty leaf, with no special case for either.
	uint16_t lo = 0;
	uint16_t hi = Count();
	while (lo < hi) {
		// lo + (hi - lo) / 2, never (lo + hi) / 2: the same habit that stops the classic overflow
		// in wider types, and it keeps mid strictly below hi, so KeyAt(mid) is always in range.
		const auto mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
		if (KeyAt(mid) < key) {
			lo = static_cast<uint16_t>(mid + 1);
		} else {
			hi = mid;
		}
	}
	return lo;
}

// Node-local checks only: count <= NODE_CAPACITY, level == 0, keys strictly increasing.
// Occupancy against the fanout, the separator bounds and the chain's order are tree-level and
// live in BPlusTree::CheckInvariants.
bool ConstLeafNode::CheckInvariants() const {}

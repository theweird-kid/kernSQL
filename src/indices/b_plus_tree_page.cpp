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

// Node-local checks only: count <= NODE_CAPACITY, level == 0, next_page_id is INVALID_PAGE or
// a non-reserved page (which also catches a leaf nobody Init()ed), every RID valid, keys strictly
// increasing.
// Occupancy against the fanout, the separator bounds and the chain's order are tree-level and
// live in BPlusTree::CheckInvariants.
bool ConstLeafNode::CheckInvariants() const {
	// One header read, so every check below judges the same snapshot.
	const NodeSubHeader header = Header();

	// FIRST, and returning rather than recording: a count past the capacity means the entry loop
	// below would walk off the body. Nothing after this line is safe to evaluate until it passes.
	if (header.count > NODE_CAPACITY) return false;

	if (header.level != 0) return false;

	// A sibling link can be "none" or a real data page, never a reserved page. This is also the
	// check that catches a leaf nobody called Init() on: a zeroed sub-header reads next_page_id 0,
	// which is META_PAGE_ID — the same trick as HeapPage's tuple_data_start, where zeroed and
	// initialised differ in exactly one field.
	if (header.next_page_id != INVALID_PAGE && header.next_page_id <= CATALOG_ROOT_PAGE_ID) {
		return false;
	}

	for (uint16_t i = 0; i < header.count; ++i) {
		const LeafEntry entry = EntryAt(i);

		// A default-constructed RID names INVALID_PAGE; one stored here points no row anywhere.
		if (!entry.rid.isValid()) return false;

		// STRICTLY increasing: keys are unique, so an equal neighbour is a duplicate that slipped
		// past Insert's check, and a smaller one is a mis-sorted insert or a bad split.
		if (i > 0 && EntryAt(static_cast<uint16_t>(i - 1)).key >= entry.key) return false;
	}
	return true;
}

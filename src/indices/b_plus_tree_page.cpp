#include "b_plus_tree_page.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/types.hpp"

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

void LeafNode::Init() {
	NodeSubHeader node_sh{0, 0, INVALID_PAGE};
	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

void LeafNode::SetNextLeaf(page_id_t next) {
	NodeSubHeader node_sh = Header();
	node_sh.next_page_id = next;
	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

void LeafNode::InsertAt(uint16_t index, const LeafEntry& entry) {
	uint16_t N = Count();

	assert(N < NODE_CAPACITY);
	assert(index <= N);

	std::memmove(body_.data() + NodeEntryOffset(index + 1), body_.data() + NodeEntryOffset(index),
	             (N - index) * NODE_ENTRY_SIZE);
	entry.WriteTo(body_.subspan(NodeEntryOffset(index)).first<NODE_ENTRY_SIZE>());

	NodeSubHeader node_sh = Header();
	node_sh.count++;
	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

void LeafNode::RemoveAt(uint16_t index) {
	uint16_t N = Count();

	assert(N <= NODE_CAPACITY);
	assert(index < N);

	std::memmove(body_.data() + NodeEntryOffset(index), body_.data() + NodeEntryOffset(index + 1),
	             (N - index - 1) * NODE_ENTRY_SIZE);
	// The shift left a second copy of the last entry in slot N - 1. Zero it, so a node's bytes
	// past count are always zero and two nodes holding the same entries stay byte-identical.
	std::memset(body_.data() + NodeEntryOffset(static_cast<uint16_t>(N - 1)), 0, NODE_ENTRY_SIZE);

	NodeSubHeader node_sh = Header();
	node_sh.count--;
	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

void LeafNode::Assign(std::span<const LeafEntry> entries) {
	assert(entries.size() <= NODE_CAPACITY);
	for (size_t it = 0; it < entries.size(); it++) {
		entries[it].WriteTo(body_.subspan(NodeEntryOffset(it)).first<NODE_ENTRY_SIZE>());
	}
	// Zero every slot past the new count, up to capacity — not just up to the old count. The old
	// count comes from the page and may be garbage (a page nobody Init()ed); capacity is a
	// constant, so this never trusts the page to bound a write. The left half of every split
	// shrinks through here.
	const std::size_t tail = NodeEntryOffset(static_cast<uint16_t>(entries.size()));
	std::memset(body_.data() + tail, 0, NodeEntryOffset(NODE_CAPACITY) - tail);

	NodeSubHeader node_sh = Header();
	node_sh.count = static_cast<uint16_t>(entries.size());
	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

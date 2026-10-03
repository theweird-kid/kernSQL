#include "b_plus_tree_page.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

#include "common/status.hpp"
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
		const std::size_t offset = NodeEntryOffset(static_cast<uint16_t>(it));
		entries[it].WriteTo(body_.subspan(offset).first<NODE_ENTRY_SIZE>());
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

NodeSubHeader ConstInternalNode::Header() const {
	return NodeSubHeader::ReadFrom(body_.first<NODE_SUB_HEADER_SIZE>());
}

InternalEntry ConstInternalNode::EntryAt(uint16_t index) const {
	assert(index < Count());
	assert(index < NODE_CAPACITY);
	return InternalEntry::ReadFrom(body_.subspan(NodeEntryOffset(index)).first<NODE_ENTRY_SIZE>());
}

index_key_t ConstInternalNode::KeyAt(uint16_t index) const {
	uint16_t N = Count();

	assert(1 <= index);
	assert(index < N);

	return EntryAt(index).key;
}

uint16_t ConstInternalNode::ChildIndexFor(index_key_t key) const {
	uint16_t N = Count();
	uint16_t low = 1;
	uint16_t high = N;

	while (low < high) {
		const auto mid = static_cast<uint16_t>(low + (high - low) / 2);
		if (KeyAt(mid) <= key) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}
	return low - 1;
}

bool ConstInternalNode::CheckInvariants() const {
	// One header read, so every check below judges the same snapshot.
	const NodeSubHeader header = Header();

	// FIRST, and returning rather than recording: same reason as the leaf — past this line the
	// entry loop trusts count to bound its reads.
	if (header.count > NODE_CAPACITY) return false;

	// Level 0 is a leaf. This is also what catches a node nobody Init()ed: a zeroed sub-header
	// reads level 0.
	if (header.level < 1) return false;

	// No B-link tree: the sibling field exists on internal nodes but is never used.
	if (header.next_page_id != INVALID_PAGE) return false;

	index_key_t prev = INT64_MIN;
	for (uint16_t i = 0; i < header.count; ++i) {
		const InternalEntry entry = EntryAt(i);

		// A child must be a real data page. INVALID_PAGE is a hole Assign or InsertAt left behind;
		// a reserved page (META, the catalog root) is a zeroed slot read as a child — 0 is
		// META_PAGE_ID.
		if (entry.child == INVALID_PAGE || entry.child <= CATALOG_ROOT_PAGE_ID) return false;

		if (i == 0) {
			// The unread slot has one canonical value, which Assign always writes.
			if (entry.key != INT64_MIN) return false;
			continue;
		}

		// STRICTLY increasing, starting at entry[1] compared against entry[0]'s INT64_MIN. That
		// first comparison is deliberate: a separator equal to INT64_MIN would leave child 0 an
		// empty key range, and the smallest key always stays in the leftmost leaf, so it can never
		// legitimately be promoted.
		if (entry.key <= prev) return false;
		prev = entry.key;
	}
	return true;
}

void InternalNode::Init(uint16_t level) {
	NodeSubHeader node_sh;

	assert(level >= 1);
	node_sh.level = level;

	node_sh.count = 0;
	node_sh.next_page_id = INVALID_PAGE;

	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

void InternalNode::InsertAt(uint16_t index, const InternalEntry& entry) {
	uint16_t N = Count();

	assert(N < NODE_CAPACITY);
	assert(1 <= index);
	assert(index <= N);

	std::memmove(body_.data() + NodeEntryOffset(index + 1), body_.data() + NodeEntryOffset(index),
	             (N - index) * NODE_ENTRY_SIZE);
	entry.WriteTo(body_.subspan(NodeEntryOffset(index)).first<NODE_ENTRY_SIZE>());

	NodeSubHeader node_sh = Header();
	node_sh.count++;
	node_sh.WriteTo(body_.first<NODE_SUB_HEADER_SIZE>());
}

void InternalNode::RemoveAt(uint16_t index) {
	uint16_t N = Count();

	assert(N <= NODE_CAPACITY);
	assert(1 <= index);
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

void InternalNode::SetKeyAt(uint16_t index, index_key_t key) {
	uint16_t N = Count();

	assert(1 <= index);
	assert(index < N);

	auto entry = EntryAt(index);
	entry.key = key;
	entry.WriteTo(body_.subspan(NodeEntryOffset(index)).first<NODE_ENTRY_SIZE>());
}

void InternalNode::Assign(std::span<const InternalEntry> entries) {
	// At least one child, unlike a leaf: an internal node with no children has nowhere to send a
	// search. One child is legal — a root holds one for a moment before it collapses.
	assert(1 <= entries.size());
	assert(entries.size() <= NODE_CAPACITY);

	// entry[0] goes through a copy with its key canonicalized. A split's right half starts with
	// the entry whose key was just pushed up to the parent; without this it would also linger in
	// the unread slot, and CheckInvariants would reject the node far from the split that caused it.
	InternalEntry first = entries[0];
	first.key = INT64_MIN;
	first.WriteTo(body_.subspan(NodeEntryOffset(0)).first<NODE_ENTRY_SIZE>());
	for (size_t it = 1; it < entries.size(); it++) {
		const std::size_t offset = NodeEntryOffset(static_cast<uint16_t>(it));
		entries[it].WriteTo(body_.subspan(offset).first<NODE_ENTRY_SIZE>());
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

Result<ConstLeafNode> kernsql::AsLeaf(const ReadPageGuard& guard) {
	if (guard.Header().page_type != PageType::INDEX_LEAF) {
		return std::unexpected(Status::Corruption("not a leaf page"));
	}
	return ConstLeafNode(guard.Body());
}

Result<LeafNode> kernsql::AsLeaf(WritePageGuard& guard) {
	if (guard.Header().page_type != PageType::INDEX_LEAF) {
		return std::unexpected(Status::Corruption("not a leaf page"));
	}
	return LeafNode(guard.MutableBody());
}

Result<ConstInternalNode> kernsql::AsInternal(const ReadPageGuard& guard) {
	if (guard.Header().page_type != PageType::INDEX_INTERNAL) {
		return std::unexpected(Status::Corruption("not an internal node page"));
	}
	return ConstInternalNode(guard.Body());
}

Result<InternalNode> kernsql::AsInternal(WritePageGuard& guard) {
	if (guard.Header().page_type != PageType::INDEX_INTERNAL) {
		return std::unexpected(Status::Corruption("not an internal node page"));
	}
	return InternalNode(guard.MutableBody());
}

Result<IndexHeader> kernsql::ReadIndexHeader(const ReadPageGuard& guard) {
	if (guard.Header().page_type != PageType::INDEX_HEADER) {
		return std::unexpected(Status::Corruption("not an index header page"));
	}
	return IndexHeader::ReadFrom(guard.Body());
}

Result<IndexHeader> kernsql::ReadIndexHeader(const WritePageGuard& guard) {
	if (guard.Header().page_type != PageType::INDEX_HEADER) {
		return std::unexpected(Status::Corruption("not an index header page"));
	}
	return IndexHeader::ReadFrom(guard.Body());
}

// Checks the type BEFORE writing: stamping an IndexHeader over a leaf or a heap page would
// overwrite its first 12 bytes — the node sub-header and the start of entry[0].
Status kernsql::WriteIndexHeader(WritePageGuard& guard, const IndexHeader& header) {
	if (guard.Header().page_type != PageType::INDEX_HEADER) {
		return Status::Corruption("not an index header page");
	}
	header.WriteTo(guard.MutableBody());
	return Status::OK();
}

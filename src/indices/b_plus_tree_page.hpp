#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

namespace kernsql {

/*
 * B+tree page formats (DD-005). Three page types, all laid out inside the 4064-byte body a page
 * guard hands out, all offsets BODY-RELATIVE:
 *
 *   INDEX_HEADER    one per index. Holds the root page id, which moves; its own page id does not,
 *                   and that page id IS the index — it is what the catalog stores.
 *   INDEX_INTERNAL  sub-header + sorted (key, child) pairs. entry[0].key is never read.
 *   INDEX_LEAF      sub-header + sorted (key, RID) pairs. Right sibling in the sub-header's
 *                   next_page_id — NOT PageHeader::next_page_id, which stays INVALID_PAGE on
 *                   every index page (DD-001's ownership chains are for FREE, HEAP, CATALOG).
 *
 *   body offset 0
 *   +-----------------------------+
 *   | node sub-header (8 bytes)   |
 *   +-----------------------------+
 *   | entry[0] entry[1] ...       |  sorted by key, 16 bytes each, no slot array
 *   |        unused space         |
 *   +-----------------------------+
 *   body offset 4064
 *
 * NO SLOTS, unlike HeapPage. Slots exist there because tuples vary in length and RIDs must
 * survive compaction; here every entry is the same width and nothing outside the tree holds an
 * entry's position. Search is binary search, insert and remove shift the tail with memmove.
 *
 * Every struct below is serialized with memcpy, same host-endian convention as PageHeader and
 * HeapSubHeader. Not reinterpret_cast over the body: the body is not guaranteed to be aligned for
 * int64_t, and aliasing it as a struct is undefined behaviour besides.
 */

using index_key_t = int64_t;

inline constexpr std::size_t NODE_SUB_HEADER_SIZE = 8;
inline constexpr std::size_t NODE_ENTRY_SIZE = 16;

// Physical entries per node, both types: (4064 - 8) / 16 = 253. The fanout actually used is
// whatever the header page records, which may be smaller in tests but never larger.
inline constexpr uint16_t NODE_CAPACITY =
    static_cast<uint16_t>((PAGE_BODY_SIZE - NODE_SUB_HEADER_SIZE) / NODE_ENTRY_SIZE);

// Body offset of entry `index`, for BOTH node types — they share one entry size. The one place
// this arithmetic lives: EntryAt, InsertAt, RemoveAt and Assign all go through it, so an
// off-by-one here is one bug, not four.
constexpr std::size_t NodeEntryOffset(uint16_t index) {
	return NODE_SUB_HEADER_SIZE + std::size_t{index} * NODE_ENTRY_SIZE;
}

// Smallest fanout BPlusTree::Create accepts. Below 3 the occupancy arithmetic stops producing
// legal nodes (an internal node needs at least two children), and tests have no reason to go
// lower: fanout 3-4 already builds a deep tree from a handful of keys.
inline constexpr uint16_t MIN_FANOUT = 3;

// Occupancy floors for a NON-ROOT node, as a function of the fanout in use. The root is exempt:
// a root leaf may hold 0 entries, a root internal node needs 2 children.
//
// Chosen so that split and merge both produce legal nodes for every max >= MIN_FANOUT:
//   leaf split      max + 1 entries -> ceil((max+1)/2) and floor((max+1)/2), both >= max/2
//   leaf merge      (max/2 - 1) + max/2 <= max - 1
//   internal split  max + 1 children -> floor((max+1)/2) == ceil(max/2) on the right
//   internal merge  (ceil(max/2) - 1) + ceil(max/2) <= max
// DD-005, "Capacity and occupancy", works these through for max = 253.
constexpr uint16_t LeafMinEntries(uint16_t leaf_max) {
	return static_cast<uint16_t>(leaf_max / 2);
}
constexpr uint16_t InternalMinChildren(uint16_t internal_max) {
	return static_cast<uint16_t>((internal_max + 1) / 2);
}

static_assert(NODE_CAPACITY == 253, "DD-005's arithmetic is written for 253 entries per node");
static_assert(NodeEntryOffset(NODE_CAPACITY) <= PAGE_BODY_SIZE, "the last entry must end in the body");
static_assert(LeafMinEntries(NODE_CAPACITY) == 126);
static_assert(InternalMinChildren(NODE_CAPACITY) == 127);
static_assert(InternalMinChildren(MIN_FANOUT) >= 2, "a non-root internal node needs 2 children");
static_assert(LeafMinEntries(NODE_CAPACITY) + LeafMinEntries(NODE_CAPACITY) - 1 <= NODE_CAPACITY,
              "a leaf merge must fit in one node");
static_assert(2 * InternalMinChildren(NODE_CAPACITY) - 1 <= NODE_CAPACITY,
              "an internal merge must fit in one node");

/*
 * The index header page's body. Written on root changes only — deliberately no entry count,
 * which would make every insert and delete write this page.
 */
struct IndexHeader {
	// Never INVALID_PAGE: an empty tree's root is an empty leaf.
	page_id_t root_page_id{INVALID_PAGE};

	// Levels, so 1 means the root is a leaf. Always root.level + 1. Stored so CheckInvariants can
	// assert every leaf's depth against a recorded number rather than against whichever leaf it
	// reached first.
	uint16_t height{0};

	// Fanout in use, fixed at Create. NODE_CAPACITY in production; smaller in tests so every
	// split, redistribution and merge runs within a few hundred operations. Stored here rather
	// than passed to Open so a tree can never be reopened with a different fanout than it was
	// built with.
	uint16_t leaf_max{NODE_CAPACITY};
	uint16_t internal_max{NODE_CAPACITY};

	uint16_t reserved{0};

	static IndexHeader ReadFrom(std::span<const std::byte, PAGE_BODY_SIZE> body) {
		IndexHeader h;
		std::memcpy(&h, body.data(), sizeof(IndexHeader));
		return h;
	}
	void WriteTo(std::span<std::byte, PAGE_BODY_SIZE> body) const {
		std::memcpy(body.data(), this, sizeof(IndexHeader));
	}
};

static_assert(sizeof(IndexHeader) == 12, "index header must pack with no implicit padding");
static_assert(std::is_trivially_copyable_v<IndexHeader>);

// The first 8 bytes of every internal and leaf node.
//
// Owns the leaf sibling link, rather than PageHeader::next_page_id, so that a node view sees its
// whole structure: a split or merge is then entirely view operations, and the chain rewiring —
// where getting the order wrong loses the rest of the chain — is testable over a bare byte array.
struct NodeSubHeader {
	// 0 for a leaf, parent's level + 1 above it. Redundant with page_type on purpose: the checker
	// asserts child.level == parent.level - 1 for every link, which catches a node hung at the
	// wrong depth or a child pointer written into the wrong parent.
	uint16_t level{0};

	// Entries in use. For a leaf, keys. For an internal node, CHILDREN — which is one more than
	// the number of separators, because entry[0] carries a child and no key.
	uint16_t count{0};

	// Leaf: the right sibling, INVALID_PAGE for the rightmost leaf. Internal: always INVALID_PAGE,
	// asserted by CheckInvariants — no B-link tree, though the field is where one would go.
	// Sized to keep the entry array 8-byte aligned within the body.
	page_id_t next_page_id{INVALID_PAGE};

	static NodeSubHeader ReadFrom(std::span<const std::byte, NODE_SUB_HEADER_SIZE> bytes) {
		NodeSubHeader s;
		std::memcpy(&s, bytes.data(), sizeof(NodeSubHeader));
		return s;
	}
	void WriteTo(std::span<std::byte, NODE_SUB_HEADER_SIZE> bytes) const {
		std::memcpy(bytes.data(), this, sizeof(NodeSubHeader));
	}
};

static_assert(sizeof(NodeSubHeader) == NODE_SUB_HEADER_SIZE);
static_assert(std::is_trivially_copyable_v<NodeSubHeader>);

/*
 * sizeof(RID) is 8, not 6: page_id_t + slot_id_t pads to RID's 4-byte alignment. Those two
 * padding bytes, and InternalEntry's four, must be WRITTEN AS ZERO so two nodes holding the same
 * entries are byte-identical. Do not rely on RID{} to zero them — a class with default member
 * initializers is not zero-initialized — write through a zeroed buffer or field by field.
 */
struct LeafEntry {
	index_key_t key{0};
	RID rid{};

	static LeafEntry ReadFrom(std::span<const std::byte, NODE_ENTRY_SIZE> bytes) {
		LeafEntry s;
		std::memcpy(&s, bytes.data(), sizeof(LeafEntry));
		return s;
	}
	// Field by field over zeroed bytes — NEVER a memcpy of the whole struct. RID's two padding
	// bytes (14-15 here) are indeterminate, so copying `this` would carry whatever was on the stack
	// onto the page: reads still round-trip, but two leaves with identical entries stop being
	// byte-identical, and every byte-comparison test becomes flaky.
	void WriteTo(std::span<std::byte, NODE_ENTRY_SIZE> bytes) const {
		std::memset(bytes.data(), 0, bytes.size());
		std::memcpy(bytes.data() + offsetof(LeafEntry, key), &key, sizeof(key));
		std::memcpy(bytes.data() + offsetof(LeafEntry, rid) + offsetof(RID, page_id), &rid.page_id,
		            sizeof(rid.page_id));
		std::memcpy(bytes.data() + offsetof(LeafEntry, rid) + offsetof(RID, slot), &rid.slot,
		            sizeof(rid.slot));
	}
};

struct InternalEntry {
	// Unused in entry[0], where it is always INT64_MIN — the -infinity it stands for, so a dump
	// reads correctly and CheckInvariants can assert it.
	index_key_t key{0};
	page_id_t child{INVALID_PAGE};
	uint32_t reserved{0};

	static InternalEntry ReadFrom(std::span<const std::byte, NODE_ENTRY_SIZE> bytes) {
		InternalEntry s;
		std::memcpy(&s, bytes.data(), sizeof(InternalEntry));
		return s;
	}
	void WriteTo(std::span<std::byte, NODE_ENTRY_SIZE> bytes) const {
		std::memcpy(bytes.data(), this, sizeof(InternalEntry));
	}
};

static_assert(sizeof(LeafEntry) == NODE_ENTRY_SIZE);
static_assert(sizeof(InternalEntry) == NODE_ENTRY_SIZE);
static_assert(std::is_trivially_copyable_v<LeafEntry>);
static_assert(std::is_trivially_copyable_v<InternalEntry>);

// The on-disk format depends on field OFFSETS, not only sizes: a reordering that keeps each struct
// at 16 bytes would pass every sizeof check above and silently change the file format.
static_assert(offsetof(RID, page_id) == 0 && offsetof(RID, slot) == 4 && sizeof(RID) == 8);
static_assert(offsetof(LeafEntry, key) == 0 && offsetof(LeafEntry, rid) == 8);
static_assert(offsetof(InternalEntry, key) == 0 && offsetof(InternalEntry, child) == 8 &&
                  offsetof(InternalEntry, reserved) == 12,
              "InternalEntry has no implicit padding — every byte is a named, initialised field");

/*
 * Views over a node body. Same rules as ConstHeapPage/HeapPage, for the same reasons:
 *
 *   - Non-owning: a span, not a guard, not a pin, not a latch. A view must never outlive the
 *     guard it came from.
 *   - No default constructor, so an unbound view cannot exist.
 *   - Const and mutable as two types, the mutable one converting to the const one.
 *   - Pure: no buffer pool, no disk. Tests build them over a bare std::array<std::byte, 4064>,
 *     which is where the index arithmetic gets debugged.
 *
 * The views know NOTHING about fanout. They enforce the physical capacity (NODE_CAPACITY) and
 * nothing else; whether a node is full, underfull or legal at the tree's fanout is BPlusTree's
 * question, answered against IndexHeader::leaf_max / internal_max.
 *
 * STRUCTURAL OPERATIONS GO THROUGH Assign. A split, merge or redistribution reads the entries it
 * needs into a temporary array, computes the result there, and Assigns each affected node its new
 * contents whole. No in-place partial moves between two pages: building the N+1 array first and
 * cutting it is what removes the case analysis where the classic off-by-one lives (DD-005, "Leaf
 * split").
 */
class ConstLeafNode {
  public:
	ConstLeafNode() = delete;
	explicit ConstLeafNode(std::span<const std::byte, PAGE_BODY_SIZE> body) : body_(body) {}

	[[nodiscard]] NodeSubHeader Header() const;
	[[nodiscard]] uint16_t Count() const { return Header().count; }

	// Precondition: index < Count(). Asserted, not checked — an out-of-range index is a tree
	// bug, not a condition the caller can handle.
	[[nodiscard]] LeafEntry EntryAt(uint16_t index) const;
	[[nodiscard]] index_key_t KeyAt(uint16_t index) const { return EntryAt(index).key; }

	// First index whose key is >= `key`, or Count() if there is none. The insert position, and
	// the starting position of a range scan — which is why Count() is a legal answer: every key
	// here is below `key`, and the caller must continue in the NEXT leaf (DD-005, "Range scan").
	[[nodiscard]] uint16_t LowerBound(index_key_t key) const;

	// The right sibling, INVALID_PAGE for the rightmost leaf.
	[[nodiscard]] page_id_t NextLeaf() const { return Header().next_page_id; }

	// Node-local checks only: count <= NODE_CAPACITY, level == 0, keys strictly increasing.
	// Occupancy against the fanout, the separator bounds and the chain's order are tree-level and
	// live in BPlusTree::CheckInvariants.
	[[nodiscard]] bool CheckInvariants() const;

  private:
	std::span<const std::byte, PAGE_BODY_SIZE> body_;
};

class LeafNode {
  public:
	LeafNode() = delete;
	explicit LeafNode(std::span<std::byte, PAGE_BODY_SIZE> body) : body_(body) {}

	// NOLINTNEXTLINE(google-explicit-constructor) — deliberate, the T* -> const T* edge.
	operator ConstLeafNode() const { return ConstLeafNode(body_); }
	[[nodiscard]] ConstLeafNode View() const { return ConstLeafNode(body_); }

	[[nodiscard]] NodeSubHeader Header() const { return View().Header(); }
	[[nodiscard]] uint16_t Count() const { return View().Count(); }
	[[nodiscard]] LeafEntry EntryAt(uint16_t index) const { return View().EntryAt(index); }
	[[nodiscard]] index_key_t KeyAt(uint16_t index) const { return View().KeyAt(index); }
	[[nodiscard]] uint16_t LowerBound(index_key_t key) const { return View().LowerBound(key); }
	[[nodiscard]] page_id_t NextLeaf() const { return View().NextLeaf(); }
	[[nodiscard]] bool CheckInvariants() const { return View().CheckInvariants(); }

	// level = 0, count = 0, next_page_id = INVALID_PAGE. The CALLER stamps page_type = INDEX_LEAF
	// through the guard; this view cannot see the page header.
	void Init();

	// Rewire the chain. On a split, right.SetNextLeaf(left.NextLeaf()) BEFORE
	// left.SetNextLeaf(right) — the reverse order overwrites the only copy of the old link and
	// loses every leaf to the right. On a merge, left.SetNextLeaf(right.NextLeaf()).
	void SetNextLeaf(page_id_t next);

	// Shift entry[index ..] right by one and write `entry` at `index`. Preconditions, asserted:
	// Count() < NODE_CAPACITY, index <= Count(). Does NOT check ordering or duplicates — the tree
	// found `index` with LowerBound and already rejected a duplicate.
	void InsertAt(uint16_t index, const LeafEntry& entry);

	// Shift entry[index + 1 ..] left by one. Precondition, asserted: index < Count().
	void RemoveAt(uint16_t index);

	// Replace the node's whole contents with `entries`, which must already be sorted. Sets count
	// to entries.size(). Precondition, asserted: entries.size() <= NODE_CAPACITY. The one
	// primitive split, merge and redistribution are built from.
	void Assign(std::span<const LeafEntry> entries);

  private:
	std::span<std::byte, PAGE_BODY_SIZE> body_;
};

class ConstInternalNode {
  public:
	ConstInternalNode() = delete;
	explicit ConstInternalNode(std::span<const std::byte, PAGE_BODY_SIZE> body) : body_(body) {}

	[[nodiscard]] NodeSubHeader Header() const;

	// Children, not separators. An internal node with Count() == n has n - 1 separators.
	[[nodiscard]] uint16_t Count() const { return Header().count; }
	[[nodiscard]] uint16_t Level() const { return Header().level; }

	[[nodiscard]] InternalEntry EntryAt(uint16_t index) const;
	[[nodiscard]] page_id_t ChildAt(uint16_t index) const { return EntryAt(index).child; }

	// Separator `index`, which bounds ChildAt(index) from below. Precondition, asserted:
	// 1 <= index < Count(). Index 0 is rejected rather than answered — entry[0].key is not a
	// separator, and a caller asking for it has an off-by-one.
	[[nodiscard]] index_key_t KeyAt(uint16_t index) const;

	// Which child to descend into for `key`: the largest i in [1, Count()) with KeyAt(i) <= key,
	// or 0 if there is none. An upper_bound over entry[1 ..] minus one — never over entry[0].
	[[nodiscard]] uint16_t ChildIndexFor(index_key_t key) const;

	// Node-local checks: 1 <= level, count <= NODE_CAPACITY, entry[0].key == INT64_MIN, separators
	// strictly increasing, no INVALID_PAGE child, next_page_id == INVALID_PAGE.
	[[nodiscard]] bool CheckInvariants() const;

  private:
	std::span<const std::byte, PAGE_BODY_SIZE> body_;
};

class InternalNode {
  public:
	InternalNode() = delete;
	explicit InternalNode(std::span<std::byte, PAGE_BODY_SIZE> body) : body_(body) {}

	// NOLINTNEXTLINE(google-explicit-constructor) — deliberate, the T* -> const T* edge.
	operator ConstInternalNode() const { return ConstInternalNode(body_); }
	[[nodiscard]] ConstInternalNode View() const { return ConstInternalNode(body_); }

	[[nodiscard]] NodeSubHeader Header() const { return View().Header(); }
	[[nodiscard]] uint16_t Count() const { return View().Count(); }
	[[nodiscard]] uint16_t Level() const { return View().Level(); }
	[[nodiscard]] InternalEntry EntryAt(uint16_t index) const { return View().EntryAt(index); }
	[[nodiscard]] page_id_t ChildAt(uint16_t index) const { return View().ChildAt(index); }
	[[nodiscard]] index_key_t KeyAt(uint16_t index) const { return View().KeyAt(index); }
	[[nodiscard]] uint16_t ChildIndexFor(index_key_t key) const {
		return View().ChildIndexFor(key);
	}
	[[nodiscard]] bool CheckInvariants() const { return View().CheckInvariants(); }

	// count = 0 at the given level (>= 1), next_page_id = INVALID_PAGE. The caller stamps
	// page_type = INDEX_INTERNAL.
	void Init(uint16_t level);

	// Shift entry[index ..] right and write `entry` at `index`. Preconditions, asserted:
	// Count() < NODE_CAPACITY, 1 <= index <= Count(). Never at 0 — a new child always goes AFTER
	// the child that split, so the leftmost child is only ever replaced through Assign.
	void InsertAt(uint16_t index, const InternalEntry& entry);

	// Shift entry[index + 1 ..] left. Precondition, asserted: 1 <= index < Count(). Removing
	// entry[0] would promote entry[1]'s key into the unread position and silently drop a
	// separator; a merge that frees the leftmost child rewrites the node through Assign instead.
	void RemoveAt(uint16_t index);

	// Rewrite separator `index` in place — the parent side of a redistribution. Precondition,
	// asserted: 1 <= index < Count().
	void SetKeyAt(uint16_t index, index_key_t key);

	// Replace the whole contents. Writes INT64_MIN into entry[0].key regardless of what
	// entries[0].key holds, so the unread slot has one canonical value no matter which split or
	// rotation produced it. Precondition, asserted: 1 <= entries.size() <= NODE_CAPACITY.
	void Assign(std::span<const InternalEntry> entries);

  private:
	std::span<std::byte, PAGE_BODY_SIZE> body_;
};

/*
 * The only way to get a node view in non-test code, for the same reason AsHeapPage exists: a view
 * sees only the body and cannot check page_type, so a view built over the wrong page type parses
 * garbage and scribbles on it. The guard can see the header. kCorruption on mismatch.
 */
[[nodiscard]] Result<ConstLeafNode> AsLeaf(const ReadPageGuard& guard);
[[nodiscard]] Result<LeafNode> AsLeaf(WritePageGuard& guard);
[[nodiscard]] Result<ConstInternalNode> AsInternal(const ReadPageGuard& guard);
[[nodiscard]] Result<InternalNode> AsInternal(WritePageGuard& guard);

// Header page access, validated the same way: kCorruption unless page_type == INDEX_HEADER.
[[nodiscard]] Result<IndexHeader> ReadIndexHeader(const ReadPageGuard& guard);
[[nodiscard]] Result<IndexHeader> ReadIndexHeader(const WritePageGuard& guard);
[[nodiscard]] Status WriteIndexHeader(WritePageGuard& guard, const IndexHeader& header);

}  // namespace kernsql

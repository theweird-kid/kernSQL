#include "indices/b_plus_tree_page.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <utility>
#include <optional>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "gtest/gtest.h"
#include "storage/disk_manager.hpp"

namespace kernsql {

// A test not written yet is GTEST_SKIP rather than an empty body, so it shows up as skipped in
// ctest instead of passing silently.

class BPlusTreePageTest : public ::testing::Test {
  protected:
	// A bare body: no DiskManager, no BufferPoolManager, no guard. Each test Inits it as the node
	// type it needs. Starts zeroed, so "never Init()ed" is also testable.
	void SetUp() override { body_.fill(std::byte{0}); }

	LeafNode Leaf() { return LeafNode(body_); }
	ConstLeafNode LeafView() const { return ConstLeafNode(body_); }
	InternalNode Internal() { return InternalNode(body_); }
	ConstInternalNode InternalView() const { return ConstInternalNode(body_); }

	// n sorted leaf entries: keys 10, 20, ..., RIDs on page 2 with slot = index.
	static std::vector<LeafEntry> LeafEntries(uint16_t n) {
		std::vector<LeafEntry> entries;
		for (uint16_t i = 0; i < n; ++i) entries.push_back({10 * (i + 1), RID{2, i}});
		return entries;
	}

	// n children: entry[i] = {10 * i, page 2 + i}. entry[0]'s key is 0 here; Assign canonicalizes
	// it to INT64_MIN on the page.
	static std::vector<InternalEntry> InternalEntries(uint16_t n) {
		std::vector<InternalEntry> entries;
		for (uint16_t i = 0; i < n; ++i) entries.push_back({10 * i, 2 + i});
		return entries;
	}

	// Raw writes that bypass every view operation. ONLY for the CheckInvariants tests, which need
	// a node the API would never produce.
	void StampSubHeader(const NodeSubHeader& header) {
		header.WriteTo(std::span(body_).first<NODE_SUB_HEADER_SIZE>());
	}
	void StampEntry(uint16_t index, const InternalEntry& entry) {
		entry.WriteTo(std::span(body_).subspan(NodeEntryOffset(index)).first<NODE_ENTRY_SIZE>());
	}

	// Offset of the first non-zero byte in body_[from, PAGE_BODY_SIZE), or nullopt if they are all
	// zero. An offset, not a bool, so a failure says where the stray byte is.
	std::optional<std::size_t> FirstNonZeroByte(std::size_t from) const {
		for (std::size_t off = from; off < PAGE_BODY_SIZE; ++off) {
			if (body_[off] != std::byte{0}) return off;
		}
		return std::nullopt;
	}

	// A fresh database file and a 4-frame pool for the guard-level tests, torn down afterwards
	// whether `fn` passes or not. An ASSERT inside `fn` returns from `fn` only, so Shutdown and
	// the file cleanup still run.
	template <typename Fn>
	static void WithBufferPool(const char* file_name, Fn&& fn) {
		const auto path = std::filesystem::temp_directory_path() / file_name;
		std::filesystem::remove(path);
		auto dm = DiskManager::Open(path);
		ASSERT_TRUE(dm.has_value()) << dm.error().message();
		{
			BufferPoolManager bpm(**dm, 4);
			std::forward<Fn>(fn)(bpm);
			EXPECT_TRUE(bpm.Shutdown().ok());
		}
		dm->reset();
		std::filesystem::remove(path);
	}

	std::array<std::byte, PAGE_BODY_SIZE> body_{};
};

// ---------------------------------------------------------------------------------------------
// Leaf: Init and the empty leaf
// ---------------------------------------------------------------------------------------------

// level 0, count 0, next_page_id INVALID_PAGE, CheckInvariants true.
TEST_F(BPlusTreePageTest, LeafInitWritesAnEmptyLeafHeader) {
	auto leaf = Leaf();
	leaf.Init();
	auto leaf_header = leaf.Header();

	ASSERT_EQ(leaf_header.count, 0);
	ASSERT_EQ(leaf_header.level, 0);
	ASSERT_EQ(leaf_header.next_page_id, INVALID_PAGE);
}

// A zeroed body reads next_page_id 0 == META_PAGE_ID, which CheckInvariants must reject.
TEST_F(BPlusTreePageTest, LeafNeverInitedFailsCheckInvariants) {
	auto leaf = Leaf();
	EXPECT_FALSE(leaf.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Leaf: InsertAt / RemoveAt
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, LeafInsertAtIntoEmptyLeaf) {
	auto leaf = Leaf();
	leaf.Init();

	const LeafEntry entry{10, RID{2, 0}};
	leaf.InsertAt(0, entry);

	ASSERT_EQ(leaf.Count(), 1);
	EXPECT_EQ(leaf.EntryAt(0), entry);
	EXPECT_TRUE(leaf.CheckInvariants());
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(1)), std::nullopt);
}

TEST_F(BPlusTreePageTest, LeafInsertAtPastCountAsserts) {
	// Process-wide, and it stays set for every later test. "threadsafe" re-runs the binary in the
	// child instead of forking a process that has threads, which is what TSan objects to.
	GTEST_FLAG_SET(death_test_style, "threadsafe");
#ifdef NDEBUG
	GTEST_SKIP() << "assertion only in debug build";
#endif

	auto leaf = Leaf();
	leaf.Init();
	EXPECT_DEATH(leaf.InsertAt(1, LeafEntry{10, RID{2, 10}}), "index <= N");
}

// Front, middle and end: every existing entry lands one slot right, in order.
TEST_F(BPlusTreePageTest, LeafInsertAtShiftsTheTailRight) {
	auto leaf = Leaf();
	leaf.Init();

	// End, three times: each insert at Count() shifts nothing.
	leaf.InsertAt(0, LeafEntry{10, RID{2, 0}});
	leaf.InsertAt(1, LeafEntry{20, RID{2, 1}});
	leaf.InsertAt(2, LeafEntry{30, RID{2, 2}});

	// Front shifts all three; middle shifts only the two after it.
	leaf.InsertAt(0, LeafEntry{5, RID{3, 0}});
	leaf.InsertAt(2, LeafEntry{15, RID{3, 1}});

	const std::vector<LeafEntry> expected{
	    {5, RID{3, 0}}, {10, RID{2, 0}}, {15, RID{3, 1}}, {20, RID{2, 1}}, {30, RID{2, 2}},
	};
	ASSERT_EQ(leaf.Count(), expected.size());
	for (uint16_t i = 0; i < leaf.Count(); ++i) {
		EXPECT_EQ(leaf.EntryAt(i), expected[i]) << "at index " << i;
	}
	EXPECT_TRUE(leaf.CheckInvariants());
}

// NODE_CAPACITY inserts succeed; the last entry ends exactly at NodeEntryOffset(NODE_CAPACITY).
TEST_F(BPlusTreePageTest, LeafInsertAtFillsToCapacity) {
	auto leaf = Leaf();
	leaf.Init();

	// Every insert goes to the FRONT, keys descending so the node stays sorted. The last one
	// shifts all 252 existing entries, and that memmove ends exactly at the capacity boundary —
	// inserting at the end would never move anything.
	for (uint16_t k = NODE_CAPACITY; k-- > 0;) {
		leaf.InsertAt(0, LeafEntry{k, RID{2, k}});
	}

	ASSERT_EQ(leaf.Count(), NODE_CAPACITY);
	for (uint16_t i = 0; i < NODE_CAPACITY; ++i) {
		EXPECT_EQ(leaf.EntryAt(i), (LeafEntry{i, RID{2, i}})) << "at index " << i;
	}
	EXPECT_TRUE(leaf.CheckInvariants());

	// The 8 bytes past the last entry are never part of any entry; a shift one entry too long
	// would land here.
	for (std::size_t off = NodeEntryOffset(NODE_CAPACITY); off < PAGE_BODY_SIZE; ++off) {
		EXPECT_EQ(body_[off], std::byte{0}) << "at body offset " << off;
	}
}

// Bytes 14-15 of a written entry (RID padding) are zero even if they were 0xFF beforehand.
TEST_F(BPlusTreePageTest, LeafEntryWriteZeroesRidPadding) {
	// Start from all-0xFF so a WriteTo that skips the padding leaves a visible trace.
	std::array<std::byte, NODE_ENTRY_SIZE> bytes{};
	bytes.fill(std::byte{0xFF});

	const LeafEntry entry{7, RID{2, 3}};
	entry.WriteTo(std::span<std::byte, NODE_ENTRY_SIZE>(bytes));

	EXPECT_EQ(bytes[14], std::byte{0});
	EXPECT_EQ(bytes[15], std::byte{0});
	EXPECT_EQ(LeafEntry::ReadFrom(std::span<const std::byte, NODE_ENTRY_SIZE>(bytes)), entry);
}

// Front, middle and end: the tail shifts left, order is kept.
TEST_F(BPlusTreePageTest, LeafRemoveAtShiftsTheTailLeft) {
	auto leaf = Leaf();
	leaf.Init();
	const auto e = LeafEntries(5);
	leaf.Assign(e);

	leaf.RemoveAt(4);  // end: shifts nothing
	leaf.RemoveAt(0);  // front: shifts everything
	leaf.RemoveAt(1);  // middle

	const std::vector<LeafEntry> expected{e[1], e[3]};
	ASSERT_EQ(leaf.Count(), expected.size());
	for (uint16_t i = 0; i < leaf.Count(); ++i) {
		EXPECT_EQ(leaf.EntryAt(i), expected[i]) << "at index " << i;
	}
	EXPECT_TRUE(leaf.CheckInvariants());
}

// The slot at the old Count() - 1 is all zero after the remove.
TEST_F(BPlusTreePageTest, LeafRemoveAtZeroesTheVacatedSlot) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));

	leaf.RemoveAt(0);

	ASSERT_EQ(leaf.Count(), 2);
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(2)), std::nullopt);
}

// Insert then remove the same entry: body is byte-identical to before the insert.
TEST_F(BPlusTreePageTest, LeafInsertThenRemoveIsByteIdentical) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(4));  // keys 10, 20, 30, 40

	const auto before = body_;
	// Every legal position, front through end. The key is out of order at most positions, which
	// does not matter: InsertAt does not check order, and the entry is gone again before any check.
	for (uint16_t index = 0; index <= 4; ++index) {
		leaf.InsertAt(index, LeafEntry{25, RID{3, 0}});
		leaf.RemoveAt(index);
		EXPECT_EQ(body_, before) << "after insert and remove at index " << index;
	}
}

// ---------------------------------------------------------------------------------------------
// Leaf: Assign and SetNextLeaf
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, LeafAssignReplacesContentsAndCount) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(5));

	const std::vector<LeafEntry> replacement{{7, RID{3, 0}}, {8, RID{3, 1}}, {9, RID{3, 2}}};
	leaf.Assign(replacement);

	ASSERT_EQ(leaf.Count(), replacement.size());
	for (uint16_t i = 0; i < leaf.Count(); ++i) {
		EXPECT_EQ(leaf.EntryAt(i), replacement[i]) << "at index " << i;
	}
	EXPECT_TRUE(leaf.CheckInvariants());
}

// Fill to capacity, Assign 2: every byte from NodeEntryOffset(2) to NodeEntryOffset(NODE_CAPACITY)
// is zero.
TEST_F(BPlusTreePageTest, LeafAssignZeroesEveryByteToCapacity) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(NODE_CAPACITY));

	const auto two = LeafEntries(2);
	leaf.Assign(two);

	ASSERT_EQ(leaf.Count(), 2);
	EXPECT_EQ(leaf.EntryAt(0), two[0]);
	EXPECT_EQ(leaf.EntryAt(1), two[1]);
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(2)), std::nullopt);
}

// An empty Assign is legal for a leaf (empty root) and still passes CheckInvariants.
TEST_F(BPlusTreePageTest, LeafAssignEmptyLeavesALegalEmptyLeaf) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));

	leaf.Assign({});

	EXPECT_EQ(leaf.Count(), 0);
	EXPECT_TRUE(leaf.CheckInvariants());
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(0)), std::nullopt);
}

// level and next_page_id survive an Assign.
TEST_F(BPlusTreePageTest, LeafAssignPreservesTheSubHeader) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.SetNextLeaf(7);

	leaf.Assign(LeafEntries(3));

	const NodeSubHeader header = leaf.Header();
	EXPECT_EQ(header.level, 0);
	EXPECT_EQ(header.count, 3);
	EXPECT_EQ(header.next_page_id, 7);
}

// Only next_page_id changes: level, count and every entry byte are untouched.
TEST_F(BPlusTreePageTest, LeafSetNextLeafChangesOnlyTheLink) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));

	// The expected body is the old one with exactly the four next_page_id bytes rewritten.
	auto expected = body_;
	const page_id_t next = 9;
	std::memcpy(expected.data() + offsetof(NodeSubHeader, next_page_id), &next, sizeof(next));

	leaf.SetNextLeaf(next);

	EXPECT_EQ(leaf.NextLeaf(), next);
	EXPECT_EQ(body_, expected);
}

// ---------------------------------------------------------------------------------------------
// Leaf: LowerBound
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, LowerBoundOnEmptyLeafIsZero) {
	auto leaf = Leaf();
	leaf.Init();
	EXPECT_EQ(leaf.LowerBound(0), 0);
	EXPECT_EQ(leaf.LowerBound(INT64_MIN), 0);
	EXPECT_EQ(leaf.LowerBound(INT64_MAX), 0);
}
TEST_F(BPlusTreePageTest, LowerBoundOnExactMatch) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(5));  // keys 10, 20, 30, 40, 50

	EXPECT_EQ(leaf.LowerBound(10), 0);
	EXPECT_EQ(leaf.LowerBound(30), 2);
	EXPECT_EQ(leaf.LowerBound(50), 4);
}
TEST_F(BPlusTreePageTest, LowerBoundBetweenKeys) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(5));  // keys 10, 20, 30, 40, 50

	EXPECT_EQ(leaf.LowerBound(11), 1);
	EXPECT_EQ(leaf.LowerBound(25), 2);
	EXPECT_EQ(leaf.LowerBound(49), 4);
}
TEST_F(BPlusTreePageTest, LowerBoundBelowEveryKeyIsZero) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(5));  // keys 10, 20, 30, 40, 50

	EXPECT_EQ(leaf.LowerBound(9), 0);
	EXPECT_EQ(leaf.LowerBound(INT64_MIN), 0);
}
TEST_F(BPlusTreePageTest, LowerBoundAboveEveryKeyIsCount) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(5));  // keys 10, 20, 30, 40, 50

	EXPECT_EQ(leaf.LowerBound(51), 5);
	EXPECT_EQ(leaf.LowerBound(INT64_MAX), 5);
}

// Full leaf: for every stored key and every gap between keys, against a linear scan.
TEST_F(BPlusTreePageTest, LowerBoundMatchesALinearScanOnAFullLeaf) {
	auto leaf = Leaf();
	leaf.Init();
	const auto entries = LeafEntries(NODE_CAPACITY);  // keys 10, 20, ..., 2530
	leaf.Assign(entries);

	// Every key and every gap, plus both ends, against the definition: first index whose key is
	// >= the probe, Count() if none.
	std::vector<index_key_t> probes{INT64_MIN, INT64_MAX};
	for (index_key_t k = 0; k <= 10 * (NODE_CAPACITY + 1); ++k) probes.push_back(k);

	for (const index_key_t probe : probes) {
		uint16_t expected = 0;
		while (expected < entries.size() && entries[expected].key < probe) ++expected;
		ASSERT_EQ(leaf.LowerBound(probe), expected) << "probe " << probe;
	}
}

// ---------------------------------------------------------------------------------------------
// Leaf: CheckInvariants rejects each corruption
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, LeafCheckRejectsCountOverCapacity) {
	auto leaf = Leaf();
	leaf.Init();
	ASSERT_TRUE(leaf.CheckInvariants());

	StampSubHeader(NodeSubHeader{0, NODE_CAPACITY + 1, INVALID_PAGE});
	EXPECT_FALSE(leaf.CheckInvariants());
}
TEST_F(BPlusTreePageTest, LeafCheckRejectsNonZeroLevel) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));
	ASSERT_TRUE(leaf.CheckInvariants());

	StampSubHeader(NodeSubHeader{1, 3, INVALID_PAGE});
	EXPECT_FALSE(leaf.CheckInvariants());
}
TEST_F(BPlusTreePageTest, LeafCheckRejectsReservedNextPage) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));

	leaf.SetNextLeaf(2);
	ASSERT_TRUE(leaf.CheckInvariants());

	leaf.SetNextLeaf(META_PAGE_ID);
	EXPECT_FALSE(leaf.CheckInvariants());
	leaf.SetNextLeaf(CATALOG_ROOT_PAGE_ID);
	EXPECT_FALSE(leaf.CheckInvariants());
}
TEST_F(BPlusTreePageTest, LeafCheckRejectsInvalidRid) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));  // keys 10, 20, 30
	ASSERT_TRUE(leaf.CheckInvariants());

	leaf.InsertAt(1, LeafEntry{15, RID{}});  // RID{} names INVALID_PAGE
	EXPECT_FALSE(leaf.CheckInvariants());
}
TEST_F(BPlusTreePageTest, LeafCheckRejectsDuplicateKey) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));  // keys 10, 20, 30
	ASSERT_TRUE(leaf.CheckInvariants());

	leaf.InsertAt(2, LeafEntry{20, RID{3, 0}});  // 10, 20, 20, 30
	EXPECT_FALSE(leaf.CheckInvariants());
}
TEST_F(BPlusTreePageTest, LeafCheckRejectsDescendingKeys) {
	auto leaf = Leaf();
	leaf.Init();
	leaf.Assign(LeafEntries(3));  // keys 10, 20, 30
	ASSERT_TRUE(leaf.CheckInvariants());

	leaf.InsertAt(3, LeafEntry{25, RID{3, 0}});  // 10, 20, 30, 25
	EXPECT_FALSE(leaf.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Internal: Init and Assign
// ---------------------------------------------------------------------------------------------

// level as given, count 0, next_page_id INVALID_PAGE.
TEST_F(BPlusTreePageTest, InternalInitWritesHeaderAtLevel) {
	auto node = Internal();
	for (const uint16_t level : {uint16_t{1}, uint16_t{3}}) {
		node.Init(level);
		const NodeSubHeader header = node.Header();
		EXPECT_EQ(header.level, level);
		EXPECT_EQ(header.count, 0);
		EXPECT_EQ(header.next_page_id, INVALID_PAGE);
	}
}

// A zeroed body reads level 0, which CheckInvariants must reject.
TEST_F(BPlusTreePageTest, InternalNeverInitedFailsCheckInvariants) {
	EXPECT_FALSE(InternalView().CheckInvariants());
}

// entries[0].key = 42 goes in; EntryAt(0).key reads INT64_MIN and CheckInvariants passes.
TEST_F(BPlusTreePageTest, InternalAssignWritesInt64MinIntoEntryZero) {
	GTEST_SKIP() << "not written yet";
}

// The caller's span is not modified by the canonicalization.
TEST_F(BPlusTreePageTest, InternalAssignLeavesTheCallersEntriesAlone) {
	auto node = Internal();
	node.Init(1);

	auto entries = InternalEntries(3);
	entries[0].key = 42;  // Assign writes INT64_MIN to the PAGE, not back into this vector
	const auto copy = entries;

	node.Assign(entries);

	EXPECT_EQ(entries, copy);
}

// Fill to capacity, Assign 2: every byte from NodeEntryOffset(2) to capacity is zero.
TEST_F(BPlusTreePageTest, InternalAssignZeroesEveryByteToCapacity) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(NODE_CAPACITY));

	node.Assign(InternalEntries(2));

	ASSERT_EQ(node.Count(), 2);
	EXPECT_TRUE(node.CheckInvariants());
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(2)), std::nullopt);
}

// One child is legal (a root about to collapse).
TEST_F(BPlusTreePageTest, InternalAssignSingleChild) {
	auto node = Internal();
	node.Init(1);

	node.Assign(std::vector<InternalEntry>{{0, 5}});

	ASSERT_EQ(node.Count(), 1);
	EXPECT_EQ(node.ChildAt(0), 5);
	EXPECT_EQ(node.EntryAt(0).key, INT64_MIN);
	EXPECT_TRUE(node.CheckInvariants());
}

// level and next_page_id survive an Assign.
TEST_F(BPlusTreePageTest, InternalAssignPreservesTheSubHeader) {
	auto node = Internal();
	node.Init(4);

	node.Assign(InternalEntries(3));

	const NodeSubHeader header = node.Header();
	EXPECT_EQ(header.level, 4);
	EXPECT_EQ(header.count, 3);
	EXPECT_EQ(header.next_page_id, INVALID_PAGE);
}

// ---------------------------------------------------------------------------------------------
// Internal: InsertAt / RemoveAt / SetKeyAt
// ---------------------------------------------------------------------------------------------

// At Count() (append) and in the middle; entry[0] is never moved.
TEST_F(BPlusTreePageTest, InternalInsertAtShiftsTheTailRight) {
	auto node = Internal();
	node.Init(1);
	node.Assign(std::vector<InternalEntry>{{0, 2}, {20, 3}});

	node.InsertAt(2, InternalEntry{30, 4});  // append
	node.InsertAt(1, InternalEntry{10, 5});  // front-most legal position, shifts everything but [0]
	node.InsertAt(3, InternalEntry{25, 6});  // middle

	const std::vector<InternalEntry> expected{
	    {INT64_MIN, 2}, {10, 5}, {20, 3}, {25, 6}, {30, 4},
	};
	ASSERT_EQ(node.Count(), expected.size());
	for (uint16_t i = 0; i < node.Count(); ++i) {
		EXPECT_EQ(node.EntryAt(i), expected[i]) << "at index " << i;
	}
	EXPECT_TRUE(node.CheckInvariants());
}

TEST_F(BPlusTreePageTest, InternalInsertAtFillsToCapacity) {
	auto node = Internal();
	node.Init(1);
	node.Assign(std::vector<InternalEntry>{{0, 2}});

	// Always at index 1, separators descending, so the last insert shifts 251 entries up to the
	// capacity boundary — the internal twin of LeafInsertAtFillsToCapacity.
	for (uint16_t k = NODE_CAPACITY - 1; k >= 1; --k) {
		node.InsertAt(1, InternalEntry{10 * k, 2 + k});
	}

	ASSERT_EQ(node.Count(), NODE_CAPACITY);
	EXPECT_EQ(node.EntryAt(0), (InternalEntry{INT64_MIN, 2}));
	for (uint16_t i = 1; i < NODE_CAPACITY; ++i) {
		EXPECT_EQ(node.EntryAt(i), (InternalEntry{10 * i, 2 + i})) << "at index " << i;
	}
	EXPECT_TRUE(node.CheckInvariants());
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(NODE_CAPACITY)), std::nullopt);
}

// Last and middle; entry[0] is never moved.
TEST_F(BPlusTreePageTest, InternalRemoveAtShiftsTheTailLeft) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(5));  // [MIN,2] [10,3] [20,4] [30,5] [40,6]

	node.RemoveAt(4);  // last
	node.RemoveAt(2);  // middle
	node.RemoveAt(1);  // front-most legal position

	const std::vector<InternalEntry> expected{{INT64_MIN, 2}, {30, 5}};
	ASSERT_EQ(node.Count(), expected.size());
	for (uint16_t i = 0; i < node.Count(); ++i) {
		EXPECT_EQ(node.EntryAt(i), expected[i]) << "at index " << i;
	}
	EXPECT_TRUE(node.CheckInvariants());
}

TEST_F(BPlusTreePageTest, InternalRemoveAtZeroesTheVacatedSlot) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(4));

	node.RemoveAt(1);

	ASSERT_EQ(node.Count(), 3);
	EXPECT_EQ(FirstNonZeroByte(NodeEntryOffset(3)), std::nullopt);
}

// Only that entry's key changes: its child and every other entry are untouched.
TEST_F(BPlusTreePageTest, InternalSetKeyAtChangesOnlyThatKey) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(4));  // separators 10, 20, 30

	// The expected body is the old one with exactly entry[2]'s eight key bytes rewritten.
	auto expected = body_;
	const index_key_t key = 25;
	std::memcpy(expected.data() + NodeEntryOffset(2) + offsetof(InternalEntry, key), &key,
	            sizeof(key));

	node.SetKeyAt(2, key);

	EXPECT_EQ(node.KeyAt(2), key);
	EXPECT_EQ(body_, expected);
	EXPECT_TRUE(node.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Internal: ChildIndexFor
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, ChildIndexForBelowFirstSeparatorIsZero) {
	GTEST_SKIP() << "not written yet";
}

// key == KeyAt(i) goes to child i, not i - 1: separators bound their child from below.
TEST_F(BPlusTreePageTest, ChildIndexForEqualToSeparator) {
	GTEST_SKIP() << "not written yet";
}

TEST_F(BPlusTreePageTest, ChildIndexForBetweenSeparators) {
	GTEST_SKIP() << "not written yet";
}

TEST_F(BPlusTreePageTest, ChildIndexForAboveLastSeparatorIsLastChild) {
	GTEST_SKIP() << "not written yet";
}

// No separators at all: every key, including INT64_MIN and INT64_MAX, goes to child 0.
TEST_F(BPlusTreePageTest, ChildIndexForSingleChildIsAlwaysZero) {
	GTEST_SKIP() << "not written yet";
}

// Full node: for every separator and every gap, against a linear scan.
TEST_F(BPlusTreePageTest, ChildIndexForMatchesALinearScanOnAFullNode) {
	GTEST_SKIP() << "not written yet";
}

// ---------------------------------------------------------------------------------------------
// Internal: CheckInvariants rejects each corruption
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, InternalCheckRejectsCountOverCapacity) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(2));
	ASSERT_TRUE(node.CheckInvariants());

	StampSubHeader(NodeSubHeader{1, NODE_CAPACITY + 1, INVALID_PAGE});
	EXPECT_FALSE(node.CheckInvariants());
}
TEST_F(BPlusTreePageTest, InternalCheckRejectsNextPageSet) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(2));
	ASSERT_TRUE(node.CheckInvariants());

	StampSubHeader(NodeSubHeader{1, 2, 5});
	EXPECT_FALSE(node.CheckInvariants());
}
TEST_F(BPlusTreePageTest, InternalCheckRejectsEntryZeroKeyNotMin) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(2));
	ASSERT_TRUE(node.CheckInvariants());

	// Raw, because Assign would canonicalize it straight back to INT64_MIN.
	StampEntry(0, InternalEntry{42, node.ChildAt(0)});
	EXPECT_FALSE(node.CheckInvariants());
}
TEST_F(BPlusTreePageTest, InternalCheckRejectsNonIncreasingSeparators) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(3));
	ASSERT_TRUE(node.CheckInvariants());

	node.Assign(std::vector<InternalEntry>{{0, 2}, {20, 3}, {20, 4}});  // equal
	EXPECT_FALSE(node.CheckInvariants());
	node.Assign(std::vector<InternalEntry>{{0, 2}, {30, 3}, {20, 4}});  // descending
	EXPECT_FALSE(node.CheckInvariants());
}
TEST_F(BPlusTreePageTest, InternalCheckRejectsSeparatorEqualToMin) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(2));
	ASSERT_TRUE(node.CheckInvariants());

	node.Assign(std::vector<InternalEntry>{{0, 2}, {INT64_MIN, 3}});
	EXPECT_FALSE(node.CheckInvariants());
}
TEST_F(BPlusTreePageTest, InternalCheckRejectsInvalidChild) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(2));
	ASSERT_TRUE(node.CheckInvariants());

	node.Assign(std::vector<InternalEntry>{{0, INVALID_PAGE}, {10, 3}});  // leftmost child
	EXPECT_FALSE(node.CheckInvariants());
	node.Assign(std::vector<InternalEntry>{{0, 2}, {10, INVALID_PAGE}});  // a later child
	EXPECT_FALSE(node.CheckInvariants());
}
TEST_F(BPlusTreePageTest, InternalCheckRejectsReservedChild) {
	auto node = Internal();
	node.Init(1);
	node.Assign(InternalEntries(2));
	ASSERT_TRUE(node.CheckInvariants());

	node.Assign(std::vector<InternalEntry>{{0, META_PAGE_ID}, {10, 3}});
	EXPECT_FALSE(node.CheckInvariants());
	node.Assign(std::vector<InternalEntry>{{0, 2}, {10, CATALOG_ROOT_PAGE_ID}});
	EXPECT_FALSE(node.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// IndexHeader on a bare body
// ---------------------------------------------------------------------------------------------

TEST_F(BPlusTreePageTest, IndexHeaderRoundTrips) {
	const IndexHeader written{7, 3, 4, 5};
	written.WriteTo(body_);

	const IndexHeader read = IndexHeader::ReadFrom(body_);
	EXPECT_EQ(read.root_page_id, 7);
	EXPECT_EQ(read.height, 3);
	EXPECT_EQ(read.leaf_max, 4);
	EXPECT_EQ(read.internal_max, 5);
	EXPECT_EQ(read.reserved, 0);
	EXPECT_EQ(FirstNonZeroByte(sizeof(IndexHeader)), std::nullopt);
}

// ---------------------------------------------------------------------------------------------
// AsLeaf / AsInternal / ReadIndexHeader / WriteIndexHeader — need a real guard
// ---------------------------------------------------------------------------------------------

// A HEAP page, and a page of the other index type: both kCorruption, for read and write guards.
TEST_F(BPlusTreePageTest, AsLeafRejectsANonLeafPage) {
	WithBufferPool("kernsql_bptree_asleaf_reject_test", [](BufferPoolManager& bpm) {
		page_id_t page_id{};
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			page_id = guard->PageId();
			for (const PageType type :
			     {PageType::HEAP, PageType::INDEX_INTERNAL, PageType::INDEX_HEADER}) {
				guard->SetPageType(type);
				auto view = AsLeaf(*guard);
				ASSERT_FALSE(view.has_value()) << "page_type " << static_cast<int>(type);
				EXPECT_EQ(view.error().code(), ErrorCode::kCorruption);
			}
		}
		{
			auto guard = bpm.FetchPageRead(page_id);  // still INDEX_HEADER from the loop
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto view = AsLeaf(*guard);
			ASSERT_FALSE(view.has_value());
			EXPECT_EQ(view.error().code(), ErrorCode::kCorruption);
		}
	});
}
TEST_F(BPlusTreePageTest, AsLeafAcceptsALeafPage) {
	WithBufferPool("kernsql_bptree_asleaf_accept_test", [](BufferPoolManager& bpm) {
		const LeafEntry entry{10, RID{2, 0}};
		page_id_t page_id{};
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			page_id = guard->PageId();
			guard->SetPageType(PageType::INDEX_LEAF);

			auto leaf = AsLeaf(*guard);
			ASSERT_TRUE(leaf.has_value()) << leaf.error().message();
			leaf->Init();
			leaf->InsertAt(0, entry);
		}
		{
			auto guard = bpm.FetchPageRead(page_id);
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto leaf = AsLeaf(*guard);
			ASSERT_TRUE(leaf.has_value()) << leaf.error().message();
			ASSERT_EQ(leaf->Count(), 1);
			EXPECT_EQ(leaf->EntryAt(0), entry);

			// The view's span is the guard's body itself, not an offset into it.
			EXPECT_EQ(LeafEntry::ReadFrom(guard->Body().subspan<NodeEntryOffset(0), NODE_ENTRY_SIZE>()),
			          entry);
		}
	});
}
TEST_F(BPlusTreePageTest, AsInternalRejectsANonInternalPage) {
	WithBufferPool("kernsql_bptree_asinternal_reject_test", [](BufferPoolManager& bpm) {
		page_id_t page_id{};
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			page_id = guard->PageId();
			for (const PageType type :
			     {PageType::HEAP, PageType::INDEX_LEAF, PageType::INDEX_HEADER}) {
				guard->SetPageType(type);
				auto view = AsInternal(*guard);
				ASSERT_FALSE(view.has_value()) << "page_type " << static_cast<int>(type);
				EXPECT_EQ(view.error().code(), ErrorCode::kCorruption);
			}
		}
		{
			auto guard = bpm.FetchPageRead(page_id);  // still INDEX_HEADER from the loop
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto view = AsInternal(*guard);
			ASSERT_FALSE(view.has_value());
			EXPECT_EQ(view.error().code(), ErrorCode::kCorruption);
		}
	});
}
TEST_F(BPlusTreePageTest, AsInternalAcceptsAnInternalPage) {
	WithBufferPool("kernsql_bptree_asinternal_accept_test", [](BufferPoolManager& bpm) {
		page_id_t page_id{};
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			page_id = guard->PageId();
			guard->SetPageType(PageType::INDEX_INTERNAL);

			auto node = AsInternal(*guard);
			ASSERT_TRUE(node.has_value()) << node.error().message();
			node->Init(1);
			node->Assign(std::vector<InternalEntry>{{0, 2}, {10, 3}});
		}
		{
			auto guard = bpm.FetchPageRead(page_id);
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto node = AsInternal(*guard);
			ASSERT_TRUE(node.has_value()) << node.error().message();
			ASSERT_EQ(node->Count(), 2);
			EXPECT_EQ(node->ChildAt(0), 2);
			EXPECT_EQ(node->KeyAt(1), 10);
			EXPECT_EQ(node->ChildAt(1), 3);
			EXPECT_TRUE(node->CheckInvariants());
		}
	});
}

TEST_F(BPlusTreePageTest, ReadIndexHeaderRejectsANonHeaderPage) {
	WithBufferPool("kernsql_bptree_readheader_reject_test", [](BufferPoolManager& bpm) {
		page_id_t page_id{};
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			page_id = guard->PageId();
			guard->SetPageType(PageType::INDEX_LEAF);

			auto header = ReadIndexHeader(*guard);
			ASSERT_FALSE(header.has_value());
			EXPECT_EQ(header.error().code(), ErrorCode::kCorruption);
		}
		{
			auto guard = bpm.FetchPageRead(page_id);
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto header = ReadIndexHeader(*guard);
			ASSERT_FALSE(header.has_value());
			EXPECT_EQ(header.error().code(), ErrorCode::kCorruption);
		}
	});
}

// kCorruption AND the page's body bytes are unchanged — the type check runs before the write.
TEST_F(BPlusTreePageTest, WriteIndexHeaderRejectsANonHeaderPageWithoutWriting) {
	WithBufferPool("kernsql_bptree_writeheader_reject_test", [](BufferPoolManager& bpm) {
		auto guard = bpm.NewPage();
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		guard->SetPageType(PageType::INDEX_LEAF);
		{
			auto leaf = AsLeaf(*guard);
			ASSERT_TRUE(leaf.has_value()) << leaf.error().message();
			leaf->Init();
			leaf->InsertAt(0, LeafEntry{10, RID{2, 0}});
		}
		const std::vector<std::byte> before(guard->Body().begin(), guard->Body().end());

		const Status status = WriteIndexHeader(*guard, IndexHeader{9, 2, 4, 4});

		EXPECT_EQ(status.code(), ErrorCode::kCorruption);
		EXPECT_EQ(std::vector<std::byte>(guard->Body().begin(), guard->Body().end()), before);
	});
}

// Write, Shutdown, reopen, read back: survives a trip through disk, not just the frame.
TEST_F(BPlusTreePageTest, IndexHeaderSurvivesFlushAndReopen) {
	GTEST_SKIP() << "not written yet";
}

}  // namespace kernsql

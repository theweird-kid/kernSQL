#include "indices/b_plus_tree.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "indices/b_plus_tree_page.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * BPlusTree's read path and IndexIterator, against a real file and a real buffer pool.
 *
 * A test not written yet is GTEST_SKIP rather than an empty body, so it shows up as skipped in
 * ctest instead of passing silently. "needs Insert" marks a test that cannot be written until
 * Insert exists: it needs a tree with entries in it, and the API has no other way to make one.
 * Do NOT call Insert, Delete or CheckInvariants from this file before they are defined — the
 * test binary will fail to link.
 *
 * The assertions to reach for:
 *
 *   1. Exact key sequences out of a scan, not counts. A scan that returns the right number of
 *      entries and the wrong ones is the likely bug.
 *   2. Pool quiescence. TearDown's Shutdown() fails on a leaked pin, so every test checks that
 *      for free — an iterator still holding a guard at teardown shows up there.
 *   3. Leaf boundaries. With the fanout at MIN_FANOUT, every few keys is a new leaf, so the
 *      interesting intervals start, end or sit exactly on one.
 */
class BPlusTreeTest : public ::testing::Test {
  protected:
	// Splits within a handful of keys (DD-005). Every test uses the same fanout unless it is
	// about the fanout.
	static constexpr uint16_t kFanout = MIN_FANOUT;

	// A tree of height 3 at MIN_FANOUT plus a scan's single leaf guard fit comfortably. Kept
	// small on purpose, so a scan that holds more than one leaf at a time runs out of frames.
	static constexpr std::size_t kFrames = 16;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_b_plus_tree_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name());
		std::filesystem::remove(path_);

		OpenPool();

		auto created = BPlusTree::Create(*bpm_, kFanout, kFanout);
		ASSERT_TRUE(created.has_value()) << created.error().message();
		tree_ = std::move(created.value());
	}

	void TearDown() override {
		// The tree first, then Shutdown, which verifies quiescence rather than arranging it.
		tree_.reset();
		if (bpm_) {
			EXPECT_TRUE(bpm_->Shutdown().ok());
		}
		bpm_.reset();
		dm_.reset();
		std::filesystem::remove(path_);
	}

	void OpenPool() {
		auto dm = DiskManager::Open(path_);
		ASSERT_TRUE(dm.has_value()) << dm.error().message();
		dm_ = std::move(dm.value());
		bpm_ = std::make_unique<BufferPoolManager>(*dm_, kFrames);
	}

	// Close everything, reopen the file, and reopen the tree from its header page id — the one
	// number the catalog will hold. The only way to prove anything reached disk.
	void Reopen() {
		const page_id_t header = tree_->HeaderPageId();
		tree_.reset();
		ASSERT_TRUE(bpm_->Shutdown().ok());
		bpm_.reset();
		dm_.reset();

		OpenPool();
		auto opened = BPlusTree::Open(*bpm_, header);
		ASSERT_TRUE(opened.has_value()) << opened.error().message();
		tree_ = std::move(opened.value());
	}

	// Rewrite the index header in place, bypassing the tree. ONLY for tests that need a header
	// the API would never write.
	void StampHeader(const std::function<void(IndexHeader&)>& edit) {
		auto guard = bpm_->FetchPageWrite(tree_->HeaderPageId());
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		auto header = ReadIndexHeader(guard.value());
		ASSERT_TRUE(header.has_value()) << header.error().message();
		edit(header.value());
		ASSERT_TRUE(WriteIndexHeader(guard.value(), header.value()).ok());
	}

	// A RID derived from the key, so a scan that returns the right key with another key's RID
	// is caught. Not a real row — the index never dereferences it.
	static RID RidFor(index_key_t key) {
		return RID{static_cast<page_id_t>(1000 + key), static_cast<slot_id_t>(key % 7)};
	}

	// Every (key, RID) in [lo, hi], in scan order. Fails the test on a scan error rather than
	// returning short.
	std::vector<std::pair<index_key_t, RID>> CollectScan(index_key_t lo, index_key_t hi) {
		std::vector<std::pair<index_key_t, RID>> out;
		auto it = tree_->Scan(lo, hi);
		if (!it.has_value()) {
			ADD_FAILURE() << "Scan failed: " << it.error().message();
			return out;
		}
		for (;;) {
			auto more = it->Next();
			if (!more.has_value()) {
				ADD_FAILURE() << "Next failed: " << more.error().message();
				return out;
			}
			if (!more.value()) break;
			out.emplace_back(it->Key(), it->Rid());
		}
		return out;
	}

	std::filesystem::path path_;
	std::unique_ptr<DiskManager> dm_;
	std::unique_ptr<BufferPoolManager> bpm_;
	std::unique_ptr<BPlusTree> tree_;
};

// ---------------------------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------------------------

// Header page typed INDEX_HEADER with height 1, the given fanout, and a root that is an empty,
// Init()ed INDEX_LEAF (count 0, level 0, NextLeaf INVALID_PAGE).
TEST_F(BPlusTreeTest, CreateStampsHeaderAndAnEmptyRootLeaf) {
	page_id_t header_page_id = tree_->HeaderPageId();
	auto index_header = bpm_->FetchPageRead(header_page_id);
	ASSERT_TRUE(index_header.has_value()) << index_header.error().message();

	ASSERT_EQ(index_header->Header().page_type, PageType::INDEX_HEADER);
	auto header = IndexHeader::ReadFrom(index_header->Body());

	EXPECT_EQ(header.height, 1);
	EXPECT_EQ(header.internal_max, kFanout);
	EXPECT_EQ(header.leaf_max, kFanout);

	auto root_leaf = bpm_->FetchPageRead(header.root_page_id);
	ASSERT_TRUE(root_leaf.has_value()) << root_leaf.error().message();

	auto root_leaf_header = root_leaf->Header();
	EXPECT_EQ(root_leaf_header.page_type, PageType::INDEX_LEAF);

	auto leaf = AsLeaf(root_leaf.value());
	ASSERT_TRUE(leaf.has_value()) << leaf.error().message();

	EXPECT_EQ(leaf->NextLeaf(), INVALID_PAGE);
	EXPECT_EQ(leaf->Count(), 0);
	EXPECT_EQ(leaf->Header().level, 0);
}

// Neither the header nor the root lands on a reserved page (superblock, catalog).
TEST_F(BPlusTreeTest, CreateDoesNotTakeAReservedPage) {
	page_id_t header_page_id = tree_->HeaderPageId();
	ASSERT_NE(header_page_id, META_PAGE_ID);
	ASSERT_NE(header_page_id, CATALOG_ROOT_PAGE_ID);

	auto index_header = bpm_->FetchPageRead(header_page_id);
	ASSERT_TRUE(index_header.has_value()) << index_header.error().message();

	ASSERT_EQ(index_header->Header().page_type, PageType::INDEX_HEADER);
	auto header = IndexHeader::ReadFrom(index_header->Body());

	auto root_leaf = bpm_->FetchPageRead(header.root_page_id);
	ASSERT_TRUE(root_leaf.has_value()) << root_leaf.error().message();

	auto root_leaf_header = root_leaf->Header();
	ASSERT_EQ(root_leaf_header.page_type, PageType::INDEX_LEAF);

	page_id_t root_page_id = root_leaf->PageId();
	ASSERT_NE(root_page_id, META_PAGE_ID);
	ASSERT_NE(root_page_id, CATALOG_ROOT_PAGE_ID);
}

// leaf_max and internal_max below MIN_FANOUT or above NODE_CAPACITY are kInvalidArgument, and
// no page is allocated: the next NewPage() gets the id it would have got anyway.
TEST_F(BPlusTreeTest, CreateRejectsFanoutOutOfRange) {
	// SetUp's Create left the free list empty, so the next allocation extends the file and its id
	// is the current page count.
	const page_id_t before = dm_->PageCount();

	// One bad argument per case, the other valid: Create checks leaf_max first and returns, so a
	// case with both bad never reaches the internal_max check.
	struct Case {
		uint16_t leaf_max;
		uint16_t internal_max;
	};
	const Case cases[] = {
	    {static_cast<uint16_t>(MIN_FANOUT - 1), kFanout},
	    {static_cast<uint16_t>(NODE_CAPACITY + 1), kFanout},
	    {kFanout, static_cast<uint16_t>(MIN_FANOUT - 1)},
	    {kFanout, static_cast<uint16_t>(NODE_CAPACITY + 1)},
	};
	for (const Case& c : cases) {
		SCOPED_TRACE(testing::Message()
		             << "leaf_max=" << c.leaf_max << " internal_max=" << c.internal_max);
		auto tree = BPlusTree::Create(*bpm_, c.leaf_max, c.internal_max);
		ASSERT_FALSE(tree.has_value());
		EXPECT_EQ(tree.error().code(), ErrorCode::kInvalidArgument);
	}

	// Nothing grew the file, and the next id is still the one it would have been. The probe is
	// what catches a leak once the free list is non-empty: a leaked page taken from the free list
	// leaves PageCount alone but moves the next id.
	EXPECT_EQ(dm_->PageCount(), before);
	auto probe = bpm_->NewPage();
	ASSERT_TRUE(probe.has_value()) << probe.error().message();
	EXPECT_EQ(probe->PageId(), before);
}

// MIN_FANOUT and NODE_CAPACITY themselves are accepted — the range is closed at both ends.
TEST_F(BPlusTreeTest, CreateAcceptsFanoutAtBothBounds) {
	struct Case {
		uint16_t leaf_max;
		uint16_t internal_max;
	};
	const Case cases[] = {
	    {MIN_FANOUT, MIN_FANOUT},
	    {NODE_CAPACITY, NODE_CAPACITY},
	    {MIN_FANOUT, NODE_CAPACITY},
	    {NODE_CAPACITY, MIN_FANOUT},
	};
	for (const Case& c : cases) {
		SCOPED_TRACE(testing::Message()
		             << "leaf_max=" << c.leaf_max << " internal_max=" << c.internal_max);
		auto tree = BPlusTree::Create(*bpm_, c.leaf_max, c.internal_max);
		ASSERT_TRUE(tree.has_value()) << tree.error().message();
		EXPECT_EQ(tree.value()->LeafMax(), c.leaf_max);
		EXPECT_EQ(tree.value()->InternalMax(), c.internal_max);
	}
}

// ---------------------------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------------------------

// Create at a non-default fanout, Reopen(), and LeafMax()/InternalMax() come back unchanged, as
// does HeaderPageId().
TEST_F(BPlusTreeTest, OpenReadsTheFanoutBackAfterAReopen) {
	// Different from kFanout and from each other, so a swapped pair or a fanout that fell back
	// to the fixture's is caught.
	constexpr uint16_t kLeafMax = 5;
	constexpr uint16_t kInternalMax = 7;

	auto created = BPlusTree::Create(*bpm_, kLeafMax, kInternalMax);
	ASSERT_TRUE(created.has_value()) << created.error().message();
	tree_ = std::move(created.value());
	const page_id_t header = tree_->HeaderPageId();

	Reopen();

	EXPECT_EQ(tree_->HeaderPageId(), header);
	EXPECT_EQ(tree_->LeafMax(), kLeafMax);
	EXPECT_EQ(tree_->InternalMax(), kInternalMax);
}

// Open on a page that is not INDEX_HEADER (the root leaf, a heap page) is kCorruption.
TEST_F(BPlusTreeTest, OpenRejectsAPageThatIsNotAnIndexHeader) {
	page_id_t root{INVALID_PAGE};
	{
		auto guard = bpm_->FetchPageRead(tree_->HeaderPageId());
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		root = IndexHeader::ReadFrom(guard->Body()).root_page_id;
	}
	page_id_t heap{INVALID_PAGE};
	{
		auto guard = bpm_->NewPage();
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		guard->SetPageType(PageType::HEAP);
		heap = guard->PageId();
	}

	for (const page_id_t id : {root, heap}) {
		SCOPED_TRACE(testing::Message() << "page " << id);
		auto opened = BPlusTree::Open(*bpm_, id);
		ASSERT_FALSE(opened.has_value());
		EXPECT_EQ(opened.error().code(), ErrorCode::kCorruption);
	}
}

// StampHeader a leaf_max outside [MIN_FANOUT, NODE_CAPACITY]; Open is kCorruption.
TEST_F(BPlusTreeTest, OpenRejectsAnOutOfRangeFanout) {
	struct Case {
		uint16_t leaf_max;
		uint16_t internal_max;
	};
	const Case cases[] = {
	    {static_cast<uint16_t>(MIN_FANOUT - 1), kFanout},
	    {static_cast<uint16_t>(NODE_CAPACITY + 1), kFanout},
	    {kFanout, static_cast<uint16_t>(MIN_FANOUT - 1)},
	    {kFanout, static_cast<uint16_t>(NODE_CAPACITY + 1)},
	};
	for (const Case& c : cases) {
		SCOPED_TRACE(testing::Message()
		             << "leaf_max=" << c.leaf_max << " internal_max=" << c.internal_max);
		StampHeader([&](IndexHeader& h) {
			h.leaf_max = c.leaf_max;
			h.internal_max = c.internal_max;
		});
		auto opened = BPlusTree::Open(*bpm_, tree_->HeaderPageId());
		ASSERT_FALSE(opened.has_value());
		EXPECT_EQ(opened.error().code(), ErrorCode::kCorruption);
	}
}

// Open on a page id past the end of the file fails rather than handing back a tree.
TEST_F(BPlusTreeTest, OpenOnAPageThatDoesNotExistFails) {
	auto opened = BPlusTree::Open(*bpm_, dm_->PageCount() + 10);
	EXPECT_FALSE(opened.has_value());
}

// ---------------------------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------------------------

// Any key, including INT64_MIN and INT64_MAX, is kNotFound on a freshly created tree.
TEST_F(BPlusTreeTest, GetOnAnEmptyTreeIsNotFound) {
	for (const index_key_t key : {INT64_MIN, index_key_t{-1}, index_key_t{0}, index_key_t{1}, INT64_MAX}) {
		SCOPED_TRACE(testing::Message() << "key " << key);
		auto rid = tree_->Get(key);
		ASSERT_FALSE(rid.has_value());
		EXPECT_EQ(rid.error().code(), ErrorCode::kNotFound);
	}
}

// StampHeader height = 0; Get is kCorruption, not a read of the root with a garbage position.
TEST_F(BPlusTreeTest, GetOnAZeroHeightHeaderIsCorruption) {
	StampHeader([](IndexHeader& h) { h.height = 0; });
	auto rid = tree_->Get(0);
	ASSERT_FALSE(rid.has_value());
	EXPECT_EQ(rid.error().code(), ErrorCode::kCorruption);
}

// StampHeader height = 2 over a single root leaf; the descent asks AsInternal of a leaf and
// reports kCorruption instead of walking off the tree.
TEST_F(BPlusTreeTest, GetOnAHeightTooLargeIsCorruption) {
	StampHeader([](IndexHeader& h) { h.height = 2; });
	auto rid = tree_->Get(0);
	ASSERT_FALSE(rid.has_value());
	EXPECT_EQ(rid.error().code(), ErrorCode::kCorruption);
}

// Grow the tree to height 2, StampHeader height = 1; the caller's AsLeaf on an internal root
// reports kCorruption.
TEST_F(BPlusTreeTest, GetOnAHeightTooSmallIsCorruption) {
	GTEST_SKIP() << "needs Insert";
}

// Every inserted key comes back with ITS OWN RidFor(key), at heights 1, 2 and 3.
TEST_F(BPlusTreeTest, GetFindsEveryInsertedKey) {
	GTEST_SKIP() << "needs Insert";
}

// Absent keys below every key, above every key, and between two keys in the SAME leaf and
// across a leaf boundary are all kNotFound. The across-a-boundary case is the one where
// LowerBound returns Count().
TEST_F(BPlusTreeTest, GetOnAnAbsentKeyIsNotFound) {
	GTEST_SKIP() << "needs Insert";
}

// A key equal to a separator is found in the RIGHT child — separators are copies of the right
// child's first key.
TEST_F(BPlusTreeTest, GetFindsAKeyEqualToASeparator) {
	GTEST_SKIP() << "needs Insert";
}

// Insert, Reopen(), and every key is still found. Proves the tree, not just the header,
// reached disk.
TEST_F(BPlusTreeTest, KeysSurviveAReopen) {
	GTEST_SKIP() << "needs Insert";
}

// ---------------------------------------------------------------------------------------------
// Scan — positioning and the empty cases
// ---------------------------------------------------------------------------------------------

// [INT64_MIN, INT64_MAX] on a fresh tree: the first Next() is false, not an entry read past
// the empty root leaf's count.
TEST_F(BPlusTreeTest, ScanOnAnEmptyTreeYieldsNothing) {
	EXPECT_TRUE(CollectScan(INT64_MIN, INT64_MAX).empty());
}

// lo > hi yields nothing, with no special case: LowerBound(lo) lands on a key above hi. Check
// on an empty tree now, and again once Insert exists with lo and hi both inside the key range.
TEST_F(BPlusTreeTest, ScanWithLoAboveHiYieldsNothing) {
	EXPECT_TRUE(CollectScan(10, 5).empty());
	EXPECT_TRUE(CollectScan(INT64_MAX, INT64_MIN).empty());
}

// StampHeader height = 0; Scan itself fails with kCorruption, before any iterator exists.
TEST_F(BPlusTreeTest, ScanOnAZeroHeightHeaderIsCorruption) {
	StampHeader([](IndexHeader& h) { h.height = 0; });
	auto it = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_FALSE(it.has_value());
	EXPECT_EQ(it.error().code(), ErrorCode::kCorruption);
}

// ---------------------------------------------------------------------------------------------
// Scan — intervals over a multi-leaf tree
//
// The reference layout for these: insert enough keys that one leaf ends at 7 and the next
// starts at 10 (e.g. 3 5 7 | 10 12 15 | 20 25 at fanout 3). Assert exact key sequences.
// ---------------------------------------------------------------------------------------------

// [INT64_MIN, INT64_MAX] returns every key, ascending, each with its own RidFor(key).
TEST_F(BPlusTreeTest, ScanOfTheWholeDomainVisitsEveryKeyInOrder) {
	GTEST_SKIP() << "needs Insert";
}

// Scan(8, 11): the descent for 8 reaches the leaf ending at 7, LowerBound is Count(), and the
// first result must be 10 — not an entry read past that leaf's end.
TEST_F(BPlusTreeTest, ScanStartingPastALeafsLastKeyStepsToTheNextLeaf) {
	GTEST_SKIP() << "needs Insert";
}

// Scan(3, 6): hi is absent, so the scan stops at the first key ABOVE it. Returns 3 5 — not 7.
TEST_F(BPlusTreeTest, ScanStopsBeforeTheFirstKeyAboveAnAbsentHi) {
	GTEST_SKIP() << "needs Insert";
}

// Scan(3, 7): hi is the last key of its leaf. Returns 3 5 7 and stops — stepping to the next
// leaf must not undo the stop.
TEST_F(BPlusTreeTest, ScanStopsAtAHiThatEndsALeaf) {
	GTEST_SKIP() << "needs Insert";
}

// Scan(5, 20): starts mid-leaf, crosses two leaf boundaries, ends on a present hi.
TEST_F(BPlusTreeTest, ScanCrossesSeveralLeafBoundaries) {
	GTEST_SKIP() << "needs Insert";
}

// lo == hi: one entry when the key is present, none when absent.
TEST_F(BPlusTreeTest, ScanOfASingleKeyInterval) {
	GTEST_SKIP() << "needs Insert";
}

// lo below every key and hi above every key, using INT64_MIN and INT64_MAX themselves as
// stored keys too — nothing at the ends of the domain overflows.
TEST_F(BPlusTreeTest, ScanAtTheEndsOfTheKeyDomain) {
	GTEST_SKIP() << "needs Insert";
}

// An interval entirely between two adjacent keys, e.g. (8, 9), yields nothing.
TEST_F(BPlusTreeTest, ScanOfAGapBetweenKeysYieldsNothing) {
	GTEST_SKIP() << "needs Insert";
}

// Scan, Reopen(), Scan again: identical sequences. The leaf chain reached disk too.
TEST_F(BPlusTreeTest, LeafChainSurvivesAReopen) {
	GTEST_SKIP() << "needs Insert";
}

// ---------------------------------------------------------------------------------------------
// IndexIterator — state, guards, moves
// ---------------------------------------------------------------------------------------------

// Once Next() has returned false, it keeps returning false, and the pool shows no pin held.
TEST_F(BPlusTreeTest, NextIsIdempotentOnceExhausted) {
	auto it = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(it.has_value()) << it.error().message();
	for (int i = 0; i < 3; i++) {
		SCOPED_TRACE(testing::Message() << "call " << i);
		auto more = it->Next();
		ASSERT_TRUE(more.has_value()) << more.error().message();
		EXPECT_FALSE(more.value());
		EXPECT_EQ(bpm_->GetStats().pinned_frames, 0u);
	}
}

// Key() and Rid() are copies: they still name the entry after the iterator has moved on to the
// next leaf, and after the iterator is destroyed.
TEST_F(BPlusTreeTest, EntryOutlivesTheIteratorMovingOn) {
	GTEST_SKIP() << "needs Insert";
}

// Mid-scan, exactly one frame is pinned by the iterator (GetStats().pinned_frames == 1), and it
// is still one after crossing a leaf boundary — the old leaf was released, not leaked.
TEST_F(BPlusTreeTest, IteratorHoldsOneLeafAtATime) {
	GTEST_SKIP() << "needs Insert";
}

// After a scan runs to its end (hi reached, or the chain ended), pinned_frames is 0 — the
// iterator released its guard on finishing, not only when destroyed.
TEST_F(BPlusTreeTest, ExhaustedIteratorHoldsNoPin) {
	GTEST_SKIP() << "needs Insert";
}

// Move-construct from an iterator; the source's Next() is false, and the destination continues
// from exactly where the source was. Check on an empty tree now, mid-scan once Insert exists.
TEST_F(BPlusTreeTest, MovedFromIteratorIsExhausted) {
	auto source = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(source.has_value()) << source.error().message();

	IndexIterator dest(std::move(source.value()));

	auto from_source = source->Next();
	ASSERT_TRUE(from_source.has_value()) << from_source.error().message();
	EXPECT_FALSE(from_source.value());

	auto from_dest = dest.Next();
	ASSERT_TRUE(from_dest.has_value()) << from_dest.error().message();
	EXPECT_FALSE(from_dest.value());
	EXPECT_EQ(bpm_->GetStats().pinned_frames, 0u);
}

// Move-assign a mid-scan iterator over another mid-scan iterator on a different leaf;
// pinned_frames drops by one — the target released its own guard before taking the source's.
TEST_F(BPlusTreeTest, MoveAssignmentReleasesTheTargetsPin) {
	GTEST_SKIP() << "needs Insert";
}

// Self-move-assignment leaves the iterator usable and its position unchanged.
TEST_F(BPlusTreeTest, SelfMoveAssignmentIsHarmless) {
	GTEST_SKIP() << "needs Insert";
}

// Overwrite the second leaf's page type through a raw write guard, then scan across it: Next()
// returns the error, holds no pin, and every later Next() is false.
TEST_F(BPlusTreeTest, NextReportsACorruptLeafAndStaysExhausted) {
	GTEST_SKIP() << "needs Insert";
}

// Collect the keys from a scan, let the iterator die, then Insert/Delete each — completes
// rather than hanging. The documented pattern for "mutate what you scanned".
TEST_F(BPlusTreeTest, CollectThenMutateDoesNotDeadlock) {
	GTEST_SKIP() << "needs Insert";
}

}  // namespace kernsql

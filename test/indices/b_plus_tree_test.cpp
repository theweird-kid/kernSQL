#include "indices/b_plus_tree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "indices/b_plus_tree_page.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * BPlusTree's Insert, read path and IndexIterator, against a real file and a real buffer pool.
 *
 * A test not written yet is GTEST_SKIP rather than an empty body, so it shows up as skipped in
 * ctest instead of passing silently.
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
		// The pid keeps two presets running the same test at once off each other's file.
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_b_plus_tree_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
		         std::to_string(getpid()));
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
	// is caught. Not a real row — the index never dereferences it. `key % 100000` rather than
	// `key`, so INT64_MAX does not overflow the addition.
	static RID RidFor(index_key_t key) {
		return RID{static_cast<page_id_t>(1000 + key % 100000), static_cast<slot_id_t>(key % 7)};
	}

	// The expected scan output for `keys`: each with its own RidFor.
	static std::vector<std::pair<index_key_t, RID>> WithRids(const std::vector<index_key_t>& keys) {
		std::vector<std::pair<index_key_t, RID>> out;
		for (const index_key_t key : keys) out.emplace_back(key, RidFor(key));
		return out;
	}

	// Insert each key with its RidFor, in order, and check the whole tree after every one.
	void InsertAll(const std::vector<index_key_t>& keys) {
		for (const index_key_t key : keys) {
			const Status s = tree_->Insert(key, RidFor(key));
			ASSERT_TRUE(s.ok()) << "key " << key << ": " << s.message();
			ASSERT_TRUE(tree_->CheckInvariants()) << "after key " << key;
		}
	}

	// 1 .. n in a fixed shuffled order — neither ascending nor descending, so splits land in
	// the left half, the right half and the middle.
	static std::vector<index_key_t> ShuffledKeys(index_key_t n) {
		std::vector<index_key_t> keys(static_cast<std::size_t>(n));
		std::iota(keys.begin(), keys.end(), index_key_t{1});
		std::shuffle(keys.begin(), keys.end(), std::mt19937{42});
		return keys;
	}

	IndexHeader ReadHeader() {
		auto guard = bpm_->FetchPageRead(tree_->HeaderPageId());
		if (!guard.has_value()) {
			ADD_FAILURE() << guard.error().message();
			return {};
		}
		auto header = ReadIndexHeader(guard.value());
		if (!header.has_value()) {
			ADD_FAILURE() << header.error().message();
			return {};
		}
		return header.value();
	}
	uint16_t Height() { return ReadHeader().height; }

	// Every leaf's page id, left to right: down child 0 to the leftmost leaf, then along the
	// NextLeaf chain. Capped, so a cycle in a broken chain fails instead of hanging.
	std::vector<page_id_t> LeafPages() {
		std::vector<page_id_t> pages;
		const IndexHeader header = ReadHeader();
		page_id_t id = header.root_page_id;
		for (uint16_t level = header.height; level > 1; --level) {
			auto guard = bpm_->FetchPageRead(id);
			if (!guard.has_value()) {
				ADD_FAILURE() << guard.error().message();
				return pages;
			}
			auto node = AsInternal(guard.value());
			if (!node.has_value()) {
				ADD_FAILURE() << node.error().message();
				return pages;
			}
			id = node->ChildAt(0);
		}
		while (id != INVALID_PAGE) {
			if (pages.size() > 10'000) {
				ADD_FAILURE() << "leaf chain does not end";
				return pages;
			}
			pages.push_back(id);
			auto guard = bpm_->FetchPageRead(id);
			if (!guard.has_value()) {
				ADD_FAILURE() << guard.error().message();
				return pages;
			}
			auto leaf = AsLeaf(guard.value());
			if (!leaf.has_value()) {
				ADD_FAILURE() << leaf.error().message();
				return pages;
			}
			id = leaf->NextLeaf();
		}
		return pages;
	}

	// The keys of every leaf, left to right — the tree's shape at the bottom level.
	std::vector<std::vector<index_key_t>> LeafKeys() {
		std::vector<std::vector<index_key_t>> out;
		for (const page_id_t id : LeafPages()) {
			auto guard = bpm_->FetchPageRead(id);
			if (!guard.has_value()) {
				ADD_FAILURE() << guard.error().message();
				return out;
			}
			auto leaf = AsLeaf(guard.value());
			if (!leaf.has_value()) {
				ADD_FAILURE() << leaf.error().message();
				return out;
			}
			auto& keys = out.emplace_back();
			for (uint16_t i = 0; i < leaf->Count(); ++i) keys.push_back(leaf->KeyAt(i));
		}
		return out;
	}

	// The reference layout the Scan tests are written against, at MIN_FANOUT:
	//
	//     root: [10, 20]
	//     leaves: 3 5 7 | 10 12 15 | 20 25
	//
	// 3 5 10 12 splits into 3 5 | 10 12; 20 25 split the right leaf into 10 12 | 20 25; 7 and
	// 15 then fill the first two leaves without splitting. Asserted, so a change to the split
	// point fails here and not as a confusing scan mismatch.
	static inline const std::vector<index_key_t> kReferenceKeys{3, 5, 7, 10, 12, 15, 20, 25};
	void BuildReferenceTree() {
		ASSERT_NO_FATAL_FAILURE(InsertAll({3, 5, 10, 12, 20, 25, 7, 15}));
		ASSERT_EQ(Height(), 2);
		ASSERT_EQ(LeafKeys(),
		          (std::vector<std::vector<index_key_t>>{{3, 5, 7}, {10, 12, 15}, {20, 25}}));
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

	// One Next() that must yield `key` with its RidFor.
	static void ExpectNext(IndexIterator& it, index_key_t key) {
		auto more = it.Next();
		ASSERT_TRUE(more.has_value()) << more.error().message();
		ASSERT_TRUE(more.value()) << "exhausted, expected " << key;
		EXPECT_EQ(it.Key(), key);
		EXPECT_EQ(it.Rid(), RidFor(key));
	}

	// One Next() that must report the end.
	static void ExpectExhausted(IndexIterator& it) {
		auto more = it.Next();
		ASSERT_TRUE(more.has_value()) << more.error().message();
		EXPECT_FALSE(more.value()) << "yielded " << it.Key();
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
// Insert
// ---------------------------------------------------------------------------------------------

// Into the empty root leaf: found, height still 1, root page unchanged, no page allocated.
TEST_F(BPlusTreeTest, InsertWithoutASplitStaysInTheRootLeaf) {
	const page_id_t root = ReadHeader().root_page_id;
	const page_id_t pages = dm_->PageCount();

	ASSERT_NO_FATAL_FAILURE(InsertAll({20, 10, 30}));

	EXPECT_EQ(Height(), 1);
	EXPECT_EQ(ReadHeader().root_page_id, root);
	EXPECT_EQ(dm_->PageCount(), pages);
	EXPECT_EQ(LeafKeys(), (std::vector<std::vector<index_key_t>>{{10, 20, 30}}));
}

// A present key is kDuplicateKey and changes nothing: the old RID survives, no page is
// allocated, the leaves are as they were. 7 and 15 sit in FULL leaves — the duplicate check
// must come before the split, or a duplicate splits a leaf and then fails.
TEST_F(BPlusTreeTest, InsertOfAPresentKeyIsDuplicateKeyAndChangesNothing) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	const auto leaves = LeafKeys();
	const page_id_t pages = dm_->PageCount();

	for (const index_key_t key : {index_key_t{7}, index_key_t{15}, index_key_t{25}}) {
		SCOPED_TRACE(testing::Message() << "key " << key);
		const Status s = tree_->Insert(key, RID{9, 9});
		EXPECT_EQ(s.code(), ErrorCode::kDuplicateKey);

		auto rid = tree_->Get(key);
		ASSERT_TRUE(rid.has_value()) << rid.error().message();
		EXPECT_EQ(rid.value(), RidFor(key));
	}

	EXPECT_EQ(dm_->PageCount(), pages);
	EXPECT_EQ(LeafKeys(), leaves);
	EXPECT_TRUE(tree_->CheckInvariants());
}

// The first split: the old root leaf becomes the LEFT leaf, and a new internal root at level 1
// holds [old root, new leaf] with the right leaf's first key as its separator.
TEST_F(BPlusTreeTest, InsertIntoAFullRootLeafGrowsANewRoot) {
	ASSERT_NO_FATAL_FAILURE(InsertAll({1, 2, 3}));
	const page_id_t old_root = ReadHeader().root_page_id;
	ASSERT_EQ(Height(), 1);

	ASSERT_NO_FATAL_FAILURE(InsertAll({4}));

	const IndexHeader header = ReadHeader();
	EXPECT_EQ(header.height, 2);
	EXPECT_NE(header.root_page_id, old_root);
	EXPECT_EQ(LeafKeys(), (std::vector<std::vector<index_key_t>>{{1, 2}, {3, 4}}));

	const auto leaves = LeafPages();
	ASSERT_EQ(leaves.size(), 2u);
	EXPECT_EQ(leaves[0], old_root);

	auto guard = bpm_->FetchPageRead(header.root_page_id);
	ASSERT_TRUE(guard.has_value()) << guard.error().message();
	auto root = AsInternal(guard.value());
	ASSERT_TRUE(root.has_value()) << root.error().message();
	EXPECT_EQ(root->Level(), 1);
	ASSERT_EQ(root->Count(), 2);
	EXPECT_EQ(root->ChildAt(0), leaves[0]);
	EXPECT_EQ(root->ChildAt(1), leaves[1]);
	EXPECT_EQ(root->KeyAt(1), 3);
}

// Keep inserting ascending keys until the root itself is an internal node that split: height
// 3, a root at level 2 with exactly two children — a root split only ever makes two.
TEST_F(BPlusTreeTest, InsertCascadesThroughAFullInternalRoot) {
	index_key_t key = 0;
	while (Height() < 3) {
		ASSERT_LT(key, 100) << "never reached height 3";
		ASSERT_NO_FATAL_FAILURE(InsertAll({++key}));
	}

	{
		auto guard = bpm_->FetchPageRead(ReadHeader().root_page_id);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		auto root = AsInternal(guard.value());
		ASSERT_TRUE(root.has_value()) << root.error().message();
		EXPECT_EQ(root->Level(), 2);
		EXPECT_EQ(root->Count(), 2);
	}

	std::vector<index_key_t> all(static_cast<std::size_t>(key));
	std::iota(all.begin(), all.end(), index_key_t{1});
	EXPECT_EQ(CollectScan(INT64_MIN, INT64_MAX), WithRids(all));
}

// Ascending, descending and shuffled orders each stay valid after every insert (InsertAll
// checks) and scan back as exactly 1 .. n. Ascending always splits at the right edge,
// descending at the left; shuffled hits the middle.
TEST_F(BPlusTreeTest, InsertInAnyOrderKeepsTheTreeValid) {
	constexpr index_key_t kN = 30;
	std::vector<index_key_t> ascending(static_cast<std::size_t>(kN));
	std::iota(ascending.begin(), ascending.end(), index_key_t{1});
	const std::vector<index_key_t> descending(ascending.rbegin(), ascending.rend());

	struct Case {
		const char* name;
		std::vector<index_key_t> keys;
	};
	const Case cases[] = {
	    {"ascending", ascending},
	    {"descending", descending},
	    {"shuffled", ShuffledKeys(kN)},
	};
	for (const Case& c : cases) {
		SCOPED_TRACE(c.name);
		auto created = BPlusTree::Create(*bpm_, kFanout, kFanout);
		ASSERT_TRUE(created.has_value()) << created.error().message();
		tree_ = std::move(created.value());

		ASSERT_NO_FATAL_FAILURE(InsertAll(c.keys));
		EXPECT_EQ(CollectScan(INT64_MIN, INT64_MAX), WithRids(ascending));
	}
}

// Allocation fails partway through a cascading split (a small pool with frames pinned
// elsewhere, so NewPage fails on the 2nd or 3rd page). Insert returns the error, and the tree
// is untouched: CheckInvariants passes, every old key is found, the new key is not, the leaves
// are byte-for-byte as before, and no page leaked. Then a retry with the pool freed succeeds.
TEST_F(BPlusTreeTest, InsertThatCannotAllocateLeavesTheTreeUntouched) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // root [10, 20] full; leaf 3 5 7 full

	// Every tree page's bytes, taken before anything is pinned.
	std::map<page_id_t, std::vector<std::byte>> snapshot;
	auto take_snapshot = [&] {
		std::map<page_id_t, std::vector<std::byte>> out;
		std::vector<page_id_t> ids = LeafPages();
		ids.push_back(tree_->HeaderPageId());
		ids.push_back(ReadHeader().root_page_id);
		for (const page_id_t id : ids) {
			auto guard = bpm_->FetchPageRead(id);
			if (!guard.has_value()) {
				ADD_FAILURE() << guard.error().message();
				continue;
			}
			const auto body = guard->Body();
			out[id] = std::vector<std::byte>(body.begin(), body.end());
		}
		return out;
	};
	snapshot = take_snapshot();
	ASSERT_EQ(snapshot.size(), 5u);  // header, root, three leaves

	// Insert(4) splits the leaf, then the full root, then needs a new root: 3 new pages. While
	// it allocates it holds the header and the root-to-leaf path, 1 + height = 3 frames. Leave
	// exactly one more free, so the FIRST NewPage succeeds and the SECOND fails — the case where
	// AllocatePages has something to roll back. The pins are fresh pages, never tree pages: a
	// guard on a tree page would make Insert wait on this same thread forever.
	const std::size_t held_by_insert = 1 + Height();
	std::vector<WritePageGuard> pins;
	for (std::size_t i = 0; i < kFrames - held_by_insert - 1; ++i) {
		auto guard = bpm_->NewPage();
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		pins.push_back(std::move(guard.value()));
	}
	const page_id_t before = dm_->PageCount();

	const Status s = tree_->Insert(4, RidFor(4));
	EXPECT_EQ(s.code(), ErrorCode::kBufferPoolFull) << s.message();

	pins.clear();

	// Untouched: valid, every old key with its own RID, the new key absent, every byte the same.
	EXPECT_TRUE(tree_->CheckInvariants());
	for (const index_key_t key : kReferenceKeys) {
		auto rid = tree_->Get(key);
		ASSERT_TRUE(rid.has_value()) << "key " << key << ": " << rid.error().message();
		EXPECT_EQ(rid.value(), RidFor(key)) << "key " << key;
	}
	auto absent = tree_->Get(4);
	ASSERT_FALSE(absent.has_value());
	EXPECT_EQ(absent.error().code(), ErrorCode::kNotFound);
	EXPECT_EQ(take_snapshot(), snapshot);

	// The first allocation extended the file by one page, and the rollback put that page on the
	// free list: the file grew by exactly one, and the next NewPage hands that page back. Together
	// they show the failure came on the second page and the first was freed, not leaked.
	EXPECT_EQ(dm_->PageCount(), before + 1);
	{
		auto probe = bpm_->NewPage();
		ASSERT_TRUE(probe.has_value()) << probe.error().message();
		EXPECT_EQ(probe->PageId(), before);
	}

	// With the pool free again, the same insert goes through.
	ASSERT_NO_FATAL_FAILURE(InsertAll({4}));
	EXPECT_EQ(Height(), 3);
	auto rid = tree_->Get(4);
	ASSERT_TRUE(rid.has_value()) << rid.error().message();
	EXPECT_EQ(rid.value(), RidFor(4));
}

// ---------------------------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------------------------

// Any key, including INT64_MIN and INT64_MAX, is kNotFound on a freshly created tree.
TEST_F(BPlusTreeTest, GetOnAnEmptyTreeIsNotFound) {
	for (const index_key_t key :
	     {INT64_MIN, index_key_t{-1}, index_key_t{0}, index_key_t{1}, INT64_MAX}) {
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
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	StampHeader([](IndexHeader& h) { h.height = 1; });
	auto rid = tree_->Get(3);
	ASSERT_FALSE(rid.has_value());
	EXPECT_EQ(rid.error().code(), ErrorCode::kCorruption);
}

// Every inserted key comes back with ITS OWN RidFor(key), at heights 1, 2 and 3. Every key so
// far is looked up after every insert, so each height is checked with every shape it passes
// through, not just the last.
TEST_F(BPlusTreeTest, GetFindsEveryInsertedKey) {
	std::set<uint16_t> heights;
	std::vector<index_key_t> inserted;
	for (const index_key_t key : ShuffledKeys(100)) {
		ASSERT_NO_FATAL_FAILURE(InsertAll({key}));
		const uint16_t height = Height();
		if (height > 3) break;
		heights.insert(height);
		inserted.push_back(key);

		for (const index_key_t k : inserted) {
			auto rid = tree_->Get(k);
			ASSERT_TRUE(rid.has_value()) << "key " << k << ": " << rid.error().message();
			EXPECT_EQ(rid.value(), RidFor(k)) << "key " << k;
		}
	}
	EXPECT_EQ(heights, (std::set<uint16_t>{1, 2, 3}));
}

// Absent keys below every key, above every key, and between two keys in the SAME leaf and
// across a leaf boundary are all kNotFound. The across-a-boundary case is the one where
// LowerBound returns Count().
TEST_F(BPlusTreeTest, GetOnAnAbsentKeyIsNotFound) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // 3 5 7 | 10 12 15 | 20 25

	const index_key_t absent[] = {
	    INT64_MIN, 0,         2,       // below every key
	    4,         6,         11, 21,  // between two keys in the same leaf
	    8,         9,         16, 19,  // past the last key of a leaf, below the next leaf's first
	    26,        INT64_MAX,          // above every key
	};
	for (const index_key_t key : absent) {
		SCOPED_TRACE(testing::Message() << "key " << key);
		auto rid = tree_->Get(key);
		ASSERT_FALSE(rid.has_value());
		EXPECT_EQ(rid.error().code(), ErrorCode::kNotFound);
	}
}

// A key equal to a separator is found in the RIGHT child — separators are copies of the right
// child's first key.
TEST_F(BPlusTreeTest, GetFindsAKeyEqualToASeparator) {
	BuildReferenceTree();
	page_id_t idx_page_id = tree_->HeaderPageId();

	auto pg = bpm_->FetchPageRead(idx_page_id);
	ASSERT_TRUE(pg.has_value());

	auto idx_header = IndexHeader::ReadFrom(pg->Body());
	page_id_t root_id = idx_header.root_page_id;

	{
		auto root_page = bpm_->FetchPageRead(root_id);
		ASSERT_TRUE(root_page.has_value());

		auto root = AsInternal(root_page.value());
		ASSERT_EQ(root->KeyAt(1), 10);
		ASSERT_EQ(root->KeyAt(2), 20);
	}

	ASSERT_TRUE(tree_->Get(10).has_value());
	ASSERT_TRUE(tree_->Get(20).has_value());
}

// Insert, Reopen(), and every key is still found. Proves the tree, not just the header,
// reached disk.
TEST_F(BPlusTreeTest, KeysSurviveAReopen) {
	const auto keys = ShuffledKeys(40);
	ASSERT_NO_FATAL_FAILURE(InsertAll(keys));
	const uint16_t height = Height();
	ASSERT_GE(height, 3);

	Reopen();

	EXPECT_EQ(Height(), height);
	EXPECT_TRUE(tree_->CheckInvariants());
	for (const index_key_t key : keys) {
		auto rid = tree_->Get(key);
		ASSERT_TRUE(rid.has_value()) << "key " << key << ": " << rid.error().message();
		EXPECT_EQ(rid.value(), RidFor(key)) << "key " << key;
	}
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

	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // 3 5 7 | 10 12 15 | 20 25
	EXPECT_TRUE(CollectScan(12, 10).empty());       // both present, one leaf
	EXPECT_TRUE(CollectScan(20, 5).empty());        // both present, across leaves
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
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	EXPECT_EQ(CollectScan(INT64_MIN, INT64_MAX), WithRids(kReferenceKeys));
}

// Scan(8, 11): the descent for 8 reaches the leaf ending at 7, LowerBound is Count(), and the
// first result must be 10 — not an entry read past that leaf's end.
TEST_F(BPlusTreeTest, ScanStartingPastALeafsLastKeyStepsToTheNextLeaf) {
	BuildReferenceTree();
	ASSERT_EQ(CollectScan(8, 11), WithRids({10}));
	ASSERT_EQ(CollectScan(16, 20), WithRids({20}));
	ASSERT_EQ(CollectScan(8, INT64_MAX), WithRids({10, 12, 15, 20, 25}));
}

// Scan(3, 6): hi is absent, so the scan stops at the first key ABOVE it. Returns 3 5 — not 7.
TEST_F(BPlusTreeTest, ScanStopsBeforeTheFirstKeyAboveAnAbsentHi) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	EXPECT_EQ(CollectScan(3, 6), WithRids({3, 5}));
}

// Scan(3, 7): hi is the last key of its leaf. Returns 3 5 7 and stops — stepping to the next
// leaf must not undo the stop.
TEST_F(BPlusTreeTest, ScanStopsAtAHiThatEndsALeaf) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	EXPECT_EQ(CollectScan(3, 7), WithRids({3, 5, 7}));
	EXPECT_EQ(CollectScan(10, 15), WithRids({10, 12, 15}));
}

// Scan(5, 20): starts mid-leaf, crosses two leaf boundaries, ends on a present hi.
TEST_F(BPlusTreeTest, ScanCrossesSeveralLeafBoundaries) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	EXPECT_EQ(CollectScan(5, 20), WithRids({5, 7, 10, 12, 15, 20}));
}

// lo == hi: one entry when the key is present, none when absent.
TEST_F(BPlusTreeTest, ScanOfASingleKeyInterval) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());
	// Mid-leaf, last of a leaf, first of a leaf, last of the tree.
	for (const index_key_t key :
	     {index_key_t{5}, index_key_t{7}, index_key_t{10}, index_key_t{25}}) {
		SCOPED_TRACE(testing::Message() << "key " << key);
		EXPECT_EQ(CollectScan(key, key), WithRids({key}));
	}
	for (const index_key_t key : {index_key_t{4}, index_key_t{8}, index_key_t{26}}) {
		SCOPED_TRACE(testing::Message() << "key " << key);
		EXPECT_TRUE(CollectScan(key, key).empty());
	}
}

// lo below every key and hi above every key, using INT64_MIN and INT64_MAX themselves as
// stored keys too — nothing at the ends of the domain overflows.
TEST_F(BPlusTreeTest, ScanAtTheEndsOfTheKeyDomain) {
	const std::vector<index_key_t> keys{INT64_MIN, INT64_MIN + 1, -1,       0,
	                                    1,         INT64_MAX - 1, INT64_MAX};
	ASSERT_NO_FATAL_FAILURE(InsertAll(keys));
	ASSERT_GE(Height(), 2);

	EXPECT_EQ(CollectScan(INT64_MIN, INT64_MAX), WithRids(keys));
	EXPECT_EQ(CollectScan(INT64_MIN, INT64_MIN), WithRids({INT64_MIN}));
	EXPECT_EQ(CollectScan(INT64_MAX, INT64_MAX), WithRids({INT64_MAX}));
	EXPECT_EQ(CollectScan(INT64_MIN, -1), WithRids({INT64_MIN, INT64_MIN + 1, -1}));
	EXPECT_EQ(CollectScan(1, INT64_MAX), WithRids({1, INT64_MAX - 1, INT64_MAX}));
	EXPECT_TRUE(CollectScan(2, INT64_MAX - 2).empty());

	for (const index_key_t key : {INT64_MIN, INT64_MAX}) {
		auto rid = tree_->Get(key);
		ASSERT_TRUE(rid.has_value()) << "key " << key << ": " << rid.error().message();
		EXPECT_EQ(rid.value(), RidFor(key));
	}
}

// An interval entirely between two adjacent keys, e.g. (8, 9), yields nothing.
TEST_F(BPlusTreeTest, ScanOfAGapBetweenKeysYieldsNothing) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());    // 3 5 7 | 10 12 15 | 20 25
	EXPECT_TRUE(CollectScan(8, 9).empty());           // across a leaf boundary
	EXPECT_TRUE(CollectScan(16, 19).empty());         // across a leaf boundary
	EXPECT_TRUE(CollectScan(13, 14).empty());         // inside one leaf
	EXPECT_TRUE(CollectScan(INT64_MIN, 2).empty());   // below every key
	EXPECT_TRUE(CollectScan(26, INT64_MAX).empty());  // above every key
}

// Scan, Reopen(), Scan again: identical sequences. The leaf chain reached disk too.
TEST_F(BPlusTreeTest, LeafChainSurvivesAReopen) {
	ASSERT_NO_FATAL_FAILURE(InsertAll(ShuffledKeys(40)));
	const auto scan = CollectScan(INT64_MIN, INT64_MAX);
	const auto pages = LeafPages();
	ASSERT_EQ(scan.size(), 40u);

	Reopen();

	EXPECT_EQ(LeafPages(), pages);
	EXPECT_EQ(CollectScan(INT64_MIN, INT64_MAX), scan);
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
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // 3 5 7 | 10 12 15 | 20 25

	index_key_t key = 0;
	RID rid{};
	{
		auto it = tree_->Scan(7, INT64_MAX);
		ASSERT_TRUE(it.has_value()) << it.error().message();
		ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, 7));
		key = it->Key();
		rid = it->Rid();

		ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, 10));  // onto the next leaf
		EXPECT_EQ(key, 7);
		EXPECT_EQ(rid, RidFor(7));
	}
	EXPECT_EQ(key, 7);
	EXPECT_EQ(rid, RidFor(7));
}

// Mid-scan, exactly one frame is pinned by the iterator (GetStats().pinned_frames == 1), and it
// is still one after crossing a leaf boundary — the old leaf was released, not leaked.
TEST_F(BPlusTreeTest, IteratorHoldsOneLeafAtATime) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());

	auto it = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(it.has_value()) << it.error().message();
	for (const index_key_t key : kReferenceKeys) {
		SCOPED_TRACE(testing::Message() << "at key " << key);
		ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, key));
		EXPECT_EQ(bpm_->GetStats().pinned_frames, 1u);
	}
}

// After a scan runs to its end (hi reached, or the chain ended), pinned_frames is 0 — the
// iterator released its guard on finishing, not only when destroyed.
TEST_F(BPlusTreeTest, ExhaustedIteratorHoldsNoPin) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // 3 5 7 | 10 12 15 | 20 25

	// Stopped by hi mid-leaf; by hi at a leaf's end; by the end of the chain.
	struct Case {
		index_key_t lo;
		index_key_t hi;
		std::vector<index_key_t> keys;
	};
	const Case cases[] = {
	    {3, 5, {3, 5}},
	    {10, 15, {10, 12, 15}},
	    {20, INT64_MAX, {20, 25}},
	};
	for (const Case& c : cases) {
		SCOPED_TRACE(testing::Message() << "[" << c.lo << ", " << c.hi << "]");
		auto it = tree_->Scan(c.lo, c.hi);
		ASSERT_TRUE(it.has_value()) << it.error().message();
		for (const index_key_t key : c.keys) ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, key));
		ASSERT_NO_FATAL_FAILURE(ExpectExhausted(*it));
		EXPECT_EQ(bpm_->GetStats().pinned_frames, 0u);
	}
}

// Move-construct from an iterator; the source's Next() is false, and the destination continues
// from exactly where the source was. On an empty tree; the next test is the mid-scan case.
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

// The mid-scan half of the above: moved after 3 and 5, the source is exhausted and pins
// nothing, and the destination picks up at 7 and runs to the end.
TEST_F(BPlusTreeTest, MovedFromIteratorIsExhaustedMidScan) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());

	auto source = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(source.has_value()) << source.error().message();
	ASSERT_NO_FATAL_FAILURE(ExpectNext(*source, 3));
	ASSERT_NO_FATAL_FAILURE(ExpectNext(*source, 5));

	IndexIterator dest(std::move(source.value()));
	EXPECT_EQ(bpm_->GetStats().pinned_frames, 1u);
	ASSERT_NO_FATAL_FAILURE(ExpectExhausted(*source));

	for (const index_key_t key : {7, 10, 12, 15, 20, 25}) {
		ASSERT_NO_FATAL_FAILURE(ExpectNext(dest, key));
	}
	ASSERT_NO_FATAL_FAILURE(ExpectExhausted(dest));
	EXPECT_EQ(bpm_->GetStats().pinned_frames, 0u);
}

// Move-assign a mid-scan iterator over another mid-scan iterator on a different leaf;
// pinned_frames drops by one — the target released its own guard before taking the source's.
TEST_F(BPlusTreeTest, MoveAssignmentReleasesTheTargetsPin) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // 3 5 7 | 10 12 15 | 20 25

	auto target = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(target.has_value()) << target.error().message();
	ASSERT_NO_FATAL_FAILURE(ExpectNext(*target, 3));  // first leaf
	auto source = tree_->Scan(20, INT64_MAX);
	ASSERT_TRUE(source.has_value()) << source.error().message();
	ASSERT_NO_FATAL_FAILURE(ExpectNext(*source, 20));  // last leaf
	ASSERT_EQ(bpm_->GetStats().pinned_frames, 2u);

	*target = std::move(*source);

	EXPECT_EQ(bpm_->GetStats().pinned_frames, 1u);
	ASSERT_NO_FATAL_FAILURE(ExpectNext(*target, 25));
	ASSERT_NO_FATAL_FAILURE(ExpectExhausted(*target));
	ASSERT_NO_FATAL_FAILURE(ExpectExhausted(*source));
	EXPECT_EQ(bpm_->GetStats().pinned_frames, 0u);
}

// Self-move-assignment leaves the iterator usable and its position unchanged.
TEST_F(BPlusTreeTest, SelfMoveAssignmentIsHarmless) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());

	auto it = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(it.has_value()) << it.error().message();
	ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, 3));

	// Through a reference, so the compiler's -Wself-move does not reject the line itself.
	IndexIterator& alias = *it;
	*it = std::move(alias);

	EXPECT_EQ(it->Key(), 3);
	EXPECT_EQ(bpm_->GetStats().pinned_frames, 1u);
	for (const index_key_t key : {5, 7, 10, 12, 15, 20, 25}) {
		ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, key));
	}
	ASSERT_NO_FATAL_FAILURE(ExpectExhausted(*it));
}

// Overwrite the second leaf's page type through a raw write guard, then scan across it: Next()
// returns the error, holds no pin, and every later Next() is false.
TEST_F(BPlusTreeTest, NextReportsACorruptLeafAndStaysExhausted) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());  // 3 5 7 | 10 12 15 | 20 25
	const auto leaves = LeafPages();
	ASSERT_EQ(leaves.size(), 3u);
	{
		auto guard = bpm_->FetchPageWrite(leaves[1]);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		guard->SetPageType(PageType::HEAP);
	}

	auto it = tree_->Scan(INT64_MIN, INT64_MAX);
	ASSERT_TRUE(it.has_value()) << it.error().message();
	for (const index_key_t key : {3, 5, 7}) ASSERT_NO_FATAL_FAILURE(ExpectNext(*it, key));

	auto more = it->Next();
	ASSERT_FALSE(more.has_value());
	EXPECT_EQ(more.error().code(), ErrorCode::kCorruption);
	EXPECT_EQ(bpm_->GetStats().pinned_frames, 0u);

	for (int i = 0; i < 2; i++) {
		SCOPED_TRACE(testing::Message() << "call " << i << " after the error");
		ASSERT_NO_FATAL_FAILURE(ExpectExhausted(*it));
	}
}

// Collect the keys from a scan, let the iterator die, then Insert/Delete each — completes
// rather than hanging. The documented pattern for "mutate what you scanned".
//
// Insert only until Delete is defined. A live iterator holds a read latch on its leaf, and an
// Insert on this same thread would wait for the write latch on it forever — so the pin check
// before mutating is what turns that regression into a failure instead of a hang.
TEST_F(BPlusTreeTest, CollectThenMutateDoesNotDeadlock) {
	ASSERT_NO_FATAL_FAILURE(BuildReferenceTree());

	std::vector<index_key_t> scanned;
	for (const auto& [key, rid] : CollectScan(INT64_MIN, INT64_MAX)) scanned.push_back(key);
	ASSERT_EQ(scanned, kReferenceKeys);
	ASSERT_EQ(bpm_->GetStats().pinned_frames, 0u) << "the scan still holds a leaf";

	// New keys, so leaves split while we mutate what we scanned.
	std::vector<index_key_t> added;
	for (const index_key_t key : scanned) added.push_back(key + 100);
	ASSERT_NO_FATAL_FAILURE(InsertAll(added));

	std::vector<index_key_t> all = kReferenceKeys;
	all.insert(all.end(), added.begin(), added.end());
	EXPECT_EQ(CollectScan(INT64_MIN, INT64_MAX), WithRids(all));
}

}  // namespace kernsql

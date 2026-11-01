#include "catalog/table.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "catalog/schema.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "heap/table_heap.hpp"
#include "indices/b_plus_tree.hpp"
#include "indices/b_plus_tree_page.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * Table against a real file and a real buffer pool: one heap, one B+tree, kept in step.
 *
 * The assertions to reach for:
 *
 *   1. CheckConsistency() after anything that mutates. It is the heap <-> index bijection plus the
 *      tree's own invariants, so a write that updated one side and not the other fails right
 *      there instead of as a wrong answer later.
 *   2. Whole rows, not statuses. Contents() decodes every heap row; comparing it to an expected
 *      map catches a row that came back with the right key and the wrong columns.
 *   3. Pool quiescence. TearDown's Shutdown() fails on a leaked pin — an iterator left alive, or
 *      an undo path that forgot to drop a guard.
 *
 * Drift between heap and index cannot be produced through Table, which is the point of it. The
 * tests that need drift open a SECOND handle on the same pages (RawHeap / RawIndex) and write
 * through that. Neither handle caches anything that matters single-threaded, so this is exactly
 * what a buggy Table would have done.
 *
 * The schema puts the primary key in the MIDDLE, so code that assumes column 0 is the key fails.
 */
class TableTest : public ::testing::Test {
  protected:
	// The B+tree suite's size: a few levels at MIN_FANOUT plus a scan fit; much more does not.
	static constexpr std::size_t kFrames = 16;
	static constexpr uint16_t kFanout = MIN_FANOUT;
	static constexpr table_id_t kTableId = 7;
	static constexpr index_key_t kMinKey = std::numeric_limits<index_key_t>::min();
	static constexpr index_key_t kMaxKey = std::numeric_limits<index_key_t>::max();

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_table_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
		         std::to_string(getpid()));
		std::filesystem::remove(path_);

		OpenPool();

		auto created = Table::Create(*bpm_, kTableId, "people", MakeSchema(), kFanout);
		ASSERT_TRUE(created.has_value()) << created.error().message();
		table_ = std::move(created.value());
	}

	void TearDown() override {
		// Tables first: Shutdown verifies quiescence rather than arranging it.
		table_.reset();
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

	// Close everything and reopen the table from the two page ids the catalog stores. The only
	// way to prove anything reached disk.
	void Reopen() {
		const page_id_t heap_first = table_->HeapFirstPageId();
		const page_id_t index_header = table_->IndexHeaderPageId();
		table_.reset();
		ASSERT_TRUE(bpm_->Shutdown().ok());
		bpm_.reset();
		dm_.reset();

		OpenPool();
		auto opened =
		    Table::Open(*bpm_, kTableId, "people", MakeSchema(), heap_first, index_header);
		ASSERT_TRUE(opened.has_value()) << opened.error().message();
		table_ = std::move(opened.value());
	}

	// (name VARCHAR(32), id INT64 PRIMARY KEY, score INT64)
	static Schema MakeSchema() {
		auto schema = Schema::Create({{"name", ColumnType::kVarchar, 32},
		                              {"id", ColumnType::kInt64, 0},
		                              {"score", ColumnType::kInt64, 0}},
		                             1);
		EXPECT_TRUE(schema.has_value()) << schema.error().message();
		return std::move(schema.value());
	}

	// Self-identifying: every column is derived from the key, so a row returned under the wrong
	// key is visible. `variant` changes the non-key columns for "same key, new row" tests.
	static Row MakeRow(index_key_t key, int variant = 0) {
		return Row{Value{std::format("n{}-{}", key % 100000, variant)}, Value{key},
		           Value{key / 2 + variant}};
	}

	void MustInsert(index_key_t key, int variant = 0) {
		const Status s = table_->Insert(MakeRow(key, variant));
		ASSERT_TRUE(s.ok()) << "key " << key << ": " << s.message();
	}

	void MustDelete(index_key_t key) {
		const Status s = table_->Delete(key);
		ASSERT_TRUE(s.ok()) << "key " << key << ": " << s.message();
	}

	// Every heap row, decoded, by key. Fails the test on a scan or decode error, or a key seen
	// twice, rather than returning something short.
	std::map<index_key_t, Row> Contents() {
		std::map<index_key_t, Row> rows;
		TableIterator it = table_->Scan();
		for (;;) {
			auto more = it.Next();
			if (!more.has_value()) {
				ADD_FAILURE() << "scan failed: " << more.error().message();
				return rows;
			}
			if (!more.value()) break;
			auto row = table_->GetSchema().Decode(it.Tuple());
			if (!row.has_value()) {
				ADD_FAILURE() << "decode failed: " << row.error().message();
				continue;
			}
			const index_key_t key = table_->GetSchema().KeyOf(row.value());
			if (!rows.emplace(key, std::move(row.value())).second) {
				ADD_FAILURE() << "key " << key << " twice in the heap";
			}
		}
		return rows;
	}

	// (key, RID) out of ScanIndex, in the order it yields them.
	std::vector<std::pair<index_key_t, RID>> IndexEntries(index_key_t lo = kMinKey,
	                                                      index_key_t hi = kMaxKey) {
		std::vector<std::pair<index_key_t, RID>> out;
		auto it = table_->ScanIndex(lo, hi);
		if (!it.has_value()) {
			ADD_FAILURE() << "ScanIndex failed: " << it.error().message();
			return out;
		}
		for (;;) {
			auto more = it->Next();
			if (!more.has_value()) {
				ADD_FAILURE() << "index scan failed: " << more.error().message();
				return out;
			}
			if (!more.value()) break;
			out.emplace_back(it->Key(), it->Rid());
		}
		return out;
	}

	static std::map<index_key_t, Row> Expected(const std::vector<index_key_t>& keys) {
		std::map<index_key_t, Row> out;
		for (const index_key_t k : keys) out.emplace(k, MakeRow(k));
		return out;
	}

	std::unique_ptr<TableHeap> RawHeap() {
		auto heap = TableHeap::Open(*bpm_, table_->HeapFirstPageId(), table_->HeapFirstPageId());
		EXPECT_TRUE(heap.has_value()) << heap.error().message();
		return heap.has_value() ? std::move(heap.value()) : nullptr;
	}

	std::unique_ptr<BPlusTree> RawIndex() {
		auto tree = BPlusTree::Open(*bpm_, table_->IndexHeaderPageId());
		EXPECT_TRUE(tree.has_value()) << tree.error().message();
		return tree.has_value() ? std::move(tree.value()) : nullptr;
	}

	RID RidOf(index_key_t key) {
		auto entries = IndexEntries(key, key);
		EXPECT_EQ(entries.size(), 1u) << "key " << key;
		return entries.empty() ? RID{} : entries.front().second;
	}

	std::vector<std::byte> PageBytes(page_id_t id) {
		auto guard = bpm_->FetchPageRead(id);
		EXPECT_TRUE(guard.has_value()) << guard.error().message();
		if (!guard.has_value()) return {};
		const auto body = guard->Body();
		return {body.begin(), body.end()};
	}

	// Pin all but `free` frames with fresh pages — never table pages, so nothing the table
	// fetches can wait on this thread.
	std::vector<WritePageGuard> PinAllBut(std::size_t free) {
		std::vector<WritePageGuard> pins;
		for (std::size_t i = 0; i + free < kFrames; ++i) {
			auto guard = bpm_->NewPage();
			EXPECT_TRUE(guard.has_value()) << guard.error().message();
			if (!guard.has_value()) break;
			pins.push_back(std::move(guard.value()));
		}
		return pins;
	}

	std::filesystem::path path_;
	std::unique_ptr<DiskManager> dm_;
	std::unique_ptr<BufferPoolManager> bpm_;
	std::unique_ptr<Table> table_;
};

// ---------------------------------------------------------------------------------------------
// Create / Open
// ---------------------------------------------------------------------------------------------

TEST_F(TableTest, CreateMakesAnEmptyConsistentTable) {
	EXPECT_EQ(table_->Id(), kTableId);
	EXPECT_EQ(table_->Name(), "people");
	EXPECT_EQ(table_->GetSchema().Columns().size(), 3u);
	EXPECT_EQ(table_->GetSchema().PrimaryKeyColumn(), 1);

	EXPECT_NE(table_->HeapFirstPageId(), INVALID_PAGE);
	EXPECT_NE(table_->IndexHeaderPageId(), INVALID_PAGE);
	EXPECT_NE(table_->HeapFirstPageId(), table_->IndexHeaderPageId());

	EXPECT_TRUE(Contents().empty());
	EXPECT_TRUE(IndexEntries().empty());
	auto row = table_->Get(1);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kNotFound);
	EXPECT_TRUE(table_->CheckConsistency());
}

// The tree rejects the fanout AFTER the heap has its page. Create frees that page, so the next
// allocation hands it straight back instead of extending the file.
TEST_F(TableTest, CreateWithABadFanoutFreesTheHeapPage) {
	const page_id_t pages = dm_->PageCount();

	auto bad = Table::Create(*bpm_, 8, "bad", MakeSchema(), MIN_FANOUT - 1);
	ASSERT_FALSE(bad.has_value());
	EXPECT_EQ(bad.error().code(), ErrorCode::kInvalidArgument);

	EXPECT_EQ(dm_->PageCount(), pages + 1);  // the heap page, and nothing for the tree
	auto probe = bpm_->NewPage();
	ASSERT_TRUE(probe.has_value()) << probe.error().message();
	EXPECT_EQ(probe->PageId(), pages);
}

// The tree runs out of frames halfway through its own Create. Whatever was allocated is freed:
// a retry with the pool free again reuses those pages instead of growing the file by all three.
TEST_F(TableTest, CreateThatCannotAllocateTheIndexLeaksNoPages) {
	// One free frame: the heap's first page takes it and releases it, the index header takes it
	// and holds it, and the root leaf finds none.
	{
		auto pins = PinAllBut(1);
		auto failed = Table::Create(*bpm_, 8, "t2", MakeSchema(), kFanout);
		ASSERT_FALSE(failed.has_value());
		EXPECT_EQ(failed.error().code(), ErrorCode::kBufferPoolFull);
	}
	const page_id_t pages = dm_->PageCount();

	auto retry = Table::Create(*bpm_, 8, "t2", MakeSchema(), kFanout);
	ASSERT_TRUE(retry.has_value()) << retry.error().message();
	EXPECT_LT(retry.value()->HeapFirstPageId(), pages);
	EXPECT_LT(retry.value()->IndexHeaderPageId(), pages);
	EXPECT_LE(dm_->PageCount(), pages + 1);  // at most the root leaf is new
	EXPECT_TRUE(retry.value()->CheckConsistency());
}

TEST_F(TableTest, OpenRejectsAnIndexHeaderThatIsNotOne) {
	auto opened = Table::Open(*bpm_, kTableId, "people", MakeSchema(), table_->HeapFirstPageId(),
	                          table_->HeapFirstPageId());
	ASSERT_FALSE(opened.has_value());
	EXPECT_EQ(opened.error().code(), ErrorCode::kCorruption);
}

TEST_F(TableTest, RowsSurviveAReopen) {
	for (index_key_t k = 1; k <= 40; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	for (index_key_t k = 1; k <= 40; k += 4) ASSERT_NO_FATAL_FAILURE(MustDelete(k));
	const auto before = Contents();
	const auto entries = IndexEntries();

	ASSERT_NO_FATAL_FAILURE(Reopen());

	EXPECT_EQ(table_->Id(), kTableId);
	EXPECT_EQ(Contents(), before);
	EXPECT_EQ(IndexEntries(), entries);
	EXPECT_TRUE(table_->CheckConsistency());
	for (const auto& [key, row] : before) {
		auto got = table_->Get(key);
		ASSERT_TRUE(got.has_value()) << "key " << key << ": " << got.error().message();
		EXPECT_EQ(got.value(), row);
	}
}

// Writes after a reopen: inserts (into a key range that splits the tree again), deletes, and
// re-inserting a deleted key, all on the reopened handles.
TEST_F(TableTest, WritesAfterAReopenKeepTheTableConsistent) {
	for (index_key_t k = 1; k <= 30; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	ASSERT_NO_FATAL_FAILURE(Reopen());

	for (index_key_t k = 31; k <= 60; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	for (index_key_t k = 2; k <= 60; k += 2) ASSERT_NO_FATAL_FAILURE(MustDelete(k));
	ASSERT_NO_FATAL_FAILURE(MustInsert(10, 1));
	EXPECT_TRUE(table_->CheckConsistency());

	ASSERT_NO_FATAL_FAILURE(Reopen());
	std::map<index_key_t, Row> expected;
	for (index_key_t k = 1; k <= 60; k += 2) expected.emplace(k, MakeRow(k));
	expected.emplace(10, MakeRow(10, 1));
	EXPECT_EQ(Contents(), expected);
	EXPECT_TRUE(table_->CheckConsistency());
}

// Only the heap's FIRST page is stored, so after a reopen the heap's last-page hint is the first
// page. Rows that fit nowhere must still extend the chain from its REAL end: extending from a
// stale page would overwrite that page's next link and cut every page after it out of the chain,
// so the rows inserted before the reopen would vanish from Scan while the index still named them.
TEST_F(TableTest, InsertAfterReopenAppendsAtTheRealEndOfAMultiPageHeap) {
	auto schema =
	    Schema::Create({{"id", ColumnType::kInt64, 0}, {"blob", ColumnType::kVarchar, 1500}}, 0);
	ASSERT_TRUE(schema.has_value()) << schema.error().message();
	auto created = Table::Create(*bpm_, 9, "blobs", schema.value(), kFanout);
	ASSERT_TRUE(created.has_value()) << created.error().message();
	table_ = std::move(created.value());

	// ~1.4 KB rows: two per page, so ten rows is a chain of five pages.
	auto blob_row = [](index_key_t k) {
		return Row{Value{k}, Value{std::string(1400, static_cast<char>('a' + k % 26))}};
	};
	for (index_key_t k = 1; k <= 10; ++k) ASSERT_TRUE(table_->Insert(blob_row(k)).ok());
	const RID last_rid = RidOf(10);
	ASSERT_NE(last_rid.page_id, table_->HeapFirstPageId()) << "the heap never grew a second page";

	const page_id_t heap_first = table_->HeapFirstPageId();
	const page_id_t index_header = table_->IndexHeaderPageId();
	table_.reset();
	ASSERT_TRUE(bpm_->Shutdown().ok());
	bpm_.reset();
	dm_.reset();
	OpenPool();
	auto opened = Table::Open(*bpm_, 9, "blobs", schema.value(), heap_first, index_header);
	ASSERT_TRUE(opened.has_value()) << opened.error().message();
	table_ = std::move(opened.value());

	const page_id_t pages = dm_->PageCount();
	for (index_key_t k = 11; k <= 20; ++k) ASSERT_TRUE(table_->Insert(blob_row(k)).ok());
	EXPECT_GT(dm_->PageCount(), pages) << "the inserts never extended the chain";

	std::map<index_key_t, Row> expected;
	for (index_key_t k = 1; k <= 20; ++k) expected.emplace(k, blob_row(k));
	EXPECT_EQ(Contents(), expected);
	EXPECT_TRUE(table_->CheckConsistency());
}
// ---------------------------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------------------------

TEST_F(TableTest, InsertThenGetReturnsTheSameRow) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(42));
	auto row = table_->Get(42);
	ASSERT_TRUE(row.has_value()) << row.error().message();
	EXPECT_EQ(row.value(), MakeRow(42));
	EXPECT_EQ(Contents(), Expected({42}));
	EXPECT_TRUE(table_->CheckConsistency());
}

// Empty and full-length strings, and keys at both ends of the domain and around zero.
TEST_F(TableTest, InsertAcceptsBoundaryValues) {
	const std::vector<Row> rows = {
	    Row{Value{std::string{}}, Value{kMinKey}, Value{kMaxKey}},
	    Row{Value{std::string(32, 'z')}, Value{kMaxKey}, Value{kMinKey}},
	    Row{Value{std::string("zero")}, Value{index_key_t{0}}, Value{int64_t{0}}},
	    Row{Value{std::string("minus")}, Value{index_key_t{-1}}, Value{int64_t{-1}}},
	};
	for (const Row& r : rows) {
		const Status s = table_->Insert(r);
		ASSERT_TRUE(s.ok()) << s.message();
	}
	for (const Row& r : rows) {
		const index_key_t key = std::get<int64_t>(r[1]);
		auto got = table_->Get(key);
		ASSERT_TRUE(got.has_value()) << "key " << key << ": " << got.error().message();
		EXPECT_EQ(got.value(), r);
	}
	const auto entries = IndexEntries();
	ASSERT_EQ(entries.size(), 4u);
	EXPECT_EQ(entries[0].first, kMinKey);
	EXPECT_EQ(entries[1].first, -1);
	EXPECT_EQ(entries[2].first, 0);
	EXPECT_EQ(entries[3].first, kMaxKey);
	EXPECT_TRUE(table_->CheckConsistency());
}

// Each kind of invalid row is kInvalidArgument, and the check comes before anything is written:
// the file does not grow, the table stays empty.
TEST_F(TableTest, InsertOfAnInvalidRowIsRejectedBeforeAnyWrite) {
	const std::vector<std::pair<const char*, Row>> bad = {
	    {"too few columns", Row{Value{std::string("a")}, Value{index_key_t{1}}}},
	    {"too many columns",
	     Row{Value{std::string("a")}, Value{index_key_t{1}}, Value{int64_t{1}}, Value{int64_t{1}}}},
	    {"string where INT64",
	     Row{Value{std::string("a")}, Value{std::string("1")}, Value{int64_t{1}}}},
	    {"INT64 where string", Row{Value{int64_t{5}}, Value{index_key_t{1}}, Value{int64_t{1}}}},
	    {"string too long",
	     Row{Value{std::string(33, 'x')}, Value{index_key_t{1}}, Value{int64_t{1}}}},
	    {"empty row", Row{}},
	};
	const page_id_t pages = dm_->PageCount();
	const auto heap_bytes = PageBytes(table_->HeapFirstPageId());
	for (const auto& [what, row] : bad) {
		SCOPED_TRACE(what);
		const Status s = table_->Insert(row);
		EXPECT_EQ(s.code(), ErrorCode::kInvalidArgument) << s.message();
	}
	EXPECT_EQ(dm_->PageCount(), pages);
	EXPECT_EQ(PageBytes(table_->HeapFirstPageId()), heap_bytes);
	EXPECT_TRUE(Contents().empty());
	EXPECT_TRUE(IndexEntries().empty());
	EXPECT_TRUE(table_->CheckConsistency());
}

// A duplicate is caught by the index probe BEFORE the heap is touched: the heap page is
// byte-for-byte unchanged (no row inserted and then deleted), and the original row is intact.
TEST_F(TableTest, InsertOfADuplicateKeyChangesNothing) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(5));
	const auto heap_bytes = PageBytes(table_->HeapFirstPageId());

	const Status s = table_->Insert(MakeRow(5, 1));
	EXPECT_EQ(s.code(), ErrorCode::kDuplicateKey) << s.message();

	EXPECT_EQ(PageBytes(table_->HeapFirstPageId()), heap_bytes);
	auto row = table_->Get(5);
	ASSERT_TRUE(row.has_value()) << row.error().message();
	EXPECT_EQ(row.value(), MakeRow(5));
	EXPECT_EQ(Contents(), Expected({5}));
	EXPECT_TRUE(table_->CheckConsistency());
}

// At MIN_FANOUT this grows the tree several levels and the heap over several pages, in an order
// that is neither ascending nor descending.
TEST_F(TableTest, ManyInsertsInMixedOrderStayConsistent) {
	std::vector<index_key_t> keys;
	for (index_key_t k = 0; k < 300; ++k) keys.push_back((k * 37) % 300 - 150);
	for (const index_key_t k : keys) ASSERT_NO_FATAL_FAILURE(MustInsert(k));

	EXPECT_EQ(Contents(), Expected(keys));
	const auto entries = IndexEntries();
	ASSERT_EQ(entries.size(), keys.size());
	for (std::size_t i = 0; i < entries.size(); ++i) {
		EXPECT_EQ(entries[i].first, static_cast<index_key_t>(i) - 150);
	}
	EXPECT_TRUE(table_->CheckConsistency());
}

// The default fanout too: the tree stays one leaf for a while, then splits.
TEST_F(TableTest, ManyInsertsAtTheDefaultFanoutStayConsistent) {
	auto created = Table::Create(*bpm_, 8, "wide", MakeSchema());
	ASSERT_TRUE(created.has_value()) << created.error().message();
	table_ = std::move(created.value());

	std::vector<index_key_t> keys;
	for (index_key_t k = 600; k > 0; --k) keys.push_back(k);
	for (const index_key_t k : keys) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	EXPECT_EQ(Contents(), Expected(keys));
	EXPECT_TRUE(table_->CheckConsistency());
}

// The tree Insert fails AFTER the heap Insert succeeded, so the heap row must be undone.
//
// The pool is squeezed from no free frames upward. With too few, the index probe or the heap
// fails first and nothing is written; with a few more, the probe and the heap get through and
// the tree's split cannot allocate — the undo path. Whichever happens, the table must come out
// exactly as it went in. The undo is detected by the heap page's bytes changing while its rows
// do not (a slot used and tombstoned), and the test requires that it was reached at least once.
TEST_F(TableTest, InsertWhoseIndexStepFailsUndoesTheHeapRow) {
	for (index_key_t k = 1; k <= 3; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));  // leaf full
	const auto before = Contents();

	bool reached_undo = false;
	bool inserted = false;
	for (std::size_t free = 0; free <= kFrames && !inserted; ++free) {
		SCOPED_TRACE(testing::Message() << free << " free frames");
		const auto heap_bytes = PageBytes(table_->HeapFirstPageId());
		Status s = Status::OK();
		{
			auto pins = PinAllBut(free);
			s = table_->Insert(MakeRow(4));
		}
		if (s.ok()) {
			inserted = true;
			break;
		}
		EXPECT_EQ(s.code(), ErrorCode::kBufferPoolFull) << s.message();
		if (PageBytes(table_->HeapFirstPageId()) != heap_bytes) reached_undo = true;

		EXPECT_EQ(Contents(), before);
		auto absent = table_->Get(4);
		ASSERT_FALSE(absent.has_value());
		EXPECT_EQ(absent.error().code(), ErrorCode::kNotFound);
		EXPECT_TRUE(table_->CheckConsistency());
	}

	EXPECT_TRUE(reached_undo) << "no frame count failed the tree step after the heap step";
	ASSERT_TRUE(inserted);
	EXPECT_EQ(Contents(), Expected({1, 2, 3, 4}));
	EXPECT_TRUE(table_->CheckConsistency());
}

// ---------------------------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------------------------

TEST_F(TableTest, DeleteRemovesTheRowFromBothSides) {
	for (index_key_t k = 1; k <= 5; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	const RID rid = RidOf(3);

	ASSERT_NO_FATAL_FAILURE(MustDelete(3));

	auto row = table_->Get(3);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kNotFound);
	auto at = table_->RowAt(rid);
	ASSERT_FALSE(at.has_value());
	EXPECT_EQ(at.error().code(), ErrorCode::kNotFound);
	EXPECT_EQ(Contents(), Expected({1, 2, 4, 5}));
	EXPECT_TRUE(IndexEntries(3, 3).empty());
	EXPECT_TRUE(table_->CheckConsistency());
}

TEST_F(TableTest, DeleteOfAnAbsentKeyIsNotFoundAndChangesNothing) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(1));
	const auto heap_bytes = PageBytes(table_->HeapFirstPageId());

	for (const index_key_t k : {index_key_t{0}, index_key_t{2}, kMinKey, kMaxKey}) {
		const Status s = table_->Delete(k);
		EXPECT_EQ(s.code(), ErrorCode::kNotFound) << "key " << k << ": " << s.message();
	}
	EXPECT_EQ(PageBytes(table_->HeapFirstPageId()), heap_bytes);
	EXPECT_EQ(Contents(), Expected({1}));

	ASSERT_NO_FATAL_FAILURE(MustDelete(1));
	const Status again = table_->Delete(1);
	EXPECT_EQ(again.code(), ErrorCode::kNotFound) << again.message();

	EXPECT_TRUE(Contents().empty());
	EXPECT_TRUE(table_->CheckConsistency());
}

TEST_F(TableTest, ADeletedKeyCanBeInsertedAgainWithNewContents) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(9));
	ASSERT_NO_FATAL_FAILURE(MustDelete(9));
	ASSERT_NO_FATAL_FAILURE(MustInsert(9, 3));

	auto row = table_->Get(9);
	ASSERT_TRUE(row.has_value()) << row.error().message();
	EXPECT_EQ(row.value(), MakeRow(9, 3));
	EXPECT_TRUE(table_->CheckConsistency());
}

// Grow several levels, then drain in an order unlike the insert order: every merge and root
// collapse in the tree, and every row gone from the heap.
TEST_F(TableTest, DeletingEverythingLeavesAnEmptyConsistentTable) {
	for (index_key_t k = 1; k <= 100; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	for (index_key_t i = 0; i < 100; ++i) {
		ASSERT_NO_FATAL_FAILURE(MustDelete((i * 41) % 100 + 1));
		if (i % 10 == 0) {
			ASSERT_TRUE(table_->CheckConsistency()) << "after " << i + 1;
		}
	}
	EXPECT_TRUE(Contents().empty());
	EXPECT_TRUE(IndexEntries().empty());
	EXPECT_TRUE(table_->CheckConsistency());

	ASSERT_NO_FATAL_FAILURE(MustInsert(50));
	EXPECT_EQ(Contents(), Expected({50}));
}

// The tree Delete is all or nothing: squeezed so that it cannot fetch the siblings a cascading
// merge needs, it fails and the row is still there, on both sides. (The shape is the B+tree
// suite's: 1..12 at MIN_FANOUT, minus 4 and 1, then deleting 2 cascades.)
TEST_F(TableTest, DeleteThatFailsInTheIndexLeavesTheRowWhole) {
	for (index_key_t k = 1; k <= 12; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	ASSERT_NO_FATAL_FAILURE(MustDelete(4));
	ASSERT_NO_FATAL_FAILURE(MustDelete(1));
	const auto before = Contents();

	bool deleted = false;
	for (std::size_t free = 0; free <= kFrames && !deleted; ++free) {
		SCOPED_TRACE(testing::Message() << free << " free frames");
		Status s = Status::OK();
		{
			auto pins = PinAllBut(free);
			s = table_->Delete(2);
		}
		if (s.ok()) {
			deleted = true;
			break;
		}
		EXPECT_EQ(s.code(), ErrorCode::kBufferPoolFull) << s.message();
		EXPECT_EQ(Contents(), before);
		auto row = table_->Get(2);
		ASSERT_TRUE(row.has_value()) << row.error().message();
		EXPECT_EQ(row.value(), MakeRow(2));
		EXPECT_TRUE(table_->CheckConsistency());
	}
	ASSERT_TRUE(deleted);
	EXPECT_EQ(Contents(), Expected({3, 5, 6, 7, 8, 9, 10, 11, 12}));
	EXPECT_TRUE(table_->CheckConsistency());
}

// The heap step fails after the tree step succeeded — the undo path. A single-user pool cannot
// fail it with an I/O error on demand, so the heap row is removed behind the table's back first:
// the index still names it, the heap Delete finds nothing, and the undo must put the index entry
// back. The table is then exactly as drifted as before the call, no more.
TEST_F(TableTest, DeleteWhoseHeapStepFailsPutsTheIndexEntryBack) {
	for (index_key_t k = 1; k <= 6; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	const RID rid = RidOf(4);
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		ASSERT_TRUE(heap->Delete(rid).ok());
	}

	const Status s = table_->Delete(4);
	EXPECT_FALSE(s.ok());

	// The index entry is back, with the same RID.
	EXPECT_EQ(RidOf(4), rid);
	auto row = table_->Get(4);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kCorruption);
	EXPECT_FALSE(table_->CheckConsistency());

	// Everything else untouched.
	EXPECT_EQ(Contents(), Expected({1, 2, 3, 5, 6}));
	for (const index_key_t k : {1, 2, 3, 5, 6}) {
		auto other = table_->Get(k);
		ASSERT_TRUE(other.has_value()) << "key " << k << ": " << other.error().message();
	}
}

// ---------------------------------------------------------------------------------------------
// Get / RowAt / Scan / ScanIndex
// ---------------------------------------------------------------------------------------------

TEST_F(TableTest, GetOfAnAbsentKeyIsNotFound) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(10));
	for (const index_key_t k : {index_key_t{9}, index_key_t{11}, kMinKey, kMaxKey}) {
		auto row = table_->Get(k);
		ASSERT_FALSE(row.has_value()) << "key " << k;
		EXPECT_EQ(row.error().code(), ErrorCode::kNotFound) << "key " << k;
	}
}

// The index names a RID whose heap slot is empty: that is drift, and it must say so rather than
// pass for an absent row.
TEST_F(TableTest, GetWhenTheIndexOutlivesTheHeapRowIsCorruption) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(1));
	ASSERT_NO_FATAL_FAILURE(MustInsert(2));
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		ASSERT_TRUE(heap->Delete(RidOf(2)).ok());
	}
	auto row = table_->Get(2);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kCorruption) << row.error().message();

	auto other = table_->Get(1);
	ASSERT_TRUE(other.has_value()) << other.error().message();
}

TEST_F(TableTest, RowAtDecodesTheRowAtEachIndexRid) {
	for (index_key_t k = 1; k <= 20; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	for (const auto& [key, rid] : IndexEntries()) {
		auto row = table_->RowAt(rid);
		ASSERT_TRUE(row.has_value()) << "key " << key << ": " << row.error().message();
		EXPECT_EQ(row.value(), MakeRow(key));
	}
}

TEST_F(TableTest, RowAtOfASlotThatNeverExistedIsNotFound) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(1));
	const RID rid = RidOf(1);
	auto row = table_->RowAt(RID{rid.page_id, static_cast<slot_id_t>(rid.slot + 50)});
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kNotFound);
}

// Raw bytes on the heap that the schema cannot decode: RowAt surfaces Decode's kCorruption.
TEST_F(TableTest, RowAtOfUndecodableBytesIsCorruption) {
	RID rid;
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		const std::byte junk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
		auto inserted = heap->Insert(junk);
		ASSERT_TRUE(inserted.has_value()) << inserted.error().message();
		rid = inserted.value();
	}
	auto row = table_->RowAt(rid);
	ASSERT_FALSE(row.has_value());
	EXPECT_EQ(row.error().code(), ErrorCode::kCorruption);
}

// Scan yields every row exactly once, in heap order, as bytes that decode to the inserted rows,
// with RIDs matching the index's.
TEST_F(TableTest, ScanYieldsEveryRowWithTheIndexRid) {
	for (index_key_t k = 1; k <= 50; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	ASSERT_NO_FATAL_FAILURE(MustDelete(25));

	std::map<index_key_t, RID> index_rids;
	for (const auto& [key, rid] : IndexEntries()) index_rids.emplace(key, rid);

	std::size_t seen = 0;
	TableIterator it = table_->Scan();
	for (;;) {
		auto more = it.Next();
		ASSERT_TRUE(more.has_value()) << more.error().message();
		if (!more.value()) break;
		auto row = table_->GetSchema().Decode(it.Tuple());
		ASSERT_TRUE(row.has_value()) << row.error().message();
		const index_key_t key = table_->GetSchema().KeyOf(row.value());
		EXPECT_EQ(row.value(), MakeRow(key));
		ASSERT_TRUE(index_rids.contains(key)) << "key " << key;
		EXPECT_EQ(it.Rid(), index_rids[key]) << "key " << key;
		++seen;
	}
	EXPECT_EQ(seen, 49u);
}

TEST_F(TableTest, ScanIndexYieldsExactlyTheKeysInRangeInOrder) {
	for (index_key_t k = 2; k <= 40; k += 2) ASSERT_NO_FATAL_FAILURE(MustInsert(k));

	auto keys_of = [](const std::vector<std::pair<index_key_t, RID>>& entries) {
		std::vector<index_key_t> out;
		for (const auto& e : entries) out.push_back(e.first);
		return out;
	};
	EXPECT_EQ(keys_of(IndexEntries(10, 16)), (std::vector<index_key_t>{10, 12, 14, 16}));
	EXPECT_EQ(keys_of(IndexEntries(9, 17)), (std::vector<index_key_t>{10, 12, 14, 16}));
	EXPECT_EQ(keys_of(IndexEntries(11, 11)), std::vector<index_key_t>{});
	EXPECT_EQ(keys_of(IndexEntries(40, kMaxKey)), std::vector<index_key_t>{40});
	EXPECT_EQ(keys_of(IndexEntries(kMinKey, 2)), std::vector<index_key_t>{2});
	EXPECT_TRUE(IndexEntries(30, 20).empty());
	EXPECT_EQ(IndexEntries().size(), 20u);
}

// ---------------------------------------------------------------------------------------------
// CheckConsistency — each kind of drift, produced behind the table's back.
// ---------------------------------------------------------------------------------------------

// A heap row the index does not know: the orphan a failed Insert undo would leave.
TEST_F(TableTest, CheckConsistencyCatchesAHeapRowMissingFromTheIndex) {
	for (index_key_t k = 1; k <= 5; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	ASSERT_TRUE(table_->CheckConsistency());
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		auto bytes = table_->GetSchema().Encode(MakeRow(99));
		ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
		ASSERT_TRUE(heap->Insert(bytes.value()).has_value());
	}
	EXPECT_FALSE(table_->CheckConsistency());
}

// An index entry whose heap row is gone.
TEST_F(TableTest, CheckConsistencyCatchesAnIndexEntryWithNoHeapRow) {
	for (index_key_t k = 1; k <= 5; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		ASSERT_TRUE(heap->Delete(RidOf(3)).ok());
	}
	EXPECT_FALSE(table_->CheckConsistency());
}

// Same keys and same counts on both sides, but one index entry names another row's RID. Only a
// RID-for-RID comparison catches this.
TEST_F(TableTest, CheckConsistencyCatchesAnIndexEntryWithTheWrongRid) {
	for (index_key_t k = 1; k <= 5; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	const RID other = RidOf(4);
	{
		auto tree = RawIndex();
		ASSERT_NE(tree, nullptr);
		ASSERT_TRUE(tree->Delete(2).ok());
		ASSERT_TRUE(tree->Insert(2, other).ok());
	}
	EXPECT_FALSE(table_->CheckConsistency());
}

// Two heap rows with the same key, one of them indexed: counts differ and a key repeats.
TEST_F(TableTest, CheckConsistencyCatchesADuplicateKeyInTheHeap) {
	for (index_key_t k = 1; k <= 5; ++k) ASSERT_NO_FATAL_FAILURE(MustInsert(k));
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		auto bytes = table_->GetSchema().Encode(MakeRow(3, 1));
		ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
		ASSERT_TRUE(heap->Insert(bytes.value()).has_value());
	}
	EXPECT_FALSE(table_->CheckConsistency());
}

TEST_F(TableTest, CheckConsistencyCatchesAnUndecodableHeapRow) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(1));
	{
		auto heap = RawHeap();
		ASSERT_NE(heap, nullptr);
		const std::byte junk[1] = {std::byte{0xff}};
		ASSERT_TRUE(heap->Insert(junk).has_value());
	}
	EXPECT_FALSE(table_->CheckConsistency());
}

// The bijection holds but the tree itself is broken: CheckConsistency defers to its
// CheckInvariants.
TEST_F(TableTest, CheckConsistencyCatchesAnUnsortedLeafInTheTree) {
	ASSERT_NO_FATAL_FAILURE(MustInsert(1));
	ASSERT_NO_FATAL_FAILURE(MustInsert(2));
	ASSERT_TRUE(table_->CheckConsistency());

	// Swap the two entries in the root leaf, RIDs and all: every (key, RID) pair still exists,
	// but the leaf is unsorted.
	page_id_t root{INVALID_PAGE};
	{
		auto guard = bpm_->FetchPageRead(table_->IndexHeaderPageId());
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		root = IndexHeader::ReadFrom(guard->Body()).root_page_id;
	}
	{
		auto guard = bpm_->FetchPageWrite(root);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		auto leaf = AsLeaf(guard.value());
		ASSERT_TRUE(leaf.has_value()) << leaf.error().message();
		ASSERT_EQ(leaf->Count(), 2);
		const LeafEntry first = leaf->EntryAt(0);
		leaf->RemoveAt(0);
		leaf->InsertAt(1, first);
	}
	EXPECT_FALSE(table_->CheckConsistency());
}

// ---------------------------------------------------------------------------------------------
// Differential: a long random workload against std::map.
// ---------------------------------------------------------------------------------------------

// Inserts (some duplicates), deletes (some absent), gets, over a small key range so every key is
// hit many times. Every answer is checked against the model as it happens, the whole table
// against the model periodically, and once more across a reopen in the middle and at the end.
TEST_F(TableTest, RandomWorkloadMatchesAStdMap) {
	std::mt19937_64 rng(0x6b65726e);  // fixed: a failure must reproduce
	std::uniform_int_distribution<index_key_t> key_dist(-200, 200);
	std::uniform_int_distribution<int> op_dist(0, 9);
	std::uniform_int_distribution<int> variant_dist(0, 1000);

	std::map<index_key_t, Row> model;
	constexpr int kOps = 4000;
	for (int i = 1; i <= kOps; ++i) {
		const index_key_t key = key_dist(rng);
		const int op = op_dist(rng);
		if (op < 5) {
			const Row row = MakeRow(key, variant_dist(rng));
			const Status s = table_->Insert(row);
			if (model.contains(key)) {
				ASSERT_EQ(s.code(), ErrorCode::kDuplicateKey) << "op " << i << " key " << key;
			} else {
				ASSERT_TRUE(s.ok()) << "op " << i << " key " << key << ": " << s.message();
				model.emplace(key, row);
			}
		} else if (op < 8) {
			const Status s = table_->Delete(key);
			if (model.erase(key) == 1) {
				ASSERT_TRUE(s.ok()) << "op " << i << " key " << key << ": " << s.message();
			} else {
				ASSERT_EQ(s.code(), ErrorCode::kNotFound) << "op " << i << " key " << key;
			}
		} else {
			auto row = table_->Get(key);
			if (auto found = model.find(key); found != model.end()) {
				ASSERT_TRUE(row.has_value()) << "op " << i << " key " << key;
				ASSERT_EQ(row.value(), found->second) << "op " << i << " key " << key;
			} else {
				ASSERT_FALSE(row.has_value()) << "op " << i << " key " << key;
				ASSERT_EQ(row.error().code(), ErrorCode::kNotFound);
			}
		}

		if (i % 500 == 0) {
			ASSERT_TRUE(table_->CheckConsistency()) << "after op " << i;
			ASSERT_EQ(Contents(), model) << "after op " << i;
		}
		if (i == kOps / 2) {
			ASSERT_NO_FATAL_FAILURE(Reopen());
		}
	}

	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(Contents(), model);
	EXPECT_TRUE(table_->CheckConsistency());
}

}  // namespace kernsql

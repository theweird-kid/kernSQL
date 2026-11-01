#include "catalog/catalog.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "catalog/database.hpp"
#include "catalog/schema.hpp"
#include "catalog/table.hpp"
#include "common/page_header.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * Catalog and Database, against a real file. Everything goes through Database, as every caller
 * above storage will.
 *
 * The assertions to reach for:
 *
 *   1. Reopen. The catalog's whole job is to bring tables back, so almost every test closes the
 *      file and opens it again before it believes anything.
 *   2. Page 1's bytes. A rejected CREATE TABLE must leave them exactly as they were, and the
 *      format test pins them byte for byte — a format change has to change that test on purpose.
 *   3. kCorruption from Open for every field that can be wrong on disk. Each corruption test
 *      patches one field of an otherwise valid catalog, so it fails for that reason only.
 */
class CatalogTest : public ::testing::Test {
  protected:
	static constexpr std::size_t kFrames = 16;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
		        (std::string("kernsql_catalog_test_") +
		         testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
		         std::to_string(getpid()));
		std::filesystem::remove(path_);
		ASSERT_NO_FATAL_FAILURE(OpenDb());
	}

	void TearDown() override {
		if (db_) {
			EXPECT_TRUE(db_->Shutdown().ok());
		}
		db_.reset();
		std::filesystem::remove(path_);
	}

	void OpenDb() {
		auto db = Database::Open(path_, kFrames);
		ASSERT_TRUE(db.has_value()) << db.error().message();
		db_ = std::move(db.value());
	}

	void CloseDb() {
		ASSERT_TRUE(db_->Shutdown().ok());
		db_.reset();
	}

	void Reopen() {
		ASSERT_NO_FATAL_FAILURE(CloseDb());
		ASSERT_NO_FATAL_FAILURE(OpenDb());
	}

	// Close, reopen, and expect Open to refuse the file with kCorruption.
	void ExpectReopenIsCorruption() {
		ASSERT_NO_FATAL_FAILURE(CloseDb());
		auto db = Database::Open(path_, kFrames);
		ASSERT_FALSE(db.has_value()) << "Open accepted a corrupt catalog";
		EXPECT_EQ(db.error().code(), ErrorCode::kCorruption) << db.error().message();
	}

	Catalog& Cat() { return db_->GetCatalog(); }
	BufferPoolManager& Pool() { return db_->Pool(); }

	// (id INT64 PRIMARY KEY, s VARCHAR(10))
	static Schema SmallSchema() {
		auto schema =
		    Schema::Create({{"id", ColumnType::kInt64, 0}, {"s", ColumnType::kVarchar, 10}}, 0);
		EXPECT_TRUE(schema.has_value()) << schema.error().message();
		return std::move(schema.value());
	}

	// (label VARCHAR(20), qty INT64, key INT64 PRIMARY KEY, note VARCHAR(100))
	static Schema WideSchema() {
		auto schema = Schema::Create({{"label", ColumnType::kVarchar, 20},
		                              {"qty", ColumnType::kInt64, 0},
		                              {"key", ColumnType::kInt64, 0},
		                              {"note", ColumnType::kVarchar, 100}},
		                             2);
		EXPECT_TRUE(schema.has_value()) << schema.error().message();
		return std::move(schema.value());
	}

	Table* MustCreate(const std::string& name, Schema schema = SmallSchema(),
	                  uint16_t fanout = NODE_CAPACITY) {
		auto table = Cat().CreateTable(name, std::move(schema), fanout);
		EXPECT_TRUE(table.has_value()) << name << ": " << table.error().message();
		return table.value_or(nullptr);
	}

	Table* MustGet(const std::string& name) {
		auto table = Cat().GetTable(name);
		EXPECT_TRUE(table.has_value()) << name << ": " << table.error().message();
		return table.value_or(nullptr);
	}

	std::vector<std::byte> PageBody(page_id_t id) {
		auto guard = Pool().FetchPageRead(id);
		EXPECT_TRUE(guard.has_value()) << guard.error().message();
		if (!guard.has_value()) return {};
		const auto body = guard->Body();
		return {body.begin(), body.end()};
	}

	PageHeader PageHeaderOf(page_id_t id) {
		auto guard = Pool().FetchPageRead(id);
		EXPECT_TRUE(guard.has_value()) << guard.error().message();
		return guard.has_value() ? guard->Header() : PageHeader{};
	}

	CatalogHeader ReadCatalogHeader() {
		auto guard = Pool().FetchPageRead(CATALOG_ROOT_PAGE_ID);
		EXPECT_TRUE(guard.has_value()) << guard.error().message();
		if (!guard.has_value()) return {};
		return CatalogHeader::ReadFrom(guard->Body().first<CATALOG_HEADER_SIZE>());
	}

	// Edit page 1 in place, bypassing the catalog. ONLY for the corruption tests.
	void PatchPage(page_id_t id, const std::function<void(WritePageGuard&)>& edit) {
		auto guard = Pool().FetchPageWrite(id);
		ASSERT_TRUE(guard.has_value()) << guard.error().message();
		edit(guard.value());
	}

	template <typename T>
	void PatchField(std::size_t offset, T value) {
		ASSERT_NO_FATAL_FAILURE(PatchPage(CATALOG_ROOT_PAGE_ID, [&](WritePageGuard& g) {
			std::memcpy(g.MutableBody().data() + offset, &value, sizeof(T));
		}));
	}

	void PatchHeader(const std::function<void(CatalogHeader&)>& edit) {
		ASSERT_NO_FATAL_FAILURE(PatchPage(CATALOG_ROOT_PAGE_ID, [&](WritePageGuard& g) {
			auto header = CatalogHeader::ReadFrom(g.Body().first<CATALOG_HEADER_SIZE>());
			edit(header);
			header.WriteTo(g.MutableBody().first<CATALOG_HEADER_SIZE>());
		}));
	}

	// Remember page 1's body, so a corruption can be undone with OpenRawAndRestore.
	void Snapshot() { snapshot_ = PageBody(CATALOG_ROOT_PAGE_ID); }

	// With the database closed: open the file WITHOUT a catalog (which would refuse it), put
	// page 1's body back as Snapshot() saw it, and close again.
	void OpenRawAndRestore() {
		ASSERT_EQ(db_, nullptr);
		ASSERT_EQ(snapshot_.size(), PAGE_BODY_SIZE);
		auto dm = DiskManager::Open(path_);
		ASSERT_TRUE(dm.has_value()) << dm.error().message();
		BufferPoolManager bpm(*dm.value(), 4);
		{
			auto guard = bpm.FetchPageWrite(CATALOG_ROOT_PAGE_ID);
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			std::memcpy(guard->MutableBody().data(), snapshot_.data(), PAGE_BODY_SIZE);
		}
		ASSERT_TRUE(bpm.Shutdown().ok());
	}

	// A fresh page typed `type`, for building chains by hand.
	page_id_t NewTypedPage(PageType type) {
		auto guard = Pool().NewPage();
		EXPECT_TRUE(guard.has_value()) << guard.error().message();
		if (!guard.has_value()) return INVALID_PAGE;
		guard->SetPageType(type);
		return guard->PageId();
	}

	// Pin all but `free` frames with fresh pages, never catalog or table pages.
	std::vector<WritePageGuard> PinAllBut(std::size_t free) {
		std::vector<WritePageGuard> pins;
		for (std::size_t i = 0; i + free < kFrames; ++i) {
			auto guard = Pool().NewPage();
			EXPECT_TRUE(guard.has_value()) << guard.error().message();
			if (!guard.has_value()) break;
			pins.push_back(std::move(guard.value()));
		}
		return pins;
	}

	// Create one-column tables t0000, t0001, ... until one more would not fit on page 1. Each is
	// 27 bytes of stream: 17 fixed + 5 name + one column (4 fixed + 2 name).
	void FillFirstCatalogPage() {
		auto schema = Schema::Create({{"id", ColumnType::kInt64, 0}}, 0);
		ASSERT_TRUE(schema.has_value());
		constexpr std::size_t kEntry = 27;
		int i = 0;
		while (ReadCatalogHeader().stream_length + kEntry <= PAGE_BODY_SIZE) {
			ASSERT_NE(MustCreate(std::format("t{:04d}", i++), schema.value()), nullptr);
		}
		ASSERT_EQ(PageHeaderOf(CATALOG_ROOT_PAGE_ID).next_page_id, INVALID_PAGE);
	}

	// Layout of a catalog holding exactly one table "t" with SmallSchema (see FormatIsPinned).
	static constexpr std::size_t kOffTableId = 16;
	static constexpr std::size_t kOffNameLen = 20;
	static constexpr std::size_t kOffName = 21;
	static constexpr std::size_t kOffHeapFirst = 22;
	static constexpr std::size_t kOffIndexHeader = 26;
	static constexpr std::size_t kOffPrimaryKey = 30;
	static constexpr std::size_t kOffColumnCount = 32;
	static constexpr std::size_t kOffCol1Type = 42;
	static constexpr std::size_t kStreamLength = 45;

	std::filesystem::path path_;
	std::unique_ptr<Database> db_;
	std::vector<std::byte> snapshot_;
};

// ---------------------------------------------------------------------------------------------
// Empty catalog
// ---------------------------------------------------------------------------------------------

TEST_F(CatalogTest, ANewFileHasAnEmptyCatalog) {
	EXPECT_EQ(Cat().TableCount(), 0u);
	EXPECT_TRUE(Cat().TableNames().empty());
	auto missing = Cat().GetTable("anything");
	ASSERT_FALSE(missing.has_value());
	EXPECT_EQ(missing.error().code(), ErrorCode::kNotFound);

	// Nothing is written until the first CREATE TABLE: page 1 is still all zero.
	EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), std::vector<std::byte>(PAGE_BODY_SIZE));
	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(Cat().TableCount(), 0u);
}

// ---------------------------------------------------------------------------------------------
// CreateTable / GetTable
// ---------------------------------------------------------------------------------------------

TEST_F(CatalogTest, CreateTableReturnsTheTableGetTableFinds) {
	Table* created = MustCreate("users", WideSchema());
	ASSERT_NE(created, nullptr);
	EXPECT_EQ(created->Name(), "users");
	EXPECT_EQ(created->GetSchema(), WideSchema());

	EXPECT_EQ(MustGet("users"), created);
	EXPECT_EQ(Cat().TableCount(), 1u);
	EXPECT_EQ(Cat().TableNames(), std::vector<std::string>{"users"});
}

TEST_F(CatalogTest, GetTableIsAnExactMatch) {
	ASSERT_NE(MustCreate("users"), nullptr);
	for (const char* name : {"Users", "USERS", "user", "users_", " users", ""}) {
		auto table = Cat().GetTable(name);
		ASSERT_FALSE(table.has_value()) << "'" << name << "'";
		EXPECT_EQ(table.error().code(), ErrorCode::kNotFound);
	}
}

TEST_F(CatalogTest, TableIdsAreAssignedInCreationOrder) {
	EXPECT_EQ(MustCreate("c")->Id(), 0u);
	EXPECT_EQ(MustCreate("a")->Id(), 1u);
	EXPECT_EQ(MustCreate("b")->Id(), 2u);
}

TEST_F(CatalogTest, TableNamesAreSorted) {
	for (const char* name : {"zeta", "alpha", "mid", "_under", "alpha2"}) {
		ASSERT_NE(MustCreate(name), nullptr);
	}
	EXPECT_EQ(Cat().TableNames(),
	          (std::vector<std::string>{"_under", "alpha", "alpha2", "mid", "zeta"}));
}

TEST_F(CatalogTest, CreateTableRejectsAnInvalidNameBeforeWritingAnything) {
	ASSERT_NE(MustCreate("existing"), nullptr);
	const auto page1 = PageBody(CATALOG_ROOT_PAGE_ID);
	const page_id_t pages = static_cast<page_id_t>(std::filesystem::file_size(path_) / PAGE_SIZE);

	for (const std::string& name :
	     {std::string{}, std::string("Users"), std::string("1abc"), std::string("a b"),
	      std::string("a-b"), std::string(MAX_NAME_LENGTH + 1, 'a')}) {
		auto table = Cat().CreateTable(name, SmallSchema());
		ASSERT_FALSE(table.has_value()) << "'" << name << "'";
		EXPECT_EQ(table.error().code(), ErrorCode::kInvalidArgument) << "'" << name << "'";
	}

	EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), page1);
	EXPECT_EQ(Cat().TableCount(), 1u);
	ASSERT_NO_FATAL_FAILURE(CloseDb());
	EXPECT_EQ(static_cast<page_id_t>(std::filesystem::file_size(path_) / PAGE_SIZE), pages);
	ASSERT_NO_FATAL_FAILURE(OpenDb());
}

TEST_F(CatalogTest, CreateTableAcceptsANameOfMaximumLength) {
	const std::string name(MAX_NAME_LENGTH, 'n');
	ASSERT_NE(MustCreate(name), nullptr);
	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_NE(MustGet(name), nullptr);
}

TEST_F(CatalogTest, CreateTableRejectsADuplicateNameAndChangesNothing) {
	Table* first = MustCreate("users");
	ASSERT_NE(first, nullptr);
	const auto page1 = PageBody(CATALOG_ROOT_PAGE_ID);

	auto again = Cat().CreateTable("users", WideSchema());
	ASSERT_FALSE(again.has_value());
	EXPECT_EQ(again.error().code(), ErrorCode::kDuplicateKey);

	EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), page1);
	EXPECT_EQ(MustGet("users"), first);
	EXPECT_EQ(first->GetSchema(), SmallSchema());
}

// Table::Create fails after the catalog has fetched its pages (a fanout the tree rejects). The
// catalog is unchanged in memory and on disk, and the next table gets the id the failed one
// would have had — an id is spent only when a table is actually created.
TEST_F(CatalogTest, CreateTableWhoseTableCannotBeCreatedChangesNothing) {
	ASSERT_NE(MustCreate("a"), nullptr);
	const auto page1 = PageBody(CATALOG_ROOT_PAGE_ID);

	auto bad = Cat().CreateTable("b", SmallSchema(), MIN_FANOUT - 1);
	ASSERT_FALSE(bad.has_value());
	EXPECT_EQ(bad.error().code(), ErrorCode::kInvalidArgument);

	EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), page1);
	EXPECT_EQ(Cat().TableCount(), 1u);
	EXPECT_FALSE(Cat().GetTable("b").has_value());
	EXPECT_EQ(MustCreate("b")->Id(), 1u);
}

// As above, when the new stream needs a second catalog page: that page was allocated before
// Table::Create failed, so it must be freed, not linked and not leaked. Both it and the heap page
// Table::Create freed come back from the next two allocations, and the file does not grow.
TEST_F(CatalogTest, CreateTableThatFailsFreesTheNewCatalogPage) {
	ASSERT_NO_FATAL_FAILURE(FillFirstCatalogPage());
	const auto page1 = PageBody(CATALOG_ROOT_PAGE_ID);
	const std::size_t count = Cat().TableCount();

	auto bad = Cat().CreateTable("overflow", SmallSchema(), MIN_FANOUT - 1);
	ASSERT_FALSE(bad.has_value());
	EXPECT_EQ(bad.error().code(), ErrorCode::kInvalidArgument);

	EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), page1);
	EXPECT_EQ(PageHeaderOf(CATALOG_ROOT_PAGE_ID).next_page_id, INVALID_PAGE);
	EXPECT_EQ(Cat().TableCount(), count);

	// The two freed pages are the next two handed out; a leaked one would make the second
	// allocation extend the file.
	std::set<page_id_t> reused;
	ASSERT_NO_FATAL_FAILURE(CloseDb());
	const auto size_before = std::filesystem::file_size(path_);
	ASSERT_NO_FATAL_FAILURE(OpenDb());
	reused.insert(NewTypedPage(PageType::HEAP));
	reused.insert(NewTypedPage(PageType::HEAP));
	EXPECT_EQ(reused.size(), 2u);
	ASSERT_NO_FATAL_FAILURE(CloseDb());
	EXPECT_EQ(std::filesystem::file_size(path_), size_before);
	ASSERT_NO_FATAL_FAILURE(OpenDb());
}

// The pool is squeezed from no free frames upward around a CREATE TABLE that also needs a new
// catalog page. Every failure leaves the catalog exactly as it was — same tables, page 1's bytes
// and link unchanged — and once there are enough frames it succeeds and survives a reopen.
TEST_F(CatalogTest, CreateTableUnderPoolPressureIsAllOrNothing) {
	ASSERT_NO_FATAL_FAILURE(FillFirstCatalogPage());
	const auto page1 = PageBody(CATALOG_ROOT_PAGE_ID);
	const std::size_t count = Cat().TableCount();

	bool created = false;
	int failures = 0;
	for (std::size_t free = 0; free <= kFrames && !created; ++free) {
		SCOPED_TRACE(testing::Message() << free << " free frames");
		std::optional<Result<Table*>> result;
		{
			auto pins = PinAllBut(free);
			result.emplace(Cat().CreateTable("overflow", SmallSchema()));
		}
		if (result->has_value()) {
			created = true;
			break;
		}
		++failures;
		EXPECT_EQ(result->error().code(), ErrorCode::kBufferPoolFull) << result->error().message();
		EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), page1);
		EXPECT_EQ(PageHeaderOf(CATALOG_ROOT_PAGE_ID).next_page_id, INVALID_PAGE);
		EXPECT_EQ(Cat().TableCount(), count);
		EXPECT_FALSE(Cat().GetTable("overflow").has_value());
	}
	ASSERT_TRUE(created);
	EXPECT_GT(failures, 2) << "the squeeze never reached the later steps";

	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(Cat().TableCount(), count + 1);
	EXPECT_NE(MustGet("overflow"), nullptr);
}

// ---------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------

TEST_F(CatalogTest, TablesAndTheirRowsSurviveAReopen) {
	Table* users = MustCreate("users", SmallSchema(), MIN_FANOUT);
	Table* items = MustCreate("items", WideSchema());
	ASSERT_NE(users, nullptr);
	ASSERT_NE(items, nullptr);
	const table_id_t users_id = users->Id();
	const table_id_t items_id = items->Id();
	for (int64_t k = 0; k < 50; ++k) {
		ASSERT_TRUE(users->Insert(Row{Value{k}, Value{std::format("u{}", k)}}).ok());
		ASSERT_TRUE(items
		                ->Insert(Row{Value{std::format("l{}", k)}, Value{k * 2}, Value{-k},
		                             Value{std::string(static_cast<std::size_t>(k), 'n')}})
		                .ok());
	}

	ASSERT_NO_FATAL_FAILURE(Reopen());

	EXPECT_EQ(Cat().TableNames(), (std::vector<std::string>{"items", "users"}));
	users = MustGet("users");
	items = MustGet("items");
	ASSERT_NE(users, nullptr);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(users->Id(), users_id);
	EXPECT_EQ(items->Id(), items_id);
	EXPECT_EQ(users->GetSchema(), SmallSchema());
	EXPECT_EQ(items->GetSchema(), WideSchema());
	for (int64_t k = 0; k < 50; ++k) {
		auto u = users->Get(k);
		ASSERT_TRUE(u.has_value()) << u.error().message();
		EXPECT_EQ(u.value(), (Row{Value{k}, Value{std::format("u{}", k)}}));
		auto i = items->Get(-k);
		ASSERT_TRUE(i.has_value()) << i.error().message();
		EXPECT_EQ(std::get<int64_t>(i.value()[1]), k * 2);
	}
	EXPECT_TRUE(users->CheckConsistency());
	EXPECT_TRUE(items->CheckConsistency());

	// And they take writes after the reopen.
	ASSERT_TRUE(users->Insert(Row{Value{int64_t{100}}, Value{std::string("new")}}).ok());
	ASSERT_TRUE(users->Delete(0).ok());
	EXPECT_TRUE(users->CheckConsistency());
}

// next_table_id is persisted, not recomputed from the largest id present.
TEST_F(CatalogTest, TableIdsContinueAfterAReopen) {
	ASSERT_NE(MustCreate("a"), nullptr);
	ASSERT_NE(MustCreate("b"), nullptr);
	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(MustCreate("c")->Id(), 2u);
	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(MustGet("c")->Id(), 2u);
	EXPECT_EQ(MustCreate("d")->Id(), 3u);
}

// Past the first page the stream continues on a linked CATALOG page, and past that on a third.
// Every table comes back, and the chain is reused, not reallocated, after a reopen.
TEST_F(CatalogTest, TheCatalogGrowsAcrossSeveralPages) {
	ASSERT_NO_FATAL_FAILURE(FillFirstCatalogPage());
	const std::size_t first_page_tables = Cat().TableCount();
	Table* overflow = MustCreate("overflow", SmallSchema(), MIN_FANOUT);
	ASSERT_NE(overflow, nullptr);
	ASSERT_TRUE(overflow->Insert(Row{Value{int64_t{1}}, Value{std::string("one")}}).ok());

	const page_id_t second = PageHeaderOf(CATALOG_ROOT_PAGE_ID).next_page_id;
	ASSERT_NE(second, INVALID_PAGE);
	EXPECT_EQ(PageHeaderOf(second).page_type, PageType::CATALOG);
	EXPECT_EQ(PageHeaderOf(second).next_page_id, INVALID_PAGE);
	EXPECT_GT(ReadCatalogHeader().stream_length, PAGE_BODY_SIZE);

	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(Cat().TableCount(), first_page_tables + 1);
	EXPECT_EQ(PageHeaderOf(CATALOG_ROOT_PAGE_ID).next_page_id, second);
	overflow = MustGet("overflow");
	ASSERT_NE(overflow, nullptr);
	auto row = overflow->Get(1);
	ASSERT_TRUE(row.has_value()) << row.error().message();

	// Fill the second page too, after the reopen: the third page links from the second.
	auto schema = Schema::Create({{"id", ColumnType::kInt64, 0}}, 0);
	ASSERT_TRUE(schema.has_value());
	int i = 0;
	while (PageHeaderOf(second).next_page_id == INVALID_PAGE) {
		ASSERT_NE(MustCreate(std::format("u{:04d}", i++), schema.value()), nullptr);
	}
	const page_id_t third = PageHeaderOf(second).next_page_id;
	EXPECT_EQ(PageHeaderOf(third).page_type, PageType::CATALOG);
	const std::size_t total = Cat().TableCount();
	const auto names = Cat().TableNames();

	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_EQ(Cat().TableCount(), total);
	EXPECT_EQ(Cat().TableNames(), names);
	EXPECT_EQ(PageHeaderOf(second).next_page_id, third);
}

// The on-disk format, byte for byte. If this fails, the format changed: bump
// CATALOG_FORMAT_VERSION and update DD-006, then this test.
TEST_F(CatalogTest, FormatIsPinned) {
	Table* t = MustCreate("t");
	ASSERT_NE(t, nullptr);

	std::vector<std::byte> expected;
	auto put = [&](auto value) {
		const auto* p = reinterpret_cast<const std::byte*>(&value);
		expected.insert(expected.end(), p, p + sizeof(value));
	};
	auto put_name = [&](std::string_view s) {
		put(static_cast<uint8_t>(s.size()));
		for (const char c : s) expected.push_back(static_cast<std::byte>(c));
	};
	put(uint32_t{1});              // format_version
	put(uint32_t{kStreamLength});  // stream_length
	put(uint32_t{1});              // table_count
	put(uint32_t{1});              // next_table_id
	put(uint32_t{0});              // table_id
	put_name("t");
	put(t->HeapFirstPageId());
	put(t->IndexHeaderPageId());
	put(uint16_t{0});  // primary key column
	put(uint16_t{2});  // column count
	put_name("id");
	put(static_cast<uint8_t>(ColumnType::kInt64));
	put(uint16_t{0});
	put_name("s");
	put(static_cast<uint8_t>(ColumnType::kVarchar));
	put(uint16_t{10});
	ASSERT_EQ(expected.size(), kStreamLength);

	expected.resize(PAGE_BODY_SIZE);  // zero to the end of the page
	EXPECT_EQ(PageBody(CATALOG_ROOT_PAGE_ID), expected);
	EXPECT_EQ(PageHeaderOf(CATALOG_ROOT_PAGE_ID).page_type, PageType::CATALOG);
}

// ---------------------------------------------------------------------------------------------
// Open rejects a corrupt catalog. Each test starts from the one-table catalog of FormatIsPinned
// and breaks one thing.
// ---------------------------------------------------------------------------------------------

class CatalogCorruptionTest : public CatalogTest {
  protected:
	void SetUp() override {
		ASSERT_NO_FATAL_FAILURE(CatalogTest::SetUp());
		ASSERT_NE(MustCreate("t"), nullptr);
		ASSERT_EQ(ReadCatalogHeader().stream_length, kStreamLength);
		Snapshot();
	}
};

TEST_F(CatalogCorruptionTest, TheUntouchedCatalogOpens) {
	ASSERT_NO_FATAL_FAILURE(Reopen());
	EXPECT_NE(MustGet("t"), nullptr);
}

TEST_F(CatalogCorruptionTest, UnknownFormatVersion) {
	ASSERT_NO_FATAL_FAILURE(PatchHeader([](CatalogHeader& h) { h.format_version = 2; }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, StreamLengthShorterThanTheHeader) {
	ASSERT_NO_FATAL_FAILURE(PatchHeader([](CatalogHeader& h) { h.stream_length = 15; }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, StreamLengthLongerThanTheChain) {
	ASSERT_NO_FATAL_FAILURE(PatchHeader(
	    [](CatalogHeader& h) { h.stream_length = static_cast<uint32_t>(PAGE_BODY_SIZE + 1); }));
	ExpectReopenIsCorruption();
}

// stream_length cut inside the entry: the entry runs past it.
TEST_F(CatalogCorruptionTest, EntryRunsPastStreamLength) {
	ASSERT_NO_FATAL_FAILURE(
	    PatchHeader([](CatalogHeader& h) { h.stream_length = kStreamLength - 1; }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, MoreTablesClaimedThanPresent) {
	ASSERT_NO_FATAL_FAILURE(PatchHeader([](CatalogHeader& h) { h.table_count = 2; }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, BytesLeftAfterTheLastEntry) {
	ASSERT_NO_FATAL_FAILURE(PatchHeader([](CatalogHeader& h) { h.table_count = 0; }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, InvalidTableName) {
	ASSERT_NO_FATAL_FAILURE(PatchField<char>(kOffName, 'T'));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, EmptyTableName) {
	// Only the length byte changes, so everything after it is misread too. Whichever check
	// fires first, Open must not accept it.
	ASSERT_NO_FATAL_FAILURE(PatchField<uint8_t>(kOffNameLen, 0));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, DuplicateTableName) {
	// Two entries both called "t": rebuild the stream with the entry doubled.
	ASSERT_NO_FATAL_FAILURE(PatchPage(CATALOG_ROOT_PAGE_ID, [](WritePageGuard& g) {
		auto body = g.MutableBody();
		const std::size_t entry = kStreamLength - CATALOG_HEADER_SIZE;
		std::memcpy(body.data() + kStreamLength, body.data() + CATALOG_HEADER_SIZE, entry);
		uint32_t other_id = 1;
		std::memcpy(body.data() + kStreamLength, &other_id, sizeof(other_id));
		auto h = CatalogHeader::ReadFrom(g.Body().first<CATALOG_HEADER_SIZE>());
		h.stream_length = static_cast<uint32_t>(kStreamLength + entry);
		h.table_count = 2;
		h.next_table_id = 2;
		h.WriteTo(body.first<CATALOG_HEADER_SIZE>());
	}));
	ExpectReopenIsCorruption();
}

// Two entries with different names and the SAME table_id: ids are never reused.
TEST_F(CatalogCorruptionTest, DuplicateTableId) {
	ASSERT_NO_FATAL_FAILURE(PatchPage(CATALOG_ROOT_PAGE_ID, [](WritePageGuard& g) {
		auto body = g.MutableBody();
		const std::size_t entry = kStreamLength - CATALOG_HEADER_SIZE;
		std::memcpy(body.data() + kStreamLength, body.data() + CATALOG_HEADER_SIZE, entry);
		body[kStreamLength + (kOffName - CATALOG_HEADER_SIZE)] = std::byte{'u'};
		auto h = CatalogHeader::ReadFrom(g.Body().first<CATALOG_HEADER_SIZE>());
		h.stream_length = static_cast<uint32_t>(kStreamLength + entry);
		h.table_count = 2;
		h.next_table_id = 2;
		h.WriteTo(body.first<CATALOG_HEADER_SIZE>());
	}));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, UnknownColumnType) {
	ASSERT_NO_FATAL_FAILURE(PatchField<uint8_t>(kOffCol1Type, 9));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ZeroColumnType) {
	ASSERT_NO_FATAL_FAILURE(PatchField<uint8_t>(kOffCol1Type, 0));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, PrimaryKeyColumnOutOfRange) {
	ASSERT_NO_FATAL_FAILURE(PatchField<uint16_t>(kOffPrimaryKey, 2));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, PrimaryKeyColumnNotInt64) {
	ASSERT_NO_FATAL_FAILURE(PatchField<uint16_t>(kOffPrimaryKey, 1));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ZeroColumns) {
	// The column bytes stay, so the parse also finds bytes left over — either way, rejected.
	ASSERT_NO_FATAL_FAILURE(PatchField<uint16_t>(kOffColumnCount, 0));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ReservedOrInvalidHeapPage) {
	for (const page_id_t id : {META_PAGE_ID, CATALOG_ROOT_PAGE_ID, INVALID_PAGE}) {
		SCOPED_TRACE(testing::Message() << "page " << id);
		ASSERT_NO_FATAL_FAILURE(PatchField<page_id_t>(kOffHeapFirst, id));
		ASSERT_NO_FATAL_FAILURE(ExpectReopenIsCorruption());
		// Put it back so the next iteration starts from a valid catalog.
		ASSERT_NO_FATAL_FAILURE(OpenRawAndRestore());
		ASSERT_NO_FATAL_FAILURE(OpenDb());
	}
}

TEST_F(CatalogCorruptionTest, ReservedOrInvalidIndexHeaderPage) {
	ASSERT_NO_FATAL_FAILURE(PatchField<page_id_t>(kOffIndexHeader, CATALOG_ROOT_PAGE_ID));
	ExpectReopenIsCorruption();
}

// A real page, just not an index header: Table::Open's own error comes through.
TEST_F(CatalogCorruptionTest, IndexHeaderPointingAtTheHeap) {
	const page_id_t heap = MustGet("t")->HeapFirstPageId();
	ASSERT_NO_FATAL_FAILURE(PatchField<page_id_t>(kOffIndexHeader, heap));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, TableIdNotBelowNextTableId) {
	ASSERT_NO_FATAL_FAILURE(PatchField<uint32_t>(kOffTableId, 1));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ChainLinksToTheMetaPage) {
	ASSERT_NO_FATAL_FAILURE(
	    PatchPage(CATALOG_ROOT_PAGE_ID, [](WritePageGuard& g) { g.SetNextPageId(META_PAGE_ID); }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ChainLinksBackToPageOne) {
	ASSERT_NO_FATAL_FAILURE(PatchPage(
	    CATALOG_ROOT_PAGE_ID, [](WritePageGuard& g) { g.SetNextPageId(CATALOG_ROOT_PAGE_ID); }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ChainWithACycle) {
	const page_id_t a = NewTypedPage(PageType::CATALOG);
	const page_id_t b = NewTypedPage(PageType::CATALOG);
	ASSERT_NO_FATAL_FAILURE(
	    PatchPage(CATALOG_ROOT_PAGE_ID, [&](WritePageGuard& g) { g.SetNextPageId(a); }));
	ASSERT_NO_FATAL_FAILURE(PatchPage(a, [&](WritePageGuard& g) { g.SetNextPageId(b); }));
	ASSERT_NO_FATAL_FAILURE(PatchPage(b, [&](WritePageGuard& g) { g.SetNextPageId(a); }));
	ExpectReopenIsCorruption();
}

TEST_F(CatalogCorruptionTest, ChainPageThatIsNotACatalogPage) {
	const page_id_t heap = NewTypedPage(PageType::HEAP);
	ASSERT_NO_FATAL_FAILURE(
	    PatchPage(CATALOG_ROOT_PAGE_ID, [&](WritePageGuard& g) { g.SetNextPageId(heap); }));
	ExpectReopenIsCorruption();
}

// format_version 0 says "never written", but page 1 links to a chain: something wrote it.
TEST_F(CatalogTest, AnUnwrittenCatalogWithAChainIsCorruption) {
	const page_id_t a = NewTypedPage(PageType::CATALOG);
	ASSERT_NO_FATAL_FAILURE(
	    PatchPage(CATALOG_ROOT_PAGE_ID, [&](WritePageGuard& g) { g.SetNextPageId(a); }));
	ExpectReopenIsCorruption();
}

// ---------------------------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------------------------

TEST_F(CatalogTest, ShutdownIsIdempotent) {
	ASSERT_NE(MustCreate("a"), nullptr);
	EXPECT_TRUE(db_->Shutdown().ok());
	EXPECT_TRUE(db_->Shutdown().ok());
	db_.reset();
	ASSERT_NO_FATAL_FAILURE(OpenDb());
	EXPECT_NE(MustGet("a"), nullptr);
}

// A failed Open leaves nothing open: the same file opens again cleanly once repaired, and the
// failed attempt did not abort in a destructor on the way out (which is what reaching the
// assertions shows).
TEST_F(CatalogTest, AFailedOpenLeavesTheFileUsable) {
	ASSERT_NE(MustCreate("a"), nullptr);
	Snapshot();
	ASSERT_NO_FATAL_FAILURE(PatchHeader([](CatalogHeader& h) { h.format_version = 7; }));
	ASSERT_NO_FATAL_FAILURE(ExpectReopenIsCorruption());

	ASSERT_NO_FATAL_FAILURE(OpenRawAndRestore());
	ASSERT_NO_FATAL_FAILURE(OpenDb());
	EXPECT_NE(MustGet("a"), nullptr);
}

}  // namespace kernsql

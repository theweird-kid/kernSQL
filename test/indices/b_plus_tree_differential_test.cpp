#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include "buffer/buffer_pool_manager.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "indices/b_plus_tree.hpp"
#include "indices/b_plus_tree_page.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * BPlusTree against std::map: the same seeded random operations applied to both, compared after
 * every one. The hand-written tests in b_plus_tree_test.cpp cover the cases someone thought of;
 * this covers the shapes nobody did.
 *
 * Every op's own result is checked against the map as it runs; CheckInvariants runs every 10th op
 * (it walks the whole tree, and after every op it dominated the run time). Every assertion is an
 * ASSERT, so a failure stops where it is caught — at the op itself for a wrong result, within 10
 * ops of the cause for a broken structure. The SCOPED_TRACE names the seed and the op, and the
 * same seed replays the same run exactly; to pin a structural failure to its op, drop the
 * interval to 1 and replay.
 *
 * 5 fanout pairs × 5 seeds. More seeds find more, but every case costs ~10 s under asan/tsan.
 *
 * Insert and point/range reads only. Delete joins the op mix once it exists.
 */

// (leaf_max, internal_max), seed.
using DifferentialParam = std::tuple<std::pair<uint16_t, uint16_t>, uint32_t>;

class BPlusTreeDifferentialTest : public ::testing::TestWithParam<DifferentialParam> {
  protected:
	static constexpr int kOps = 2000;

	// Small enough that roughly a third of the inserts are duplicates by the end, so the
	// duplicate path runs against every shape the tree passes through.
	static constexpr uint64_t kKeyRange = 2000;

	// Not BPlusTreeTest's 16: at MIN_FANOUT ~1300 keys reach height 8 or more, and Insert peaks
	// at 2H + 2 frames.
	static constexpr std::size_t kFrames = 64;

	void SetUp() override {
		// Parameterized names contain '/', which is not a file name. The pid keeps two presets
		// running the same test at once off each other's file.
		std::string name = std::string("kernsql_b_plus_tree_differential_") +
		                   testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
		                   std::to_string(getpid());
		std::ranges::replace(name, '/', '_');
		path_ = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path_);

		OpenPool();

		const auto [leaf_max, internal_max] = std::get<0>(GetParam());
		auto created = BPlusTree::Create(*bpm_, leaf_max, internal_max);
		ASSERT_TRUE(created.has_value()) << created.error().message();
		tree_ = std::move(created.value());
	}

	void TearDown() override {
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

	// Mostly from a small dense range; now and then the ends of the domain.
	index_key_t RandomKey() {
		const uint64_t roll = rng_() % 100;
		if (roll == 0) return INT64_MIN;
		if (roll == 1) return INT64_MAX;
		return static_cast<index_key_t>(rng_() % kKeyRange);
	}

	// Scan [lo, hi] on both and compare exactly.
	void ExpectScanMatches(index_key_t lo, index_key_t hi) {
		std::vector<std::pair<index_key_t, RID>> want;
		if (lo <= hi) {
			for (auto it = oracle_.lower_bound(lo); it != oracle_.upper_bound(hi); ++it) {
				want.emplace_back(it->first, it->second);
			}
		}

		std::vector<std::pair<index_key_t, RID>> got;
		auto it = tree_->Scan(lo, hi);
		ASSERT_TRUE(it.has_value()) << it.error().message();
		for (;;) {
			auto more = it->Next();
			ASSERT_TRUE(more.has_value()) << more.error().message();
			if (!more.value()) break;
			got.emplace_back(it->Key(), it->Rid());
		}
		ASSERT_EQ(got, want) << "scan [" << lo << ", " << hi << "]";
	}

	// Get on both and compare, absence included.
	void ExpectGetMatches(index_key_t key) {
		auto rid = tree_->Get(key);
		const auto found = oracle_.find(key);
		if (found == oracle_.end()) {
			ASSERT_FALSE(rid.has_value()) << "key " << key << " should be absent";
			ASSERT_EQ(rid.error().code(), ErrorCode::kNotFound);
		} else {
			ASSERT_TRUE(rid.has_value()) << "key " << key << ": " << rid.error().message();
			ASSERT_EQ(rid.value(), found->second) << "key " << key;
		}
	}

	std::filesystem::path path_;
	std::unique_ptr<DiskManager> dm_;
	std::unique_ptr<BufferPoolManager> bpm_;
	std::unique_ptr<BPlusTree> tree_;

	std::mt19937_64 rng_{std::get<1>(GetParam())};
	std::map<index_key_t, RID> oracle_;
};

TEST_P(BPlusTreeDifferentialTest, MatchesAStdMap) {
	for (int op = 0; op < kOps; ++op) {
		SCOPED_TRACE(testing::Message() << "seed " << std::get<1>(GetParam()) << ", op " << op);
		const uint64_t roll = rng_() % 100;

		if (roll < 70) {
			// The RID comes from the op, not the key, so a duplicate that overwrote the stored
			// RID shows up on the next Get of that key.
			const index_key_t key = RandomKey();
			const RID rid{static_cast<page_id_t>(1000 + op), static_cast<slot_id_t>(op % 7)};
			SCOPED_TRACE(testing::Message() << "insert " << key);

			const Status s = tree_->Insert(key, rid);
			if (oracle_.contains(key)) {
				ASSERT_EQ(s.code(), ErrorCode::kDuplicateKey) << s.message();
			} else {
				ASSERT_TRUE(s.ok()) << s.message();
				oracle_.emplace(key, rid);
			}
		} else if (roll < 90) {
			const index_key_t key = RandomKey();
			ASSERT_NO_FATAL_FAILURE(ExpectGetMatches(key));
		} else {
			// Short ranges, so a scan per op stays cheap; the full scan below covers the rest.
			const index_key_t lo = RandomKey();
			const auto width = static_cast<index_key_t>(rng_() % 50);
			const index_key_t hi = lo > INT64_MAX - width ? INT64_MAX : lo + width;
			ASSERT_NO_FATAL_FAILURE(ExpectScanMatches(lo, hi));
		}

		if (op % 10 == 0) {
			ASSERT_TRUE(tree_->CheckInvariants());
		}
		if (op % 250 == 0) {
			ASSERT_NO_FATAL_FAILURE(ExpectScanMatches(INT64_MIN, INT64_MAX));
		}
	}

	ASSERT_NO_FATAL_FAILURE(ExpectScanMatches(INT64_MIN, INT64_MAX));

	// Everything above, again, from disk.
	ASSERT_NO_FATAL_FAILURE(Reopen());
	ASSERT_TRUE(tree_->CheckInvariants());
	ASSERT_NO_FATAL_FAILURE(ExpectScanMatches(INT64_MIN, INT64_MAX));
	for (const auto& [key, rid] : oracle_) {
		ASSERT_NO_FATAL_FAILURE(ExpectGetMatches(key));
	}
}

// "leaf3_internal5_seed7", so a failing ctest name says how to replay it. A function rather than
// a lambda in the macro: a structured binding's comma would split the macro arguments.
std::string DifferentialName(const testing::TestParamInfo<DifferentialParam>& info) {
	const auto [leaf_max, internal_max] = std::get<0>(info.param);
	return "leaf" + std::to_string(leaf_max) + "_internal" + std::to_string(internal_max) +
	       "_seed" + std::to_string(std::get<1>(info.param));
}

// 3/5 and 5/3 catch leaf_max_ and internal_max_ swapped somewhere. NODE_CAPACITY only reaches
// height 2 in 2000 ops, but it is the fanout production uses.
INSTANTIATE_TEST_SUITE_P(
    Fanouts, BPlusTreeDifferentialTest,
    ::testing::Combine(::testing::Values(std::pair<uint16_t, uint16_t>{3, 3},
                                         std::pair<uint16_t, uint16_t>{4, 4},
                                         std::pair<uint16_t, uint16_t>{3, 5},
                                         std::pair<uint16_t, uint16_t>{5, 3},
                                         std::pair<uint16_t, uint16_t>{NODE_CAPACITY,
                                                                       NODE_CAPACITY}),
                       ::testing::Range<uint32_t>(1, 6)),
    DifferentialName);

}  // namespace kernsql

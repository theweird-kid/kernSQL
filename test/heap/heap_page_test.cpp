#include "heap/heap_page.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

class HeapPageTest : public ::testing::Test {
  protected:
	// A bare body: no DiskManager, no BufferPoolManager, no guard. That is the entire point of
	// the view types, and it is what lets these tests assert against the on-disk layout itself —
	// header fields and slot entries — rather than only against what the API chooses to report.
	void SetUp() override {
		body_.fill(std::byte{0});
		Page().Init();
	}

	HeapPage Page() { return HeapPage(body_); }
	ConstHeapPage View() const { return ConstHeapPage(body_); }

	// n bytes all equal to `fill`, so a round-trip failure tells you WHICH tuple moved wrong
	// rather than just that some bytes differ.
	static std::vector<std::byte> Tuple(std::byte fill, std::size_t n) {
		return std::vector<std::byte>(n, fill);
	}

	static std::span<const std::byte> Bytes(std::string_view s) {
		return std::as_bytes(std::span{s});
	}

	static std::string_view AsChars(std::span<const std::byte> bytes) {
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	// Materialises a span so EXPECT_EQ compares CONTENT. Comparing two spans — or two data()
	// pointers — compares addresses, which is never what a round-trip assertion means, and it
	// fails in a way that looks like a real mismatch.
	static std::vector<std::byte> Read(std::span<const std::byte> bytes) {
		return {bytes.begin(), bytes.end()};
	}

	// Stamp header and slot bytes straight into the body, bypassing every operation. ONLY for
	// the CheckInvariants tests: their whole point is to hand that function a page the API
	// cannot produce, and it is the HEADER that every operation reads, so scribbling tuple bytes
	// would change nothing anything looks at.
	void PokeHeader(const HeapSubHeader& header) {
		header.WriteTo(std::span<std::byte, HEAP_SUB_HEADER_SIZE>{body_.data(),
		                                                          HEAP_SUB_HEADER_SIZE});
	}
	void PokeSlot(slot_id_t slot, const Slot& entry) {
		entry.WriteTo(std::span<std::byte, SLOT_SIZE>{
		    body_.data() + HEAP_SUB_HEADER_SIZE + std::size_t{slot} * SLOT_SIZE, SLOT_SIZE});
	}

	std::array<std::byte, PAGE_BODY_SIZE> body_{};
};

// ---------------------------------------------------------------------------------------------
// Init and the empty page
// ---------------------------------------------------------------------------------------------

// slot_count/live_count/dead_bytes zero, tuple_data_start at PAGE_BODY_SIZE, invariants hold.
TEST_F(HeapPageTest, FreshPageIsEmptyAndFullyFree) {
	SetUp();
	HeapPage page = Page();
	auto header = page.Header();

	EXPECT_EQ(header.slot_count, 0);
	EXPECT_EQ(header.live_count, 0);
	EXPECT_EQ(header.dead_bytes, 0);
	EXPECT_EQ(header.tuple_data_start, PAGE_BODY_SIZE);
}

// Contiguous() is the body less the sub-header; Reclaimable() equals it when there is no garbage.
TEST_F(HeapPageTest, FreshPageReportsAllSpaceAsFree) {
	SetUp();
	HeapPage page = Page();

	EXPECT_EQ(page.Reclaimable(), page.Contiguous());
}

TEST_F(HeapPageTest, GetOnAnEmptyPageIsNotFound) {
	SetUp();
	HeapPage page = Page();

	EXPECT_FALSE(page.Get(2).has_value());
	EXPECT_EQ(page.Get(2).error().code(), ErrorCode::kNotFound);
}

// ---------------------------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------------------------

// Slot ids come back 0, 1, 2...; every tuple reads back byte-identical.
TEST_F(HeapPageTest, InsertReturnsSequentialSlotIdsAndRoundTripsBytes) {
	SetUp();
	HeapPage page = Page();

	for (std::size_t i = 0; i < 5; i++) {
		auto slot = page.Insert(Bytes(std::format("Slot {}", i)));
		EXPECT_EQ(slot.value(), i);
	}

	for (std::size_t i = 0; i < 5; i++) {
		auto tuple = page.Get(i);
		EXPECT_EQ(AsChars(tuple.value()), std::format("Slot {}", i));
	}
}

// Tuples grow backward: each insert lowers tuple_data_start by exactly the tuple length.
TEST_F(HeapPageTest, InsertLowersTheLowWaterMarkByTheTupleLength) {
	SetUp();
	HeapPage page = Page();
	uint16_t cursor{PAGE_BODY_SIZE};

	for (std::size_t i = 0; i < 5; i++) {
		std::string write_tuple = std::format("Slot {}", i);
		auto slot = page.Insert(Bytes(write_tuple));
		auto header = page.Header();
		cursor -= write_tuple.size();
		EXPECT_EQ(header.tuple_data_start, cursor);
	}
}

TEST_F(HeapPageTest, InsertRejectsAZeroLengthTuple) {
	SetUp();
	HeapPage page = Page();
	auto slot = page.Insert(Bytes(""));

	EXPECT_FALSE(slot.has_value());
	EXPECT_EQ(slot.error().code(), ErrorCode::kInvalidArgument);
}

// Fill the page, then assert the next insert is PageFull and the page is unchanged.
TEST_F(HeapPageTest, InsertOnAFullPageReturnsPageFullAndChangesNothing) {
	HeapPage page = Page();

	// Filled through the API rather than by scribbling bytes into the body. Insert decides on the
	// HEADER and never on page content, so bytes written behind its back fill nothing: the page
	// still reports every byte free and the insert under test would succeed.
	const auto filler = Tuple(std::byte{29}, MAX_TUPLE_SIZE);
	while (page.Insert(filler).has_value()) {
	}

	const std::array<std::byte, PAGE_BODY_SIZE> before = body_;

	auto tuple_insert = page.Insert(Tuple(std::byte{42}, 100));
	EXPECT_FALSE(tuple_insert.has_value());
	EXPECT_EQ(tuple_insert.error().code(), ErrorCode::kPageFull);

	// The other half of the name: a failed insert must not have moved the header first.
	EXPECT_EQ(std::memcmp(before.data(), body_.data(), PAGE_BODY_SIZE), 0);
	EXPECT_TRUE(page.CheckInvariants());
}

// The 2000-byte cap exists to guarantee this; if it ever stops holding, the cap is wrong.
TEST_F(HeapPageTest, TwoMaxSizeTuplesFitOnOnePage) {
	HeapPage page = Page();

	// 2 * (2000 + 4) + 8 == 4016 <= 4064. This is the entire reason the cap is 2000 rather than
	// the 4052 the format could physically hold: one row per page turns a heap into a linked list.
	EXPECT_TRUE(page.Insert(Tuple(std::byte{1}, MAX_TUPLE_SIZE)).has_value());
	EXPECT_TRUE(page.Insert(Tuple(std::byte{2}, MAX_TUPLE_SIZE)).has_value());

	EXPECT_EQ(page.LiveCount(), 2);
	EXPECT_TRUE(page.CheckInvariants());
}

TEST_F(HeapPageTest, InsertRejectsATupleOverMaxTupleSize) {
	HeapPage page = Page();
	auto slot = page.Insert(Tuple(std::byte{29}, MAX_TUPLE_SIZE + 1));

	EXPECT_FALSE(slot.has_value());
	EXPECT_EQ(slot.error().code(), ErrorCode::kInvalidArgument);
}

// Reuses the slot ENTRY (slot_count does not grow) but dead_bytes stays put — the dead tuple's
// bytes are unreachable until Compact. This is the accounting rule most likely to be got wrong.
TEST_F(HeapPageTest, InsertReusesADeadSlotWithoutReclaimingItsBytes) {
	HeapPage page = Page();
	auto slot_insert = page.Insert(Tuple(std::byte{29}, 100));
	ASSERT_TRUE(slot_insert.has_value());
	auto slot_id = slot_insert.value();

	auto st = page.Delete(slot_insert.value());
	ASSERT_EQ(st.code(), ErrorCode::kOk);

	uint16_t init_dead_bytes = page.Header().dead_bytes;
	slot_insert = page.Insert(Tuple(std::byte{29}, 50));  // reuse
	ASSERT_TRUE(slot_insert.has_value());
	ASSERT_EQ(slot_insert.value(), slot_id);

	ASSERT_EQ(page.Header().dead_bytes, init_dead_bytes);
}

// With several dead slots, the lowest index is taken first.
TEST_F(HeapPageTest, InsertReusesTheLowestDeadSlot) {
	HeapPage page = Page();

	std::vector<slot_id_t> slots;
	for (size_t i = 0; i < 13; i++) {
		auto slot_insert = page.Insert(Tuple(std::byte{29}, 150));
		EXPECT_TRUE(slot_insert.has_value());
		slots.push_back(slot_insert.value());
	}

	for (size_t i = 0; i < 13; i++) {
		auto st = page.Delete(slots[i]);
		EXPECT_EQ(st.code(), ErrorCode::kOk);
	}

	auto slot_insert = page.Insert(Tuple(std::byte{29}, 150));
	EXPECT_TRUE(slot_insert.has_value());
	ASSERT_EQ(slot_insert.value(), slots.front());
}

// Insert does not compact, by contract: it fails while Reclaimable() says the space exists.
TEST_F(HeapPageTest, InsertDoesNotCompactOnItsOwn) {
	HeapPage page = Page();

	// Two max-size tuples fill the page down to 48 contiguous bytes; deleting the first turns
	// 2000 of those bytes into garbage that only Compact can recover.
	auto first = page.Insert(Tuple(std::byte{1}, MAX_TUPLE_SIZE));
	auto second = page.Insert(Tuple(std::byte{2}, MAX_TUPLE_SIZE));
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(page.Delete(*first).ok());

	// The premise, stated rather than assumed: the space exists, but not contiguously.
	constexpr std::size_t kWanted = 1000;
	ASSERT_LT(page.Contiguous(), kWanted);
	ASSERT_GE(page.Reclaimable(), kWanted);

	const std::array<std::byte, PAGE_BODY_SIZE> before = body_;

	auto blocked = page.Insert(Tuple(std::byte{3}, kWanted));
	EXPECT_FALSE(blocked.has_value());
	EXPECT_EQ(blocked.error().code(), ErrorCode::kPageFull);

	// The point of the test. Insert hands the decision back to the caller instead of compacting
	// on its own, so the garbage is still garbage and the page is untouched — byte for byte,
	// which is the only assertion that would catch a compaction that ran and then failed anyway.
	EXPECT_EQ(page.Header().dead_bytes, MAX_TUPLE_SIZE);
	EXPECT_GE(page.Reclaimable(), kWanted);
	EXPECT_EQ(std::memcmp(before.data(), body_.data(), PAGE_BODY_SIZE), 0);
	EXPECT_TRUE(page.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------------------------

// live_count drops, dead_bytes rises by exactly the tuple length, slot reads {0, 0}.
TEST_F(HeapPageTest, DeleteMarksTheSlotDeadAndAccountsItsBytes) {
	HeapPage page = Page();
	for (size_t i = 0; i < 13; i++) {
		auto slot_insert = page.Insert(Tuple(std::byte{29}, 150));
		EXPECT_TRUE(slot_insert.has_value());
	}

	uint16_t init_live_count = page.Header().live_count;

	auto st = page.Delete(1);
	ASSERT_EQ(st.code(), ErrorCode::kOk);

	uint16_t final_live_count = page.Header().live_count;
	uint16_t final_dead_bytes = page.Header().dead_bytes;

	ASSERT_EQ(init_live_count, final_live_count + 1);
	ASSERT_EQ(final_dead_bytes, 150);
}

// slot_count is unchanged, so a stale RID still resolves — to NotFound, never to bytes.
TEST_F(HeapPageTest, DeletedSlotKeepsItsIndexForever) {
	HeapPage page = Page();
	auto first = page.Insert(Bytes("first"));
	auto middle = page.Insert(Bytes("middle"));
	auto last = page.Insert(Bytes("last"));
	ASSERT_TRUE(first.has_value() && middle.has_value() && last.has_value());

	ASSERT_TRUE(page.Delete(*middle).ok());

	// The index survives: the array does not shrink and nothing after it slides down.
	EXPECT_EQ(page.SlotCount(), 3);
	EXPECT_EQ(page.LiveCount(), 2);
	EXPECT_TRUE(page.SlotAt(*middle).IsDead());

	// Dead has exactly one representation, which is what makes offset == 0 => length == 0
	// assertable in CheckInvariants.
	EXPECT_EQ(page.SlotAt(*middle).offset, 0);
	EXPECT_EQ(page.SlotAt(*middle).length, 0);

	// A stale RID gets an answer, not garbage, and its neighbours are untouched.
	EXPECT_FALSE(page.Get(*middle).has_value());
	EXPECT_EQ(page.Get(*middle).error().code(), ErrorCode::kNotFound);
	EXPECT_EQ(AsChars(page.Get(*first).value()), "first");
	EXPECT_EQ(AsChars(page.Get(*last).value()), "last");

	// "Forever" is the part worth testing: a compaction rewrites every offset in the page and the
	// dead slot still sits at its own index, still dead, while the live RIDs still resolve.
	page.Compact();
	EXPECT_EQ(page.SlotCount(), 3);
	EXPECT_TRUE(page.SlotAt(*middle).IsDead());
	EXPECT_EQ(AsChars(page.Get(*first).value()), "first");
	EXPECT_EQ(AsChars(page.Get(*last).value()), "last");
	EXPECT_TRUE(page.CheckInvariants());
}

TEST_F(HeapPageTest, DeleteOnADeadSlotIsNotFound) {
	HeapPage page = Page();
	auto slot = page.Insert(Tuple(std::byte{7}, 150));
	ASSERT_TRUE(slot.has_value());
	ASSERT_TRUE(page.Delete(*slot).ok());

	const std::array<std::byte, PAGE_BODY_SIZE> before = body_;

	auto again = page.Delete(*slot);
	EXPECT_EQ(again.code(), ErrorCode::kNotFound);

	// The real risk in a double delete is not the error code, it is dead_bytes counting the same
	// 150 bytes twice and live_count going negative — both of which would corrupt the accounting
	// permanently, since nothing ever subtracts from dead_bytes.
	EXPECT_EQ(page.Header().dead_bytes, 150);
	EXPECT_EQ(page.LiveCount(), 0);
	EXPECT_EQ(std::memcmp(before.data(), body_.data(), PAGE_BODY_SIZE), 0);
	EXPECT_TRUE(page.CheckInvariants());
}

TEST_F(HeapPageTest, DeleteOnAnOutOfRangeSlotIsNotFound) {
	HeapPage page = Page();
	auto slot = page.Insert(Tuple(std::byte{7}, 150));
	ASSERT_TRUE(slot.has_value());

	const std::array<std::byte, PAGE_BODY_SIZE> before = body_;

	// One past the end is the boundary that separates a correct `slot_count <= slot_id` from a
	// buggy `<`; the far-out value only checks that nothing indexes wildly.
	EXPECT_EQ(page.Delete(page.SlotCount()).code(), ErrorCode::kNotFound);
	EXPECT_EQ(page.Delete(9999).code(), ErrorCode::kNotFound);

	EXPECT_EQ(std::memcmp(before.data(), body_.data(), PAGE_BODY_SIZE), 0);
	EXPECT_TRUE(page.CheckInvariants());
}

// Deleting everything leaves live_count 0 with tuple_data_start still low — the space is only
// recovered by Compact, and Reclaimable() is the number that knows it.
TEST_F(HeapPageTest, DeletingEveryTupleLeavesTheSpaceReclaimableButNotContiguous) {
	constexpr slot_id_t kTuples = 5;
	constexpr std::size_t kLength = 300;

	HeapPage page = Page();
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(std::byte{9}, kLength)).has_value());
	}

	const auto low_water_mark = page.Header().tuple_data_start;
	const std::size_t contiguous_before = page.Contiguous();

	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Delete(i).ok());
	}

	EXPECT_EQ(page.LiveCount(), 0);
	EXPECT_EQ(page.SlotCount(), kTuples);  // slots outlive their tuples

	// Delete does not move a single byte, so the low-water mark and the contiguous gap are exactly
	// where they were. An empty page that still reports itself nearly full is the correct state.
	EXPECT_EQ(page.Header().tuple_data_start, low_water_mark);
	EXPECT_EQ(page.Contiguous(), contiguous_before);

	// Reclaimable() is the only number that knows the space is recoverable, and it is what a
	// cross-page free-space mechanism would publish. Everything but the sub-header and the slot
	// array is now free — after a compaction nobody has run yet.
	EXPECT_EQ(page.Header().dead_bytes, kTuples * kLength);
	EXPECT_EQ(page.Reclaimable(), page.Contiguous() + kTuples * kLength);
	EXPECT_EQ(page.Reclaimable(), PAGE_BODY_SIZE - HEAP_SUB_HEADER_SIZE - kTuples * SLOT_SIZE);
	EXPECT_TRUE(page.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Update — case 1, in place
// ---------------------------------------------------------------------------------------------

// Same offset, shorter length, and the difference lands in dead_bytes. Those orphaned bytes are
// the ones no slot walk can ever rediscover, which is why dead_bytes is stored at all.
TEST_F(HeapPageTest, UpdateToAShorterTupleOverwritesInPlace) {
	HeapPage page = Page();

	auto slot_id = page.Insert(Tuple(std::byte{29}, 150));
	ASSERT_TRUE(slot_id.has_value());

	uint16_t init_dead_bytes = page.Header().dead_bytes;

	auto st = page.Update(slot_id.value(), Tuple(std::byte{29}, 100));
	ASSERT_TRUE(st.has_value());
	ASSERT_EQ(st.value(), UpdateOutcome::kSamePage);

	uint16_t final_dead_bytes = page.Header().dead_bytes;

	ASSERT_EQ(final_dead_bytes - init_dead_bytes, 50);
}

TEST_F(HeapPageTest, UpdateToAnEqualLengthTupleAddsNoGarbage) {
	HeapPage page = Page();

	auto slot_id = page.Insert(Tuple(std::byte{29}, 150));
	ASSERT_TRUE(slot_id.has_value());

	uint16_t init_dead_bytes = page.Header().dead_bytes;

	auto st = page.Update(slot_id.value(), Tuple(std::byte{30}, 150));
	ASSERT_TRUE(st.has_value());
	ASSERT_EQ(st.value(), UpdateOutcome::kSamePage);

	uint16_t final_dead_bytes = page.Header().dead_bytes;

	ASSERT_EQ(final_dead_bytes, init_dead_bytes);
}

// Its neighbours must be untouched — an in-place overwrite that runs long corrupts the tuple
// physically adjacent to it, and only a neighbour check catches that.
TEST_F(HeapPageTest, UpdateInPlaceLeavesNeighbouringTuplesIntact) {
	constexpr slot_id_t kTuples = 13;
	constexpr std::size_t kLength = 150;
	constexpr slot_id_t kTarget = 5;

	auto fill = [](slot_id_t i) { return static_cast<std::byte>(i + 1); };

	HeapPage page = Page();
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(fill(i), kLength)).has_value());
	}

	// Tuples grow BACKWARD: Insert places each one at tuple_data_start - length and then lowers
	// the mark, so a later slot sits at a LOWER offset. Slot 4 is therefore immediately ABOVE
	// slot 5 in the body, and an in-place overwrite that runs long writes upward into it. Slot 6
	// lives below slot 5 and cannot be reached by a forward write at all — checking only slot 6
	// is checking the one neighbour that is safe no matter how badly Update overruns.
	ASSERT_GT(page.SlotAt(kTarget - 1).offset, page.SlotAt(kTarget).offset);
	ASSERT_GT(page.SlotAt(kTarget).offset, page.SlotAt(kTarget + 1).offset);

	auto st = page.Update(kTarget, Tuple(std::byte{99}, 100));
	ASSERT_TRUE(st.has_value());
	ASSERT_EQ(st.value(), UpdateOutcome::kSamePage);

	// The tuple itself: exactly 100 bytes of the new fill. A shrink that forgot to update the
	// slot length would hand back 150 bytes with a 50-byte tail of stale data.
	EXPECT_EQ(Read(page.Get(kTarget).value()), Tuple(std::byte{99}, 100));

	// Every other tuple, each against its OWN fill, so a failure names the slot that moved.
	for (slot_id_t i = 0; i < kTuples; ++i) {
		if (i == kTarget) continue;
		auto tuple = page.Get(i);
		ASSERT_TRUE(tuple.has_value()) << "slot " << i;
		EXPECT_EQ(Read(tuple.value()), Tuple(fill(i), kLength)) << "slot " << i;
	}
	EXPECT_TRUE(page.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Update — case 2, relocation
// ---------------------------------------------------------------------------------------------

// Grows with room at the low-water mark: new offset, old extent added to dead_bytes, same RID.
TEST_F(HeapPageTest, UpdateThatGrowsRelocatesWithinThePage) {
	HeapPage page = Page();
	auto grown = page.Insert(Tuple(std::byte{1}, 100));
	auto middle = page.Insert(Tuple(std::byte{2}, 200));
	auto last = page.Insert(Tuple(std::byte{3}, 300));
	ASSERT_TRUE(grown.has_value() && middle.has_value() && last.has_value());

	const auto old_offset = page.SlotAt(*grown).offset;
	ASSERT_GE(page.Contiguous(), 500u);  // the premise: it fits without reclaiming anything

	auto outcome = page.Update(*grown, Tuple(std::byte{9}, 500));
	ASSERT_TRUE(outcome.has_value());
	EXPECT_EQ(*outcome, UpdateOutcome::kSamePage);

	// The RID survives because the slot INDEX did not change, even though the tuple moved.
	EXPECT_EQ(page.SlotCount(), 3);
	EXPECT_EQ(page.LiveCount(), 3);
	EXPECT_NE(page.SlotAt(*grown).offset, old_offset);
	EXPECT_EQ(page.SlotAt(*grown).length, 500);
	EXPECT_EQ(Read(page.Get(*grown).value()), Tuple(std::byte{9}, 500));

	// No compaction happened, and dead_bytes is what proves it: the old 100-byte extent is still
	// garbage. A slow-path update would have reclaimed it and left this at zero.
	EXPECT_EQ(page.Header().dead_bytes, 100);
	EXPECT_EQ(page.Header().tuple_data_start, page.SlotAt(*grown).offset);

	EXPECT_EQ(Read(page.Get(*middle).value()), Tuple(std::byte{2}, 200));
	EXPECT_EQ(Read(page.Get(*last).value()), Tuple(std::byte{3}, 300));
	EXPECT_TRUE(page.CheckInvariants());
}

// Grows past Contiguous() but inside Reclaimable(): compacts, keeps the slot id, keeps every
// other RID readable, and ends with dead_bytes back at zero.
TEST_F(HeapPageTest, UpdateThatGrowsCompactsWhenItHasTo) {
	HeapPage page = Page();
	auto doomed = page.Insert(Tuple(std::byte{1}, 2000));
	auto grown = page.Insert(Tuple(std::byte{2}, 1000));
	auto bystander = page.Insert(Tuple(std::byte{3}, 1000));
	ASSERT_TRUE(doomed.has_value() && grown.has_value() && bystander.has_value());
	ASSERT_TRUE(page.Delete(*doomed).ok());

	// The premise: 1500 bytes do not fit in the gap, but they do fit once the garbage is
	// reclaimed. Stated rather than assumed, so this cannot silently become a fast-path test.
	ASSERT_LT(page.Contiguous(), 1500u);
	ASSERT_GE(page.Reclaimable(), 1500u);

	auto outcome = page.Update(*grown, Tuple(std::byte{9}, 1500));
	ASSERT_TRUE(outcome.has_value());
	EXPECT_EQ(*outcome, UpdateOutcome::kSamePage);

	// The slot id is unchanged, which is the whole reason case 2 returns kSamePage.
	EXPECT_EQ(Read(page.Get(*grown).value()), Tuple(std::byte{9}, 1500));
	EXPECT_EQ(Read(page.Get(*bystander).value()), Tuple(std::byte{3}, 1000));

	// The compaction ran: every byte below the low-water mark is now live, so dead_bytes is zero
	// by definition and the region is packed against the end of the body.
	EXPECT_EQ(page.Header().dead_bytes, 0);
	EXPECT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - 1500 - 1000);

	// The deleted slot kept its index through the compaction and is still dead.
	EXPECT_EQ(page.SlotCount(), 3);
	EXPECT_TRUE(page.SlotAt(*doomed).IsDead());
	EXPECT_EQ(page.Get(*doomed).error().code(), ErrorCode::kNotFound);
	EXPECT_TRUE(page.CheckInvariants());
}

// live_count must survive the kill-compact-replace dance — off by one here is the classic bug.
TEST_F(HeapPageTest, UpdateViaCompactionLeavesLiveCountCorrect) {
	constexpr slot_id_t kTuples = 5;
	constexpr std::size_t kLength = 700;

	HeapPage page = Page();
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(static_cast<std::byte>(i + 1), kLength)).has_value());
	}
	ASSERT_TRUE(page.Delete(0).ok());
	ASSERT_TRUE(page.Delete(2).ok());
	ASSERT_EQ(page.LiveCount(), 3);

	// Force the slow path on slot 4: too big for the gap, small enough once the two deleted
	// tuples and slot 4's own bytes are reclaimed.
	ASSERT_LT(page.Contiguous(), 1800u);
	ASSERT_GE(page.Reclaimable(), 1800u);
	auto outcome = page.Update(4, Tuple(std::byte{9}, 1800));
	ASSERT_TRUE(outcome.has_value());
	EXPECT_EQ(*outcome, UpdateOutcome::kSamePage);

	// Update decrements live_count to hand Compact a self-consistent page and puts it back
	// afterwards. Off by one in either direction is the classic bug, so count the slot array
	// independently rather than trusting the field to agree with itself.
	std::size_t live_slots = 0;
	for (slot_id_t i = 0; i < page.SlotCount(); ++i) {
		if (!page.SlotAt(i).IsDead()) ++live_slots;
	}
	EXPECT_EQ(live_slots, 3u);
	EXPECT_EQ(page.LiveCount(), 3);
	EXPECT_EQ(page.SlotCount(), kTuples);

	// And the live set is the RIGHT three: the survivors, not a resurrected dead slot.
	EXPECT_EQ(Read(page.Get(1).value()), Tuple(std::byte{2}, kLength));
	EXPECT_EQ(Read(page.Get(3).value()), Tuple(std::byte{4}, kLength));
	EXPECT_EQ(Read(page.Get(4).value()), Tuple(std::byte{9}, 1800));
	EXPECT_EQ(page.Get(0).error().code(), ErrorCode::kNotFound);
	EXPECT_EQ(page.Get(2).error().code(), ErrorCode::kNotFound);
	EXPECT_TRUE(page.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Update — case 3 and validation
// ---------------------------------------------------------------------------------------------

// kDoesNotFit, and the body must be BYTE-IDENTICAL to a snapshot taken before the call.
TEST_F(HeapPageTest, UpdateThatCannotFitLeavesThePageByteIdentical) {
	HeapPage page = Page();

	auto slot_1 = page.Insert(Tuple(std::byte{29}, 2000));
	EXPECT_TRUE(slot_1.has_value());
	auto slot_2 = page.Insert(Tuple(std::byte{29}, 2000));
	EXPECT_TRUE(slot_2.has_value());
	auto slot_3 = page.Insert(Tuple(std::byte{29}, 30));
	EXPECT_TRUE(slot_3.has_value());

	auto st = page.Update(slot_3.value(), Tuple(std::byte{30}, 2000));
	EXPECT_EQ(st, UpdateOutcome::kDoesNotFit);
}

TEST_F(HeapPageTest, UpdateRejectsAZeroLengthTuple) {
	HeapPage page = Page();

	auto slot = page.Insert(Tuple(std::byte{29}, 2000));
	EXPECT_TRUE(slot.has_value());

	auto st = page.Update(slot.value(), Tuple(std::byte{30}, 0));
	EXPECT_FALSE(st.has_value());
	EXPECT_EQ(st.error().code(), ErrorCode::kInvalidArgument);
}

TEST_F(HeapPageTest, UpdateRejectsATupleOverMaxTupleSize) {
	HeapPage page = Page();

	auto slot = page.Insert(Tuple(std::byte{29}, 2000));
	EXPECT_TRUE(slot.has_value());

	auto st = page.Update(slot.value(), Tuple(std::byte{30}, 2300));
	EXPECT_FALSE(st.has_value());
	EXPECT_EQ(st.error().code(), ErrorCode::kInvalidArgument);
}

// Must be NotFound, not a resurrected slot and not Corruption.
TEST_F(HeapPageTest, UpdateOnADeadSlotIsNotFound) {
	HeapPage page = Page();

	auto slot = page.Insert(Tuple(std::byte{29}, 2000));
	EXPECT_TRUE(slot.has_value());

	auto st = page.Delete(slot.value());
	ASSERT_EQ(st.code(), ErrorCode::kOk);

	auto updated = page.Update(slot.value(), Tuple(std::byte{30}, 1800));
	EXPECT_FALSE(updated.has_value());
	EXPECT_EQ(updated.error().code(), ErrorCode::kNotFound);
}

TEST_F(HeapPageTest, UpdateOnAnOutOfRangeSlotIsNotFound) {
	HeapPage page = Page();

	auto slot = page.Insert(Tuple(std::byte{29}, 2000));
	EXPECT_TRUE(slot.has_value());

	auto updated = page.Update(3, Tuple(std::byte{30}, 1800));
	EXPECT_FALSE(updated.has_value());
	EXPECT_EQ(updated.error().code(), ErrorCode::kNotFound);
}

// ---------------------------------------------------------------------------------------------
// Compact
// ---------------------------------------------------------------------------------------------

// The real proof: delete alternating tuples, compact, and every survivor still reads back its
// original bytes. That is what says the slot offsets were rewritten in lockstep with the bytes.
TEST_F(HeapPageTest, CompactReclaimsDeletedBytesAndKeepsSurvivingRidsReadable) {
	constexpr slot_id_t kTuples = 13;
	constexpr std::size_t kLength = 50;

	// A DISTINCT fill per tuple, and that is the whole test. With one shared fill every survivor
	// reads back 50 identical bytes no matter which survivor's bytes it actually got, so a
	// Compact that crossed two slots' offsets — wrote tuple 3 and pointed slot 7 at it — would
	// pass. Crossed offsets are exactly the bug this test exists to find.
	auto fill = [](slot_id_t i) { return static_cast<std::byte>(i + 1); };

	HeapPage page = Page();
	std::vector<slot_id_t> slots;
	for (slot_id_t i = 0; i < kTuples; ++i) {
		auto slot_id = page.Insert(Tuple(fill(i), kLength));
		ASSERT_TRUE(slot_id.has_value());
		slots.push_back(slot_id.value());
	}

	uint16_t dead_cnt{0};
	for (slot_id_t i = 0; i < kTuples; i += 2) {
		ASSERT_TRUE(page.Delete(slots[i]).ok());
		++dead_cnt;
	}

	ASSERT_EQ(page.Header().dead_bytes, dead_cnt * kLength);
	ASSERT_EQ(page.Header().slot_count - page.Header().live_count, dead_cnt);

	page.Compact();

	for (slot_id_t i = 0; i < kTuples; ++i) {
		if (i % 2) {
			auto tuple = page.Get(slots[i]);
			ASSERT_TRUE(tuple.has_value()) << "slot " << i << " should have survived";
			EXPECT_EQ(Read(tuple.value()), Tuple(fill(i), kLength)) << "slot " << i;
		} else {
			EXPECT_TRUE(page.SlotAt(slots[i]).IsDead()) << "slot " << i;
		}
	}

	// Bytes move, slots never renumber; and the region is packed, which dead_bytes == 0 alone
	// does not establish.
	EXPECT_EQ(page.SlotCount(), kTuples);
	EXPECT_EQ(page.LiveCount(), kTuples - dead_cnt);
	EXPECT_EQ(page.Header().dead_bytes, 0);
	EXPECT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - (kTuples - dead_cnt) * kLength);
	EXPECT_TRUE(page.CheckInvariants());
}

// tuple_data_start lands exactly at PAGE_BODY_SIZE minus the sum of the live lengths.
TEST_F(HeapPageTest, CompactPacksTheTupleRegionAgainstTheEndOfTheBody) {
	HeapPage page = Page();

	constexpr size_t N = 13;
	std::vector<slot_id_t> slots;
	for (std::size_t i = 0; i < N; i++) {
		auto slot = page.Insert(Tuple(std::byte{29}, 50));
		EXPECT_TRUE(slot.has_value());
		slots.push_back(slot.value());
	}

	uint16_t byte_cnt{0};
	for (std::size_t i = 0; i < N; i++) {
		if (i % 2) {
			byte_cnt += 50;
			continue;
		}
		auto st = page.Delete(slots[i]);
		EXPECT_EQ(st.code(), ErrorCode::kOk);
	}
	page.Compact();
	ASSERT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - byte_cnt);
}

// Bytes move, slots never renumber: slot_count and live_count come out unchanged and dead slots
// are still dead at their original indices.
TEST_F(HeapPageTest, CompactPreservesSlotCountAndLiveCount) {
	HeapPage page = Page();
	constexpr size_t N{13};

	for (size_t i = 0; i < N; i++) {
		auto slot = page.Insert(Tuple(std::byte{29}, 50));
		EXPECT_TRUE(slot.has_value());
	}

	for (size_t i = 0; i < N; i++) {
		if (i % 3) continue;
		auto st = page.Delete(static_cast<slot_id_t>(i));
		EXPECT_EQ(st.code(), ErrorCode::kOk);
	}

	uint16_t init_slot_cnt = page.Header().slot_count;
	uint16_t init_live_cnt = page.Header().live_count;

	page.Compact();

	ASSERT_EQ(init_slot_cnt, page.Header().slot_count);
	ASSERT_EQ(init_live_cnt, page.Header().live_count);
}

// dead_bytes == 0 means already packed, so this must be a no-op rather than a 4KB shuffle.
TEST_F(HeapPageTest, CompactOnAPackedPageChangesNothing) {
	constexpr slot_id_t kTuples = 5;
	constexpr std::size_t kLength = 200;

	HeapPage page = Page();
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(static_cast<std::byte>(i + 1), kLength)).has_value());
	}

	// The precondition the early-out rests on, and it is provable rather than incidental: the
	// accounting identity says tuple_data_start + live_bytes + dead_bytes == PAGE_BODY_SIZE, so
	// zero garbage means the live tuples exactly fill the region — and non-overlapping tuples
	// that exactly fill a region are already packed.
	ASSERT_EQ(page.Header().dead_bytes, 0);
	ASSERT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - kTuples * kLength);

	const std::array<std::byte, PAGE_BODY_SIZE> before = body_;

	page.Compact();

	// Byte-for-byte, which covers the header, the slot array and the tuple region in one shot.
	// NOTE: this proves the RESULT is unchanged, not that the early-out branch was taken — a
	// correct full compaction of an already-packed page produces exactly these bytes too. The
	// branch itself is not observable from out here; see the test's comment above.
	EXPECT_EQ(std::memcmp(before.data(), body_.data(), PAGE_BODY_SIZE), 0);

	EXPECT_EQ(page.SlotCount(), kTuples);
	EXPECT_EQ(page.LiveCount(), kTuples);
	EXPECT_EQ(page.Header().dead_bytes, 0);
	for (slot_id_t i = 0; i < kTuples; ++i) {
		EXPECT_EQ(Read(page.Get(i).value()), Tuple(static_cast<std::byte>(i + 1), kLength));
	}
	EXPECT_TRUE(page.CheckInvariants());
}

TEST_F(HeapPageTest, CompactIsIdempotent) {
	constexpr slot_id_t kTuples = 6;
	constexpr std::size_t kLength = 200;

	HeapPage page = Page();
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(static_cast<std::byte>(i + 1), kLength)).has_value());
	}
	for (slot_id_t i = 1; i < kTuples; i += 2) {
		ASSERT_TRUE(page.Delete(i).ok());
	}
	ASSERT_EQ(page.Header().dead_bytes, 3 * kLength);

	page.Compact();
	ASSERT_EQ(page.Header().dead_bytes, 0);
	ASSERT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - 3 * kLength);

	const std::array<std::byte, PAGE_BODY_SIZE> after_first = body_;

	page.Compact();

	// Byte-identical, which covers header, slot array and tuple region at once. This holds even
	// with the dead_bytes == 0 early-out removed: a compaction of a packed page repacks it into
	// the same offsets, so the second pass is a fixed point rather than merely a skipped call.
	EXPECT_EQ(std::memcmp(after_first.data(), body_.data(), PAGE_BODY_SIZE), 0);

	EXPECT_EQ(page.SlotCount(), kTuples);
	EXPECT_EQ(page.LiveCount(), 3);
	for (slot_id_t i = 0; i < kTuples; ++i) {
		if (i % 2 == 0) {
			EXPECT_EQ(Read(page.Get(i).value()), Tuple(static_cast<std::byte>(i + 1), kLength));
		} else {
			EXPECT_TRUE(page.SlotAt(i).IsDead());
		}
	}
	EXPECT_TRUE(page.CheckInvariants());
}

TEST_F(HeapPageTest, CompactOnAnAllDeadPageResetsTheLowWaterMark) {
	constexpr slot_id_t kTuples = 4;
	constexpr std::size_t kLength = 500;

	HeapPage page = Page();
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(static_cast<std::byte>(i + 1), kLength)).has_value());
	}
	for (slot_id_t i = 0; i < kTuples; ++i) {
		ASSERT_TRUE(page.Delete(i).ok());
	}
	ASSERT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - kTuples * kLength);

	page.Compact();

	// With no live tuples the loop copies nothing and the cursor never leaves its starting point,
	// so the region collapses to empty and the mark goes all the way back.
	EXPECT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE);
	EXPECT_EQ(page.Header().dead_bytes, 0);
	EXPECT_EQ(page.LiveCount(), 0);

	// Bytes move, slots never renumber — even when every one of them is dead and the page holds
	// nothing. The array stays at full length so a stale RID still has something to resolve to.
	EXPECT_EQ(page.SlotCount(), kTuples);
	for (slot_id_t i = 0; i < kTuples; ++i) {
		EXPECT_TRUE(page.SlotAt(i).IsDead());
		EXPECT_EQ(page.Get(i).error().code(), ErrorCode::kNotFound);
	}

	// The space is genuinely back: everything but the sub-header and the surviving slot array.
	EXPECT_EQ(page.Contiguous(), PAGE_BODY_SIZE - HEAP_SUB_HEADER_SIZE - kTuples * SLOT_SIZE);
	EXPECT_EQ(page.Reclaimable(), page.Contiguous());
	EXPECT_TRUE(page.CheckInvariants());
}

// The gap the other cases miss: a reused slot index has to survive a compaction underneath it.
// Insert, delete a middle tuple, insert again (reusing that slot), then compact, and check every
// live RID — the reused slot is the one whose offset is easiest to rewrite wrong.
TEST_F(HeapPageTest, CompactAfterSlotReuseKeepsEveryRidReadable) {
	HeapPage page = Page();
	ASSERT_TRUE(page.Insert(Tuple(std::byte{1}, 100)).has_value());   // slot 0
	ASSERT_TRUE(page.Insert(Tuple(std::byte{2}, 200)).has_value());   // slot 1, about to die
	ASSERT_TRUE(page.Insert(Tuple(std::byte{3}, 300)).has_value());   // slot 2
	ASSERT_TRUE(page.Delete(1).ok());

	// Reuses slot 1: the index is recycled but the tuple lands at the low-water mark, nowhere
	// near where the old one was, so slot 1's offset now points somewhere unrelated to its index.
	auto reused = page.Insert(Tuple(std::byte{4}, 250));
	ASSERT_TRUE(reused.has_value());
	ASSERT_EQ(*reused, 1);
	ASSERT_EQ(page.Header().dead_bytes, 200);  // the reuse reclaimed the ENTRY, not the bytes

	page.Compact();

	// The reused slot is the one whose offset is easiest to rewrite wrong, because its index no
	// longer matches its position in the tuple region at all.
	EXPECT_EQ(Read(page.Get(0).value()), Tuple(std::byte{1}, 100));
	EXPECT_EQ(Read(page.Get(1).value()), Tuple(std::byte{4}, 250));
	EXPECT_EQ(Read(page.Get(2).value()), Tuple(std::byte{3}, 300));

	EXPECT_EQ(page.SlotCount(), 3);
	EXPECT_EQ(page.LiveCount(), 3);
	EXPECT_EQ(page.Header().dead_bytes, 0);
	EXPECT_EQ(page.Header().tuple_data_start, PAGE_BODY_SIZE - (100 + 250 + 300));
	EXPECT_TRUE(page.CheckInvariants());
}

// The caller-driven contract end to end: Insert fails with PageFull, Reclaimable() says there is
// room, Compact, and the same Insert now succeeds.
TEST_F(HeapPageTest, InsertSucceedsAfterCompactWhenItPreviouslyFailed) {
	HeapPage page = Page();
	ASSERT_TRUE(page.Insert(Tuple(std::byte{1}, MAX_TUPLE_SIZE)).has_value());
	ASSERT_TRUE(page.Insert(Tuple(std::byte{2}, MAX_TUPLE_SIZE)).has_value());
	ASSERT_TRUE(page.Delete(0).ok());

	const auto wanted = Tuple(std::byte{9}, 1500);

	// The contract end to end: Insert refuses, Reclaimable() says the space is there, and the
	// caller — not Insert — decides to spend a compaction on it.
	auto refused = page.Insert(wanted);
	EXPECT_FALSE(refused.has_value());
	EXPECT_EQ(refused.error().code(), ErrorCode::kPageFull);
	ASSERT_LT(page.Contiguous(), wanted.size());
	ASSERT_GE(page.Reclaimable(), wanted.size());

	page.Compact();

	auto accepted = page.Insert(wanted);
	ASSERT_TRUE(accepted.has_value());
	EXPECT_EQ(*accepted, 0);  // the dead slot's index is recycled, not appended to
	EXPECT_EQ(Read(page.Get(*accepted).value()), wanted);
	EXPECT_EQ(Read(page.Get(1).value()), Tuple(std::byte{2}, MAX_TUPLE_SIZE));
	EXPECT_EQ(page.SlotCount(), 2);
	EXPECT_EQ(page.LiveCount(), 2);
	EXPECT_TRUE(page.CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// Corruption handling — hand CheckInvariants a page that is already wrong
// ---------------------------------------------------------------------------------------------

// It must return false, not read out of bounds. Scribble a slot_count far past the 1014 ceiling
// straight into the body and call it.
TEST_F(HeapPageTest, CheckInvariantsRejectsAnImpossibleSlotCount) {
	// 5000 slots would need 20008 bytes of array in a 4064-byte body. CheckInvariants must say
	// false rather than walk 5000 entries off the end of the page — it is the one function
	// designed to be handed garbage, so it may not index anything it has not first bounded.
	PokeHeader(HeapSubHeader{.slot_count = 5000,
	                         .tuple_data_start = static_cast<uint16_t>(PAGE_BODY_SIZE),
	                         .live_count = 0,
	                         .dead_bytes = 0});
	EXPECT_FALSE(View().CheckInvariants());

	// Its companion guard, which runs first and is what bounds slot_count in the first place: a
	// low-water mark past the end of the body.
	SetUp();
	PokeHeader(HeapSubHeader{
	    .slot_count = 0, .tuple_data_start = 5000, .live_count = 0, .dead_bytes = 0});
	EXPECT_FALSE(View().CheckInvariants());
}

TEST_F(HeapPageTest, CheckInvariantsRejectsALiveSlotPointingOutsideTheTupleRegion) {
	HeapPage page = Page();
	ASSERT_TRUE(page.Insert(Tuple(std::byte{1}, 300)).has_value());
	ASSERT_TRUE(page.CheckInvariants());

	const Slot good = page.SlotAt(0);

	// Below the low-water mark: the tuple claims bytes the page says are free space.
	PokeSlot(0, Slot{static_cast<uint16_t>(good.offset - 10), good.length});
	EXPECT_FALSE(View().CheckInvariants());

	// Past the end of the body. This is the check that stops Get from handing out a span running
	// off the page, which is why it is audited per slot here rather than on Get's hot path.
	PokeSlot(0, good);
	ASSERT_TRUE(page.CheckInvariants());
	PokeSlot(0, Slot{good.offset, static_cast<uint16_t>(PAGE_BODY_SIZE)});
	EXPECT_FALSE(View().CheckInvariants());
}

TEST_F(HeapPageTest, CheckInvariantsRejectsAMiscountedLiveCount) {
	HeapPage page = Page();
	for (slot_id_t i = 0; i < 3; ++i) {
		ASSERT_TRUE(page.Insert(Tuple(static_cast<std::byte>(i + 1), 100)).has_value());
	}
	ASSERT_TRUE(page.CheckInvariants());

	HeapSubHeader header = page.Header();
	const uint16_t truth = header.live_count;

	// Too low, which is what a Delete that forgot to decrement's counterpart looks like...
	header.live_count = static_cast<uint16_t>(truth - 1);
	PokeHeader(header);
	EXPECT_FALSE(View().CheckInvariants());

	// ...and too high, which is what an Update that killed a slot and never restored it looks
	// like. The field is maintained rather than derived, so only this cross-check catches drift.
	header.live_count = static_cast<uint16_t>(truth + 1);
	PokeHeader(header);
	EXPECT_FALSE(View().CheckInvariants());
}

// Dead has exactly one representation, and only because Delete zeroes both fields. That is what
// makes "offset == 0 implies length == 0" assertable at all, so it needs its own guard.
TEST_F(HeapPageTest, CheckInvariantsRejectsADeadSlotWithANonZeroLength) {
	HeapPage page = Page();
	ASSERT_TRUE(page.Insert(Tuple(std::byte{1}, 150)).has_value());
	ASSERT_TRUE(page.Delete(0).ok());
	ASSERT_TRUE(page.CheckInvariants());

	// A Delete that zeroed the offset but left the length behind — a dead slot in a second,
	// illegal representation that no other check would look at, since the walk skips dead slots.
	PokeSlot(0, Slot{0, 150});
	EXPECT_FALSE(View().CheckInvariants());
}

// Overlapping tuples fall out of the byte-accounting sum with no pairwise comparison.
TEST_F(HeapPageTest, CheckInvariantsRejectsOverlappingTuples) {
	HeapPage page = Page();
	ASSERT_TRUE(page.Insert(Tuple(std::byte{1}, 300)).has_value());
	ASSERT_TRUE(page.Insert(Tuple(std::byte{2}, 300)).has_value());
	ASSERT_TRUE(page.CheckInvariants());

	const uint16_t shared = page.SlotAt(0).offset;

	// Point both live slots at the same 300 bytes and pull the low-water mark up to match, which
	// is what a compaction that moved a tuple but rewrote the wrong slot would leave behind.
	PokeSlot(1, Slot{shared, 300});
	HeapSubHeader header = page.Header();
	header.tuple_data_start = shared;
	PokeHeader(header);

	// No pairwise comparison anywhere: the overlap counts the shared bytes twice, so the live
	// total overshoots the region and the accounting identity is the thing that notices.
	EXPECT_FALSE(View().CheckInvariants());
}

// ---------------------------------------------------------------------------------------------
// AsHeapPage — the one check a bare view cannot make
// ---------------------------------------------------------------------------------------------

// Needs a real guard, so this pair reaches for a BufferPoolManager where nothing else here does.
TEST_F(HeapPageTest, AsHeapPageRejectsAPageThatIsNotAHeapPage) {
	const auto path = std::filesystem::temp_directory_path() / "kernsql_asheap_reject_test";
	std::filesystem::remove(path);
	auto dm = DiskManager::Open(path);
	ASSERT_TRUE(dm.has_value()) << dm.error().message();
	{
		BufferPoolManager bpm(**dm, 4);
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			ASSERT_NE(guard->Header().page_type, PageType::HEAP);

			// A HeapPage sees only the body, so it CANNOT check page_type. Build one over a
			// non-heap page and it parses whatever is there as a sub-header and scribbles on it.
			// The guard can see the header, which is the entire reason this function exists.
			auto view = AsHeapPage(*guard);
			EXPECT_FALSE(view.has_value());
			EXPECT_EQ(view.error().code(), ErrorCode::kCorruption);
		}
		{
			auto guard = bpm.FetchPageRead(1);  // the catalog page, reserved by DiskManager
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto view = AsHeapPage(*guard);
			EXPECT_FALSE(view.has_value());
			EXPECT_EQ(view.error().code(), ErrorCode::kCorruption);
		}
		EXPECT_TRUE(bpm.Shutdown().ok());
	}
	dm->reset();
	std::filesystem::remove(path);
}

TEST_F(HeapPageTest, AsHeapPageAcceptsAHeapPage) {
	const auto path = std::filesystem::temp_directory_path() / "kernsql_asheap_accept_test";
	std::filesystem::remove(path);
	auto dm = DiskManager::Open(path);
	ASSERT_TRUE(dm.has_value()) << dm.error().message();
	{
		BufferPoolManager bpm(**dm, 4);
		page_id_t page_id{};
		{
			auto guard = bpm.NewPage();
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			page_id = guard->PageId();

			// The caller stamps the type through the guard; this layer cannot see the header.
			guard->SetPageType(PageType::HEAP);

			auto view = AsHeapPage(*guard);
			ASSERT_TRUE(view.has_value()) << view.error().message();

			// And the span it handed back really is this page's body, not an offset copy of it.
			view->Init();
			auto slot = view->Insert(Bytes("through a real guard"));
			ASSERT_TRUE(slot.has_value());
			EXPECT_EQ(AsChars(view->Get(*slot).value()), "through a real guard");
			EXPECT_TRUE(view->CheckInvariants());
		}
		{
			// The const overload, over the same page after the write guard is gone.
			auto guard = bpm.FetchPageRead(page_id);
			ASSERT_TRUE(guard.has_value()) << guard.error().message();
			auto view = AsHeapPage(*guard);
			ASSERT_TRUE(view.has_value()) << view.error().message();
			EXPECT_EQ(view->LiveCount(), 1);
			EXPECT_EQ(AsChars(view->Get(0).value()), "through a real guard");
		}
		EXPECT_TRUE(bpm.Shutdown().ok());
	}
	dm->reset();
	std::filesystem::remove(path);
}

}  // namespace kernsql

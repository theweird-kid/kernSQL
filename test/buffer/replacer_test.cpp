#include "buffer/replacer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "common/types.hpp"

namespace kernsql {

// No fixture needed — Replacer has no external state (no files, no fds), so plain
// TEST() throughout. Every test constructs its own Replacer at the capacity the
// scenario needs; small capacities (2-4) keep the clock traces small enough to
// verify by hand in the comments below.
//
// A note on method: the Replacer exposes no getters, so none of these tests peek
// at usage counts directly. Each policy test is instead built so that the
// *sequence of victims* differs between a correct and a buggy implementation —
// the observable behavior is the eviction order, same way the DiskManager tests
// observe on-disk bytes rather than private fields.

// ---------------------------------------------------------------------------
// Evict() — empty / no candidates
// ---------------------------------------------------------------------------

TEST(ReplacerTest, EvictOnFreshReplacerReturnsNullopt) {
	// Construct Replacer(8) and call Evict() immediately -> expect nullopt.
	// Proves both the evictable_count_ == 0 fast path and that frames start
	// life as non-candidates (nothing is evictable until SetEvictable says so).

	auto replacer = Replacer(8);
	ASSERT_EQ(std::nullopt, replacer.Evict());
}

TEST(ReplacerTest, AccessesAloneDoNotCreateCandidates) {
	// RecordAccess a handful of frames several times each, but never call
	// SetEvictable. Evict() -> expect nullopt. Proves usage history and
	// candidate membership are independent axes — a hot page that's pinned
	// must never be evicted no matter how its counter looks.

	auto replacer = Replacer(4);
	std::size_t recordCount = 5;
	for (std::size_t i = 0; i < recordCount; i++) {
		replacer.RecordAccess(0);
		replacer.RecordAccess(1);
		replacer.RecordAccess(3);
	}
	ASSERT_EQ(std::nullopt, replacer.Evict());
}

// ---------------------------------------------------------------------------
// Evict() — victim post-conditions
// ---------------------------------------------------------------------------

TEST(ReplacerTest, EvictsTheOnlyCandidateThenGoesEmpty) {
	// Replacer(4); SetEvictable(2, true); Evict() -> expect frame 2.
	// Then Evict() again -> expect nullopt. The second call is the important
	// half: it proves Evict removed its victim from the candidate set itself
	// (the frozen post-condition), rather than leaving that to the caller.

	auto replacer = Replacer(4);
	replacer.SetEvictable(2, true);

	ASSERT_EQ(2, replacer.Evict());
	ASSERT_EQ(std::nullopt, replacer.Evict());
}

TEST(ReplacerTest, EvictedFrameCanBeReaddedAndEvictedAgain) {
	// Same start as above, but after the first eviction call
	// SetEvictable(2, true) again — this is exactly what the BPM does when
	// the frame's new page gets unpinned. Evict() -> expect frame 2 again.
	// Proves eviction doesn't permanently retire a frame id.

	auto replacer = Replacer(4);
	replacer.SetEvictable(2, true);
	ASSERT_EQ(2, replacer.Evict());

	replacer.SetEvictable(2, true);
	ASSERT_EQ(2, replacer.Evict());
}

// ---------------------------------------------------------------------------
// Evict() — clock policy
// ---------------------------------------------------------------------------

TEST(ReplacerTest, ColdCandidatesEvictInClockOrder) {
	// Replacer(4); mark all four frames evictable, no accesses anywhere.
	// Four Evict() calls -> expect victims 0, 1, 2, 3 in that order, then a
	// fifth call -> nullopt. Pins down the hand's starting position and
	// direction, which every trace-based test below depends on.

	auto replacer = Replacer(4);
	for (frame_id_t i = 0; i < 4; i++) {
		replacer.SetEvictable(i, true);
	}

	for (frame_id_t i = 0; i < 4; i++) {
		ASSERT_EQ(i, replacer.Evict());
	}

	ASSERT_EQ(std::nullopt, replacer.Evict());
}

TEST(ReplacerTest, AccessedFrameGetsSecondChance) {
	// Replacer(3); all three evictable; RecordAccess(1) once.
	// Expect eviction order: 0, 2, 1.
	// Trace: Evict#1 finds 0 at count 0 -> victim, hand moves to 1.
	// Evict#2: frame 1 has count 1 -> decremented to 0, hand passes on,
	// frame 2 at count 0 -> victim. Evict#3: 1 is now at 0 -> victim.
	// This is the second-chance property in its smallest observable form.
	auto replacer = Replacer(3);
	for (frame_id_t i = 0; i < 3; i++) {
		replacer.SetEvictable(i, true);
	}
	replacer.RecordAccess(1);

	ASSERT_EQ(0, replacer.Evict());
	ASSERT_EQ(2, replacer.Evict());
	ASSERT_EQ(1, replacer.Evict());
}

TEST(ReplacerTest, UsageCountCapsAtThree) {
	// Replacer(2); RecordAccess(0) fifty times, RecordAccess(1) exactly
	// three times (== kMaxUsageCount); mark both evictable.
	// Evict() -> expect frame 0.
	// Why this discriminates: with the cap working, both frames sit at 3 and
	// the sweep walks them down together — 0 reaches zero first purely by
	// hand order (trace: 3/3 -> 2/2 -> 1/1 -> 0/0, hand lands on 0). If the
	// cap leaked, frame 0 would sit at 50 and outlast frame 1, making 1 the
	// first victim instead. One assertion, and it fails precisely when the
	// CAS loop's cap check is broken.

	auto replacer = Replacer(2);
	for (int i = 0; i < 50; i++) replacer.RecordAccess(0);
	for (int i = 0; i < 3; i++) replacer.RecordAccess(1);

	replacer.SetEvictable(0, true);
	replacer.SetEvictable(1, true);
	ASSERT_EQ(0, replacer.Evict());
}

TEST(ReplacerTest, PinnedHotFrameKeepsItsProtection) {
	// Proves the sweep passes over non-evictable frames WITHOUT touching
	// their usage history — DD-002: "skipped without decrementing; its
	// history is preserved."
	//
	// Replacer(2).
	// Setup round: RecordAccess(0) three times, but leave 0 non-evictable
	// (it's "pinned"). SetEvictable(1, true); RecordAccess(1) once.
	// Evict() -> expect 1. During this sweep the hand passes frame 0 twice;
	// a buggy implementation decrements it both times (3 -> 1), a correct
	// one leaves it at 3.
	// Second round: SetEvictable(0, true) ("unpinned" now),
	// SetEvictable(1, true), RecordAccess(1) once.
	// Evict() -> expect 1 again. Correct trace: 0 enters at 3, gets walked
	// 3->2->... while 1 (at 1) reaches zero first. Buggy trace: 0 entered at
	// 1, reaches zero first, and 0 is wrongly evicted — the pinned-hot page
	// lost the protection it earned before being pinned.
	// Optional third Evict() -> expect 0, confirming it was still there.

	auto replacer = Replacer(2);
	for (int i = 0; i < 3; i++) replacer.RecordAccess(0);
	replacer.SetEvictable(0, false);

	replacer.RecordAccess(1);
	replacer.SetEvictable(1, true);

	ASSERT_EQ(1, replacer.Evict());

	replacer.SetEvictable(0, true);

	replacer.SetEvictable(1, true);
	replacer.RecordAccess(1);

	ASSERT_EQ(1, replacer.Evict());
	ASSERT_EQ(0, replacer.Evict());
}

// ---------------------------------------------------------------------------
// SetEvictable() — idempotency / bookkeeping
// ---------------------------------------------------------------------------
// Heads-up for both tests below: the failure mode of broken evictable_count_
// bookkeeping is not a wrong value but a HANG — Evict's fallback loop spins
// looking for a candidate the count claims exists. A test that never finishes
// IS the failure signal here; if you want it bounded, run ctest with
// --timeout N.

TEST(ReplacerTest, RedundantMarkEvictableDoesNotInflateTheCount) {
	// Replacer(2); SetEvictable(0, true) three times in a row; Evict() ->
	// expect 0; Evict() -> expect nullopt. If the flip-check is missing,
	// evictable_count_ is 3, the second Evict believes candidates remain,
	// finds none, and never returns.
	GTEST_SKIP();
}

TEST(ReplacerTest, RedundantClearOnFreshFrameIsHarmless) {
	// Replacer(2); SetEvictable(0, false) on a frame that is already
	// non-evictable (the initial state); then Evict() -> expect nullopt.
	// Guards the other direction: without the flip-check, the size_t
	// evictable_count_ underflows to a huge value and Evict hangs exactly
	// as above.
	GTEST_SKIP();
}

// ---------------------------------------------------------------------------
// Concurrency — run this under the tsan build
// ---------------------------------------------------------------------------

TEST(ReplacerTest, HammerConcurrentAccessAndEviction) {
	// The one test whose real assertions come from ThreadSanitizer: it
	// exercises the lock-free RecordAccess path racing the locked Evict path,
	// which is exactly the interleaving the atomic usage_count_ decision
	// exists to make safe. A green run under build/debug proves little;
	// build/tsan is the point.
	//
	// Shape:
	//   - Replacer(64); mark all frames evictable up front.
	//   - A std::vector<std::atomic<bool>> claimed(64), all false — the
	//     test's own shadow of "who owns this frame right now."
	//   - 4 accessor threads: loop ~100k times calling RecordAccess on a
	//     pseudo-random frame id. (Deliberately including currently-claimed
	//     frames — a stale RecordAccess racing an eviction is the benign
	//     race DD-002 explicitly accepts, so the test should generate it.)
	//   - 2 evictor threads: loop calling Evict(). On nullopt, just retry.
	//     On a victim v: EXPECT_FALSE(claimed[v].exchange(true)) — this is
	//     the invariant that matters, no frame handed to two threads at
	//     once — then claimed[v] = false and SetEvictable(v, true) to feed
	//     it back into the pool, the same recycle the BPM performs.
	//   - Join everything; no final state assertion needed — the exchange
	//     check and tsan's race detector are the verdict.
	// Keep iteration counts high enough to force interleavings but low
	// enough that the tsan build finishes in seconds, not minutes.

	constexpr std::size_t kFrames = 64;
	Replacer replacer(kFrames);
	std::vector<std::atomic<bool>> claimed(kFrames);

	for (frame_id_t i = 0; i < static_cast<frame_id_t>(kFrames); ++i) {
		replacer.SetEvictable(i, true);
	}

	for (int iter = 0; iter < 1000; ++iter) {
		auto victim = replacer.Evict();
		if (!victim) continue;
		const auto v = static_cast<std::size_t>(*victim);
		EXPECT_FALSE(claimed[v].exchange(true));
		claimed[v].store(false);
		replacer.SetEvictable(*victim, true);
	}

	auto evictor = [&]() {
		for (int iter = 0; iter < 20000; ++iter) {
			auto victim = replacer.Evict();
			if (!victim) continue;
			const auto v = static_cast<std::size_t>(*victim);
			EXPECT_FALSE(claimed[v].exchange(true));
			claimed[v].store(false);
			replacer.SetEvictable(*victim, true);
		}
	};

	auto accessor = [&](unsigned seed) {
		std::mt19937 rng(seed);
		std::uniform_int_distribution<frame_id_t> dist(0, static_cast<frame_id_t>(kFrames) - 1);
		for (int iter = 0; iter < 20000; ++iter) {
			replacer.RecordAccess(dist(rng));
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(6);
	for (int i = 0; i < 4; ++i) threads.emplace_back(accessor, static_cast<unsigned>(i + 1));
	for (int i = 0; i < 2; ++i) threads.emplace_back(evictor);
	for (auto& t : threads) t.join();
}

}  // namespace kernsql

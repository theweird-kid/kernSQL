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
 * Slotted heap page (DD-004). A heap page is an unordered pile of tuples addressed by
 * RID{page_id, slot}, laid out inside the 4064-byte body a page guard hands out:
 *
 *   body offset 0
 *   +---------------------------+
 *   | heap sub-header (8 bytes) |
 *   +---------------------------+
 *   | slot[0] slot[1] ...       |  grows FORWARD, 4 bytes each
 *   +---------------------------+
 *   |        free space         |
 *   +---------------------------+
 *   | ... tuple tuple tuple     |  grows BACKWARD from the end
 *   +---------------------------+
 *   body offset 4064
 *
 * THE RULE EVERYTHING FOLLOWS: slots never move, tuple bytes move freely. A RID names a slot
 * index and the slot names an offset, so compaction can rewrite every byte of the tuple region
 * and every RID in the database stays valid. The corollary is the thing to keep testing: any
 * operation that moves tuple bytes must update the moved tuples' slot offsets in the same
 * latched operation.
 *
 * All offsets here are BODY-RELATIVE — offset 0 is the first byte after the 32-byte PageHeader,
 * which this layer cannot see. That is deliberate (see WritePageGuard::MutableBody) and it is
 * why page_type validation lives in AsHeapPage below rather than in these classes.
 */

inline constexpr std::size_t HEAP_SUB_HEADER_SIZE = 8;
inline constexpr std::size_t SLOT_SIZE = 4;

// Largest tuple the format can physically hold: the whole body, less the sub-header and the one
// slot that tuple needs. Not the enforced limit — see MAX_TUPLE_SIZE.
inline constexpr std::size_t MAX_TUPLE_ON_EMPTY_PAGE =
    PAGE_BODY_SIZE - HEAP_SUB_HEADER_SIZE - SLOT_SIZE;

// The enforced cap, checked at CREATE TABLE against the widest row a schema can produce and
// again at INSERT against the actual encoded size. 2000 rather than the 4052 ceiling because
// 2000 guarantees at least two rows per page (InnoDB's rule): a format that allows exactly one
// row per page degenerates into a linked list with 50% overhead.
inline constexpr std::size_t MAX_TUPLE_SIZE = 2000;

static_assert(2 * (MAX_TUPLE_SIZE + SLOT_SIZE) + HEAP_SUB_HEADER_SIZE <= PAGE_BODY_SIZE,
              "MAX_TUPLE_SIZE must guarantee two tuples per page");

/*
 * The 8 bytes at body offset 0. Serialized through ReadFrom/WriteTo rather than mapped with a
 * reinterpret_cast over the body, matching PageHeader: same host-endian memcpy, same
 * static_assert block, and it sidesteps the object-lifetime question a cast would raise.
 *
 * Three of these four fields have direct InnoDB counterparts — PAGE_N_HEAP, PAGE_HEAP_TOP,
 * PAGE_N_RECS, PAGE_GARBAGE — which is a reasonable sign the layout is not novel.
 */
struct HeapSubHeader {
	// Entries in the slot array, live and dead alike. Only ever grows within a page's life as
	// a heap page; a dead slot keeps its index forever so a stale RID has something to resolve
	// against.
	uint16_t slot_count{0};

	// Body offset of the lowest occupied tuple byte. The tuple region is
	// [tuple_data_start, PAGE_BODY_SIZE). Initialised to PAGE_BODY_SIZE for an empty page.
	uint16_t tuple_data_start{static_cast<uint16_t>(PAGE_BODY_SIZE)};

	// Non-dead slots. Maintained rather than derived so a scan can skip an empty page without
	// walking its slot array.
	uint16_t live_count{0};

	// Bytes in the tuple region that no live slot points at: deleted tuples, plus the waste
	// left behind by an in-place shrinking update.
	//
	// CANNOT BE DERIVED, which is the entire reason it is stored. Update case 1 overwrites a
	// slot's length with the smaller value, so the bytes it orphans are referenced by no slot
	// and no offset and are invisible to any later slot walk. Without this counter a page that
	// has taken many in-place shrinks looks full while being mostly holes, and nothing ever
	// learns there is space to recover.
	//
	// Only ever increases, or is reset to zero by Compact. There is no subtraction anywhere, so
	// there is no path that can drive it negative; and it is bounded by MAX_TUPLE_ON_EMPTY_PAGE,
	// so it cannot overflow its uint16_t.
	uint16_t dead_bytes{0};

	static HeapSubHeader ReadFrom(std::span<const std::byte, HEAP_SUB_HEADER_SIZE> bytes) {
		HeapSubHeader h;
		std::memcpy(&h, bytes.data(), sizeof(HeapSubHeader));
		return h;
	}
	void WriteTo(std::span<std::byte, HEAP_SUB_HEADER_SIZE> bytes) const {
		std::memcpy(bytes.data(), this, sizeof(HeapSubHeader));
	}
};

static_assert(sizeof(HeapSubHeader) == HEAP_SUB_HEADER_SIZE, "heap sub-header must be 8 bytes");
static_assert(alignof(HeapSubHeader) == 2, "four uint16_t must pack with no implicit padding");
static_assert(std::is_standard_layout_v<HeapSubHeader>);
static_assert(std::is_trivially_copyable_v<HeapSubHeader>);

/*
 * One slot directory entry. A slot is in exactly one of two states — live (offset != 0) or dead
 * (offset == 0). There is no third "never used" state: a slot is created live by an insert and
 * a delete makes it dead.
 *
 * offset == 0 is a safe sentinel rather than a magic number, because body offset 0 is inside
 * the sub-header and no tuple can legitimately start there. Postgres spends a flags bitfield on
 * the same distinction; the impossible offset gets it for free.
 */
struct Slot {
	uint16_t offset{0};  // body offset of the tuple's first byte; 0 means dead
	uint16_t length{0};  // tuple length in bytes

	[[nodiscard]] bool IsDead() const { return offset == 0; }

	static Slot ReadFrom(std::span<const std::byte, SLOT_SIZE> bytes) {
		Slot s;
		std::memcpy(&s, bytes.data(), sizeof(Slot));
		return s;
	}
	void WriteTo(std::span<std::byte, SLOT_SIZE> bytes) const {
		std::memcpy(bytes.data(), this, sizeof(Slot));
	}
};

static_assert(sizeof(Slot) == SLOT_SIZE, "slot must be 4 bytes");
static_assert(std::is_standard_layout_v<Slot>);
static_assert(std::is_trivially_copyable_v<Slot>);

// What HeapPage::Update did, for the caller that has to decide whether the RID survived.
enum class UpdateOutcome : uint8_t {
	// Overwritten in place, or relocated within this page after a compaction. Either way the
	// slot index did not change, so THE RID SURVIVES.
	kSamePage,

	// Does not fit even after compacting this page. HeapPage does nothing; TableHeap must
	// delete here and insert elsewhere, and THE RID CHANGES.
	kDoesNotFit,
};

/*
 * Read-only view over a heap page body. Non-owning: it holds a span, not a guard, not a pin,
 * and not a latch.
 *
 * LIFETIME. A view must never outlive the guard its span came from. Past that point the frame
 * is unpinned and may have been evicted and refilled with a different page — the memory is
 * still valid, so ASan and valgrind catch nothing, and reads silently return another page's
 * bytes. Two rules contain it:
 *
 *   1. A view is a function local, born from a guard in the same scope. Never a member of
 *      anything. TableIterator holds the GUARD as a member and mints the view per access;
 *      caching both means two members that must be replaced in lockstep, and the failure mode
 *      when they are not is silent.
 *   2. No default constructor, so an unbound view cannot exist. Same reasoning as the guards.
 */
class ConstHeapPage {
  public:
	ConstHeapPage() = delete;

	// Public because the point of a view is that tests construct one over a bare
	// std::array<std::byte, PAGE_BODY_SIZE> with no BufferPoolManager in existence. Prefer
	// AsHeapPage() in non-test code: it is the only place page_type can be checked.
	explicit ConstHeapPage(std::span<const std::byte, PAGE_BODY_SIZE> body) : body_(body) {}

	[[nodiscard]] HeapSubHeader Header() const;
	[[nodiscard]] Slot SlotAt(slot_id_t slot) const;

	[[nodiscard]] uint16_t SlotCount() const { return Header().slot_count; }
	[[nodiscard]] uint16_t LiveCount() const { return Header().live_count; }

	// Free space usable RIGHT NOW, between the end of the slot array and the tuple region.
	[[nodiscard]] std::size_t Contiguous() const;

	// Free space usable AFTER a Compact: Contiguous() + dead_bytes. This is the number a
	// cross-page free-space mechanism publishes, and the number to compare against before
	// deciding a compaction is worth it.
	[[nodiscard]] std::size_t Reclaimable() const;

	// Bytes of the tuple at `slot`. NotFound for an out-of-range or dead slot: a Get on a
	// deleted RID returns "not found", never bytes.
	[[nodiscard]] Result<std::span<const std::byte>> Get(slot_id_t slot) const;

	/*
	 * Debug-only total accounting check. Every byte of the tuple region belongs to exactly one
	 * live tuple or to nobody, which fixes an identity that must hold after EVERY operation:
	 *
	 *   tuple_data_start + sum(slot.length for live slots) + dead_bytes == PAGE_BODY_SIZE
	 *
	 * Call it at the end of every mutating operation in test builds. It catches the whole class
	 * of accounting bug this layer can have — a missed increment in Update case 1, a dead_bytes
	 * wrongly reduced by a dead-slot reuse, a Compact that forgot to reset — at the operation
	 * that caused it rather than a thousand operations later. Also checks
	 * live_count == count of live slots, and slot_array_end <= tuple_data_start.
	 *
	 * O(slot_count). Not for the hot path.
	 */
	[[nodiscard]] bool CheckInvariants() const;

  private:
	std::span<const std::byte, PAGE_BODY_SIZE> body_;
};

/*
 * Mutable view. Same lifetime rules as ConstHeapPage.
 *
 * Two types rather than one templated on constness, mirroring T* -> const T*: HeapPage converts
 * to ConstHeapPage, so the read operations are written once. The read/write split is carried by
 * the span's type, which is the same mechanism the guards use — it is the type system, not a
 * runtime mode check, that stops a reader writing.
 */
class HeapPage {
  public:
	HeapPage() = delete;

	explicit HeapPage(std::span<std::byte, PAGE_BODY_SIZE> body) : body_(body) {}

	// NOLINTNEXTLINE(google-explicit-constructor) — deliberate, this is the T* -> const T* edge.
	operator ConstHeapPage() const { return ConstHeapPage(body_); }

	[[nodiscard]] ConstHeapPage View() const { return ConstHeapPage(body_); }

	// Read surface, forwarded so a caller holding a write guard is not forced through View().
	[[nodiscard]] HeapSubHeader Header() const { return View().Header(); }
	[[nodiscard]] Slot SlotAt(slot_id_t slot) const { return View().SlotAt(slot); }
	[[nodiscard]] uint16_t SlotCount() const { return View().SlotCount(); }
	[[nodiscard]] uint16_t LiveCount() const { return View().LiveCount(); }
	[[nodiscard]] std::size_t Contiguous() const { return View().Contiguous(); }
	[[nodiscard]] std::size_t Reclaimable() const { return View().Reclaimable(); }
	[[nodiscard]] Result<std::span<const std::byte>> Get(slot_id_t slot) const {
		return View().Get(slot);
	}
	[[nodiscard]] bool CheckInvariants() const { return View().CheckInvariants(); }

	// Zero the sub-header: slot_count = 0, live_count = 0, dead_bytes = 0,
	// tuple_data_start = PAGE_BODY_SIZE. The CALLER stamps page_type = HEAP through the guard;
	// this layer cannot see the header.
	void Init();

	/*
	 * Reuse a dead slot if one exists, else append a new one. Copies the tuple to
	 * tuple_data_start - len and lowers tuple_data_start.
	 *
	 * Rejects an empty tuple and anything over MAX_TUPLE_SIZE. The cap is checked here as well as
	 * at CREATE TABLE and at SQL INSERT, so that no path above this layer can put an oversized
	 * tuple on a page: the format's own limit is MAX_TUPLE_ON_EMPTY_PAGE, and a tuple between the
	 * two would insert fine and then be unrelocatable for the rest of its life.
	 *
	 * Needs len + SLOT_SIZE if it must append a slot, or len alone if it reuses a dead one,
	 * checked against Contiguous(). DOES NOT COMPACT: returns failure and lets the caller
	 * decide between compacting and retrying, or moving to another page.
	 *
	 * REUSING A DEAD SLOT DOES NOT REDUCE dead_bytes. It reclaims the four bytes of the slot,
	 * not the tuple bytes: Delete destroyed the old offset, so those bytes cannot be found, let
	 * alone reused. Only Compact reclaims them. This reads backwards and is the single most
	 * likely place to get the accounting wrong.
	 */
	[[nodiscard]] Result<slot_id_t> Insert(std::span<const std::byte> tuple);

	/*
	 * Add the slot's length to dead_bytes BEFORE overwriting the slot, then set it to
	 * {offset: 0, length: 0} and decrement live_count. Zeroing both fields gives a dead slot one
	 * canonical representation, which is what makes `offset == 0 implies length == 0`
	 * assertable in CheckInvariants.
	 *
	 * The tuple's bytes are not moved; the space is reclaimed at the next compaction.
	 */
	[[nodiscard]] Status Delete(slot_id_t slot);

	/*
	 * Three cases:
	 *   1. new length <= old: overwrite in place, shrink length, and ADD THE DIFFERENCE TO
	 *      dead_bytes. Those orphaned bytes are the ones no slot walk can ever find again.
	 *   2. new length > old but the page has room after compaction: kill the old tuple first
	 *      (add its length to dead_bytes, zero the slot), THEN compact, then insert into the
	 *      same slot id. Killing before compacting is what stops the compaction from copying
	 *      bytes that are about to be abandoned. The slot is momentarily dead while still
	 *      logically live; that is safe only because the whole thing runs under one write latch,
	 *      and it is why Compact must never be callable in the middle of this from outside.
	 *   Cases 1 and 2 both return kSamePage — the slot index did not change, so the RID lives.
	 *   3. does not fit even after compaction: change nothing, return kDoesNotFit. TableHeap
	 *      deletes here and inserts elsewhere, and the RID changes.
	 */
	[[nodiscard]] Result<UpdateOutcome> Update(slot_id_t slot, std::span<const std::byte> tuple);

	/*
	 * Rewrite the tuple region packed against the end of the body, in descending offset order,
	 * updating each live slot's offset as its tuple moves. Dead slots keep their index and stay
	 * dead. Afterwards tuple_data_start is the new low-water mark and dead_bytes == 0 — by
	 * definition, since every byte below the low-water mark is now live.
	 *
	 * The only operation that reduces dead_bytes.
	 */
	void Compact();

  private:
	void WriteHeader(const HeapSubHeader& header);
	void WriteSlot(slot_id_t slot, const Slot& entry);

	std::span<std::byte, PAGE_BODY_SIZE> body_;
};

/*
 * The only way to get a view in non-test code, and the reason it is a free function in the heap
 * layer rather than a method on the guard: the dependency has to point heap -> buffer, or the
 * buffer pool ends up knowing what a heap page is.
 *
 * These exist for one check. A HeapPage sees only the body, so it CANNOT verify
 * page_type == HEAP; build one over a catalog or index page and it will parse garbage as a
 * sub-header and scribble on it, and nothing notices until that page fails validation on some
 * later miss. The guard can see the header, so the assert lives here. Corruption on mismatch.
 */
[[nodiscard]] Result<ConstHeapPage> AsHeapPage(const ReadPageGuard& guard);
[[nodiscard]] Result<HeapPage> AsHeapPage(WritePageGuard& guard);

}  // namespace kernsql

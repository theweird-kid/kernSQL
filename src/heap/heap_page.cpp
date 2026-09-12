#include "heap/heap_page.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

#include "common/logger.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

namespace kernsql {

namespace {

// Body offset of a slot entry. Slot 0 sits immediately after the sub-header and the array grows
// forward from there; the slot's INDEX is what a RID names, which is why this is pure
// arithmetic and never a search.
constexpr std::size_t SlotOffset(slot_id_t slot) {
	return HEAP_SUB_HEADER_SIZE + static_cast<std::size_t>(slot) * SLOT_SIZE;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Byte marshalling. Everything below this line is plumbing: it moves bytes in and out of the
// body and does no reasoning about them.
// ---------------------------------------------------------------------------------------------

HeapSubHeader ConstHeapPage::Header() const {
	return HeapSubHeader::ReadFrom(body_.first<HEAP_SUB_HEADER_SIZE>());
}

Slot ConstHeapPage::SlotAt(slot_id_t slot) const {
	return Slot::ReadFrom(
	    std::span<const std::byte, SLOT_SIZE>{body_.subspan(SlotOffset(slot), SLOT_SIZE)});
}

void HeapPage::WriteHeader(const HeapSubHeader& header) {
	header.WriteTo(body_.first<HEAP_SUB_HEADER_SIZE>());
}

void HeapPage::WriteSlot(slot_id_t slot, const Slot& entry) {
	entry.WriteTo(std::span<std::byte, SLOT_SIZE>{body_.subspan(SlotOffset(slot), SLOT_SIZE)});
}

Result<ConstHeapPage> AsHeapPage(const ReadPageGuard& guard) {
	if (guard.Header().page_type != PageType::HEAP) {
		return std::unexpected(Status::Corruption("page is not a heap page"));
	}
	return ConstHeapPage(guard.Body());
}

Result<HeapPage> AsHeapPage(WritePageGuard& guard) {
	if (guard.Header().page_type != PageType::HEAP) {
		return std::unexpected(Status::Corruption("page is not a heap page"));
	}
	return HeapPage(guard.MutableBody());
}

std::size_t ConstHeapPage::Contiguous() const {
	const auto h = Header();
	const std::size_t slot_array_end = HEAP_SUB_HEADER_SIZE + std::size_t{h.slot_count} * SLOT_SIZE;

	assert(h.tuple_data_start >= slot_array_end);       // loud in debug
	if (h.tuple_data_start < slot_array_end) return 0;  // safe in release
	return h.tuple_data_start - slot_array_end;
}

std::size_t ConstHeapPage::Reclaimable() const {
	return Contiguous() + Header().dead_bytes;
}

Result<std::span<const std::byte>> ConstHeapPage::Get(slot_id_t slot_id) const {
	if (Header().slot_count <= slot_id)
		return std::unexpected(Status::NotFound("slot doesn't exist"));
	auto slot = SlotAt(slot_id);
	if (slot.IsDead()) return std::unexpected(Status::NotFound("slot is dead"));

	const std::size_t tuple_end = slot.offset + slot.length;
	assert(tuple_end <= body_.size());
	if (tuple_end <= body_.size()) return body_.subspan(slot.offset, slot.length);
	return std::unexpected(Status::Corruption("tuple not accessible"));
}

bool ConstHeapPage::CheckInvariants() const {
	const auto h = Header();

	// Every failure names the invariant it broke. `assert(CheckInvariants())` on its own tells
	// you a page is corrupt and nothing whatsoever about how, which is the least useful moment
	// to be told that. LOG_DEBUG compiles to nothing under NDEBUG, which is also the only build
	// this function is meant to run in. [[maybe_unused]] because of that: in a release build the
	// parameter genuinely is unused, and -Wextra would say so.
	auto fail = []([[maybe_unused]] const char* what) {
		LOG_DEBUG("heap page invariant violated: %s", what);
		return false;
	};

	const std::size_t slot_array_end = HEAP_SUB_HEADER_SIZE + std::size_t{h.slot_count} * SLOT_SIZE;

	// These two run FIRST, and the order is load-bearing rather than stylistic. This function is
	// the one place designed to be handed a page that is already garbage, so it may not index
	// anything it has not first proved is in range. Together they bound slot_count at
	// (PAGE_BODY_SIZE - HEAP_SUB_HEADER_SIZE) / SLOT_SIZE == 1014, which is what makes both the
	// SlotAt() calls below and the loop counter safe on a corrupt page.
	if (h.tuple_data_start > PAGE_BODY_SIZE)
		return fail("tuple_data_start is past the end of the body");
	if (slot_array_end > h.tuple_data_start)
		return fail("slot array has run into the tuple region");

	std::size_t live_slots = 0;
	std::size_t live_bytes = 0;

	for (slot_id_t i = 0; i < h.slot_count; ++i) {
		const Slot s = SlotAt(i);

		if (s.IsDead()) {
			// Dead has exactly one representation, which is the only reason this is assertable:
			// Delete zeroes both fields rather than just the offset.
			if (s.length != 0) return fail("dead slot has a non-zero length");
			continue;
		}

		// A live tuple lies wholly inside the tuple region. This is the thorough per-slot audit
		// Get cannot afford on the hot path, which is why it lives here instead.
		if (s.offset < h.tuple_data_start) return fail("live slot points below tuple_data_start");
		if (std::size_t{s.offset} + s.length > PAGE_BODY_SIZE)
			return fail("live slot extends past the end of the body");

		++live_slots;
		live_bytes += s.length;
	}

	if (live_slots != h.live_count) return fail("live_count disagrees with the slot array");

	// The whole point. Every byte of the tuple region belongs to exactly one live tuple or to
	// nobody, so the region's size must be exactly accounted for. Overlapping tuples fall out of
	// this for free, with no pairwise comparison: an overlap counts the shared bytes twice, so
	// the sum overshoots the region.
	if (h.tuple_data_start + live_bytes + h.dead_bytes != PAGE_BODY_SIZE)
		return fail("tuple region accounting does not add up");

	return true;
}

void HeapPage::Init() {
	// Value-initialised so every field comes from its default member initializer, which is the
	// only way tuple_data_start starts at PAGE_BODY_SIZE. Spelling the zeros out instead OVERRIDES
	// those initializers and leaves the low-water mark at 0, i.e. a page that reports no free
	// space and fails its own accounting check the moment it is created.
	WriteHeader(HeapSubHeader{});
}

Result<slot_id_t> HeapPage::Insert(std::span<const std::byte> tuple) {
	if (tuple.empty()) {
		return std::unexpected(Status::InvalidArgument("cannot insert a zero-length tuple"));
	}

	auto header = Header();
	slot_id_t slot_id = header.slot_count;
	std::size_t required = tuple.size() + SLOT_SIZE;  // appending must pay for a new slot entry

	if (header.live_count < header.slot_count) {
		for (slot_id_t i = 0; i < header.slot_count; ++i) {
			if (SlotAt(i).IsDead()) {
				slot_id = i;
				required = tuple.size();  // the slot entry already exists; only bytes are needed
				break;
			}
		}
	}

	// DOES NOT COMPACT, by contract. The caller compares its own requirement against
	// Reclaimable() and decides between compacting this page and retrying, or another page.
	if (required > Contiguous()) {
		return std::unexpected(Status::PageFull("not enough contiguous space for the tuple"));
	}

	const auto length = static_cast<uint16_t>(tuple.size());
	const auto offset = static_cast<uint16_t>(header.tuple_data_start - length);

	std::memcpy(body_.data() + offset, tuple.data(), tuple.size());
	WriteSlot(slot_id, Slot{offset, length});

	header.tuple_data_start = offset;
	header.live_count = static_cast<uint16_t>(header.live_count + 1);
	if (slot_id == header.slot_count) {
		header.slot_count = static_cast<uint16_t>(header.slot_count + 1);
	}

	// dead_bytes is deliberately untouched, and this reads backwards every single time. Reusing
	// a dead slot reclaims the four bytes of the slot ENTRY, not the dead tuple's bytes: Delete
	// destroyed that tuple's offset, so nothing can find those bytes, let alone reuse them. Only
	// Compact recovers them.
	WriteHeader(header);

	assert(CheckInvariants());
	return slot_id;
}

Status HeapPage::Delete(slot_id_t slot_id) {
	auto slot_found = Get(slot_id);
	if (!slot_found.has_value()) return slot_found.error();

	Slot delete_slot = SlotAt(slot_id);

	// update header
	auto header = Header();
	header.dead_bytes += delete_slot.length;
	header.live_count--;
	WriteHeader(header);

	// update slot
	delete_slot.length = 0;
	delete_slot.offset = 0;
	WriteSlot(slot_id, delete_slot);

	return Status::OK();
}

Result<UpdateOutcome> HeapPage::Update(slot_id_t slot_id, std::span<const std::byte> tuple) {
	if (tuple.empty()) {
		return std::unexpected(Status::InvalidArgument("cannot update to a zero-length tuple"));
	}
	if (tuple.size() > MAX_TUPLE_SIZE) {
		return std::unexpected(Status::InvalidArgument("tuple exceeds the maximum tuple size"));
	}

	// Existence and liveness in one shot, with Get's error propagated unchanged: an update to a
	// deleted RID is NotFound, not Corruption. The span it hands back is deliberately discarded —
	// it is a const view, and every write below goes through the slot instead.
	auto existing = Get(slot_id);
	if (!existing.has_value()) return std::unexpected(existing.error());

	auto slot = SlotAt(slot_id);
	auto header = Header();

	// One snapshot, taken before anything mutates, so the branch conditions below cannot end up
	// comparing a stale header field against a freshly recomputed Contiguous().
	const std::size_t contiguous = Contiguous();
	const auto new_length = static_cast<uint16_t>(tuple.size());

	UpdateOutcome outcome = UpdateOutcome::kSamePage;
	if (slot.length >= new_length) {
		// Case 1: overwrite in place. The tuple keeps its offset, so the bytes freed at the tail
		// of the old extent are referenced by no slot and no offset; dead_bytes is the only
		// record that they exist.
		std::memcpy(body_.data() + slot.offset, tuple.data(), new_length);

		header.dead_bytes = static_cast<uint16_t>(header.dead_bytes + (slot.length - new_length));
		WriteHeader(header);

		slot.length = new_length;
		WriteSlot(slot_id, slot);
	} else if (new_length <= contiguous) {
		// Case 2, fast path: it grows, but there is room at the low-water mark, so relocate
		// within the page and skip the compaction entirely. The slot index does not change.
		header.dead_bytes = static_cast<uint16_t>(header.dead_bytes + slot.length);
		header.tuple_data_start = static_cast<uint16_t>(header.tuple_data_start - new_length);

		std::memcpy(body_.data() + header.tuple_data_start, tuple.data(), new_length);
		WriteHeader(header);

		slot.length = new_length;
		slot.offset = header.tuple_data_start;
		WriteSlot(slot_id, slot);
	} else if (new_length <= contiguous + header.dead_bytes + slot.length) {
		// Case 2, slow path: it fits only after reclaiming garbage. The `+ slot.length` term is
		// this tuple's own bytes, which are about to become garbage themselves — which is why the
		// test must be pure arithmetic run BEFORE any mutation, since case 3 has to leave the
		// page untouched.
		header.dead_bytes = static_cast<uint16_t>(header.dead_bytes + slot.length);
		header.live_count = static_cast<uint16_t>(header.live_count - 1);
		WriteHeader(header);

		slot.offset = 0;
		slot.length = 0;
		WriteSlot(slot_id, slot);

		// Killing first is what stops the compaction from copying bytes that are already
		// abandoned. The slot is dead here while still logically live, which is safe only
		// because the whole operation runs under one write latch.
		Compact();

		// Compact rewrote tuple_data_start and zeroed dead_bytes, so the local copy is stale.
		header = Header();
		header.tuple_data_start = static_cast<uint16_t>(header.tuple_data_start - new_length);
		header.live_count = static_cast<uint16_t>(header.live_count + 1);

		std::memcpy(body_.data() + header.tuple_data_start, tuple.data(), new_length);
		WriteHeader(header);

		slot.offset = header.tuple_data_start;
		slot.length = new_length;
		WriteSlot(slot_id, slot);
	} else {
		outcome = UpdateOutcome::kDoesNotFit;
	}

	assert(CheckInvariants());
	return outcome;
}

void HeapPage::Compact() {
	auto header = Header();

	// Nothing to reclaim, and that is provable rather than a guess. The accounting identity says
	// tuple_data_start + live_bytes + dead_bytes == PAGE_BODY_SIZE, so zero garbage means the live
	// tuples exactly fill the region, and non-overlapping tuples that exactly fill a region are
	// already packed. Update's slow path calls this unconditionally, so the early exit is
	// load-bearing rather than decorative.
	if (header.dead_bytes == 0) return;

	// Rebuilt in a scratch buffer and copied back in one shot, rather than slid in place. Sliding
	// in place does work — descending offset order guarantees no tuple can land on one that has
	// not moved yet — but it needs the live slots sorted by offset first and memmove throughout.
	// Postgres shipped exactly that for years and then reversed it: compactify_tuples sorted and
	// slid until PG14 rewrote it around a scratch buffer, which came out both simpler and faster.
	// InnoDB's page reorganize copies via a temp block too.
	//
	// Left uninitialised on purpose. The loop writes every byte of [cursor, PAGE_BODY_SIZE) before
	// the copy back reads it — cursor drops by exactly the length the loop then fills — so nothing
	// uninitialised is ever read, and zeroing 4KB per compaction would be pure waste.
	std::array<std::byte, PAGE_BODY_SIZE> scratch;
	std::size_t cursor = PAGE_BODY_SIZE;

	[[maybe_unused]] const auto freed_from = header.tuple_data_start;

	for (slot_id_t i = 0; i < header.slot_count; ++i) {
		Slot slot = SlotAt(i);

		// A dead slot keeps its index forever so a stale RID still has something to resolve
		// against. Compaction moves bytes; it never renumbers slots.
		if (slot.IsDead()) continue;

		cursor -= slot.length;
		std::memcpy(scratch.data() + cursor, body_.data() + slot.offset, slot.length);

		// Written inside the same pass that moved the bytes. The rule the whole format rests on is
		// that no operation may move a tuple without updating its slot in the same breath.
		slot.offset = static_cast<uint16_t>(cursor);
		WriteSlot(i, slot);
	}

	// ONLY the occupied tail. Copying the whole buffer back would stamp over the free space and,
	// below it, the slot array this loop just rewrote.
	std::memcpy(body_.data() + cursor, scratch.data() + cursor, PAGE_BODY_SIZE - cursor);

	// slot_count and live_count are deliberately untouched. Update's slow path depends on it: that
	// code decrements live_count before calling here and restores it afterwards, so an adjustment
	// in this function would leave the count off by one.
	header.tuple_data_start = static_cast<uint16_t>(cursor);
	header.dead_bytes = 0;
	WriteHeader(header);

#ifndef NDEBUG
	// The band that just became free — exactly the old dead_bytes, and strictly inside the old
	// tuple region, so it can never reach the slot array. Poisoning it turns a stale offset into
	// obvious garbage instead of plausible-looking bytes, which is the difference between a loud
	// failure and a silent wrong answer.
	std::memset(body_.data() + freed_from, 0xDD, cursor - freed_from);
#endif

	assert(CheckInvariants());
}

}  // namespace kernsql

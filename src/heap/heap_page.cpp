#include "heap/heap_page.hpp"

#include <algorithm>
#include <cassert>
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
	HeapSubHeader sub_header{0, 0, 0, 0};
	WriteHeader(sub_header);
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

Status HeapPage::Delete(slot_id_t /*slot*/) {
	// TODO(heap)
	return Status::Internal("HeapPage::Delete not implemented");
}

Result<UpdateOutcome> HeapPage::Update(slot_id_t /*slot*/, std::span<const std::byte> /*tuple*/) {
	// TODO(heap)
	return std::unexpected(Status::Internal("HeapPage::Update not implemented"));
}

void HeapPage::Compact() {
	// TODO(heap)
}

}  // namespace kernsql

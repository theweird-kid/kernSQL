#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace kernsql {

using page_id_t = int32_t;
using slot_id_t = uint16_t;
using frame_id_t = int32_t;
using lsn_t = uint64_t;

inline constexpr page_id_t INVALID_PAGE = -1;
inline constexpr page_id_t META_PAGE_ID = 0;          // reserved superblock page; never allocatable
inline constexpr page_id_t CATALOG_ROOT_PAGE_ID = 1;  // reserved page for catalog root
inline constexpr std::size_t PAGE_SIZE = 4096;
inline constexpr std::size_t PAGE_HEADER_SIZE = 32;

// Everything after the header. This — not PAGE_SIZE — is what a page guard hands out, so a
// caller cannot name the header bytes as raw memory and cannot clobber them.
inline constexpr std::size_t PAGE_BODY_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;

// Bumped whenever the on-disk layout of PageHeader changes in a way an older build would
// misread. Stamped into every header DiskManager writes; nothing reads it back yet, which is
// the point — it exists so that a future format change has a discriminator to branch on
// instead of guessing.
inline constexpr uint16_t PAGE_FORMAT_VERSION = 1;

struct RID {
	page_id_t page_id{INVALID_PAGE};
	slot_id_t slot{0};

	auto operator<=>(const RID&) const = default;
	bool isValid() const { return page_id != INVALID_PAGE; }
};

// INVALID is pinned at 0 so that an all-zero page cannot pass for a real one. Zeros
// are what the filesystem hands back for a sparse or truncate-extended file, so with
// META at 0 an empty file's page 0 validated as a genuine meta page — and Open() then
// read a freelist head of 0 out of the zeroed next_page_id, putting the reserved meta
// page itself at the head of the freelist.
//
// This is a cheap mitigation, not a format check: a garbage file whose first byte
// happens to equal a valid PageType still passes. A magic + version field in the meta
// page is the actual answer and does not exist yet.
//
// These values are persisted in every page header, so renumbering them is an on-disk
// format break. Append new types at the end; never reorder or reuse a value.
enum class PageType : uint8_t {
	INVALID = 0,
	META,
	INDEX_INTERNAL,
	INDEX_LEAF,
	HEAP,
	ALLOCATED,
	FREE,
	CATALOG
};

}  // namespace kernsql

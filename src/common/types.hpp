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
inline constexpr std::size_t PAGE_HEADER_SIZE = 24;

struct RID {
	page_id_t page_id{INVALID_PAGE};
	slot_id_t slot{0};

	auto operator<=>(const RID&) const = default;
	bool isValid() const { return page_id != INVALID_PAGE; }
};

enum class PageType : uint8_t { META, INDEX_INTERNAL, INDEX_LEAF, HEAP, ALLOCATED, FREE, CATALOG };

}  // namespace kernsql

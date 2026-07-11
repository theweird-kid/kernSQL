#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace kernsql {

using page_id_t = int32_t;
using slot_id_t = uint16_t;
using frame_id_t = int32_t;

inline constexpr page_id_t INVALID_PAGE = -1;
inline constexpr std::size_t PAGE_SIZE = 4096;

struct RID {
	page_id_t page_id{INVALID_PAGE};
	slot_id_t slot{0};

	auto operator<=>(const RID&) const = default;
	bool isValid() const { return page_id != INVALID_PAGE; }
};

}  // namespace kernsql

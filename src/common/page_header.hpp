#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "common/types.hpp"

namespace kernsql {
/*
 * struct PageHeader forms the first 24 bytes of a page, it is zero-padded to ensure byte alignment
 */
struct PageHeader {
	PageType page_type{PageType::FREE};
	uint8_t reserved0[3]{};  // explicit padding — always zero
	page_id_t next_page_id{INVALID_PAGE};
	page_id_t prev_page_id{INVALID_PAGE};
	uint32_t reserved1{0};  // explicit padding before 8-aligned lsn
	lsn_t page_lsn{0};

	static PageHeader ReadFrom(std::span<const std::byte, PAGE_HEADER_SIZE> page_header) {
		PageHeader h;
		std::memcpy(&h, page_header.data(), sizeof(PageHeader));
		return h;
	}
	void WriteTo(std::span<std::byte, PAGE_HEADER_SIZE> page_header) const {
		std::memcpy(page_header.data(), this, sizeof(PageHeader));
	}
};

static_assert(sizeof(PageHeader) == PAGE_HEADER_SIZE, "Page Header must be exactly 24 bytes");
static_assert(std::is_standard_layout_v<PageHeader>, "Page Header must be standard layout");
static_assert(std::is_trivially_copyable_v<PageHeader>, "PageHeader must be trivially copyable");

}  // namespace kernsql

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "common/types.hpp"

namespace kernsql {
/*
 * struct PageHeader forms the first 32 bytes of every page.
 *
 * Field order is chosen so every member lands on its natural alignment with no implicit
 * padding: page_lsn sits at offset 16 and the whole struct is a multiple of 8. The two
 * reserved members are explicit rather than left to the compiler so that ReadFrom/WriteTo,
 * which memcpy the struct wholesale, move a fully-defined 32 bytes.
 *
 * NOT PORTABLE ACROSS ARCHITECTURES. ReadFrom/WriteTo are a raw memcpy, so the file is
 * host-endian and host-ABI. A database file written on one machine is only readable on
 * another with the same byte order and layout rules. That is a deliberate non-goal — a
 * byte-at-a-time serializer would fix it and buys nothing for a single-node engine.
 */
struct PageHeader {
	PageType page_type{PageType::FREE};
	uint8_t flags{0};  // reserved — always zero
	uint16_t format_version{PAGE_FORMAT_VERSION};

	// This page's own id, redundant with the offset it was read from — and redundant on
	// purpose. The offset says which page we *asked* for; this says which page the bytes
	// think they are. Derive identity from the offset alone and a disagreement becomes
	// undetectable by construction, because you have assumed the answer. Catches misdirected
	// writes, off-by-one page arithmetic, torn extends, and backups copied at a wrong offset.
	// InnoDB stores the same thing as FIL_PAGE_OFFSET; Postgres folds the block number into
	// its checksum to get the same effect.
	//
	// It does NOT catch a page freed and handed back out under the same id — only page_type
	// == FREE catches that.
	//
	// Stamped by DiskManager::write_page_header for every header it writes, and by
	// BufferPoolManager::NewPage for the one page that is created without ever being read.
	page_id_t page_id{INVALID_PAGE};

	// Doubles as the disk freelist link (DD-001) and, later, the B+tree leaf sibling pointer.
	// Postgres keeps sibling pointers in per-page-type special space at the end of the page
	// rather than in the shared header, since only some page types have siblings. This is a
	// deliberate deviation: the freelist is already built on it, and 8 bytes for next+prev is
	// 0.2% of a page.
	page_id_t next_page_id{INVALID_PAGE};
	page_id_t prev_page_id{INVALID_PAGE};

	// Reserved for a write-ahead log. Nothing writes it today and there is no WAL planned for
	// this engine, but the field is here because growing a page header later shifts every byte
	// of every page — an on-disk format break — whereas eight unused bytes cost 0.2%.
	lsn_t page_lsn{0};

	// Reserved. A checksum is what turns this header from "catches my bugs" into "catches the
	// disk's", which is the class Postgres's PageIsVerified and InnoDB's page checksum are
	// actually defending against. Space claimed now so implementing it later is not a format
	// break.
	uint32_t checksum{0};
	uint32_t reserved{0};

	static PageHeader ReadFrom(std::span<const std::byte, PAGE_HEADER_SIZE> page_header) {
		PageHeader h;
		std::memcpy(&h, page_header.data(), sizeof(PageHeader));
		return h;
	}
	void WriteTo(std::span<std::byte, PAGE_HEADER_SIZE> page_header) const {
		std::memcpy(page_header.data(), this, sizeof(PageHeader));
	}
};

static_assert(sizeof(PageHeader) == PAGE_HEADER_SIZE, "Page Header must be exactly 32 bytes");
static_assert(alignof(PageHeader) == 8, "page_lsn must land 8-aligned, so the struct must too");
static_assert(offsetof(PageHeader, page_lsn) == 16, "no implicit padding before page_lsn");
static_assert(std::is_standard_layout_v<PageHeader>, "Page Header must be standard layout");
static_assert(std::is_trivially_copyable_v<PageHeader>, "PageHeader must be trivially copyable");

}  // namespace kernsql

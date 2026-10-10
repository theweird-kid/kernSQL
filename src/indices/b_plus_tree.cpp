#include "b_plus_tree.hpp"

#include <cstdint>
#include <expected>

#include "common/status.hpp"
#include "common/types.hpp"
#include "indices/b_plus_tree_page.hpp"

using namespace kernsql;

Result<std::unique_ptr<BPlusTree>> BPlusTree::Create(BufferPoolManager& bpm, uint16_t leaf_max,
                                                     uint16_t internal_max) {
	// Validate before allocating, so a bad argument never leaves a page to clean up.
	if (leaf_max < MIN_FANOUT || leaf_max > NODE_CAPACITY) {
		return std::unexpected(
		    Status::InvalidArgument("leaf_max outside [MIN_FANOUT, NODE_CAPACITY]"));
	}
	if (internal_max < MIN_FANOUT || internal_max > NODE_CAPACITY) {
		return std::unexpected(
		    Status::InvalidArgument("internal_max outside [MIN_FANOUT, NODE_CAPACITY]"));
	}

	// index header
	auto idx_header_page = bpm.NewPage();
	if (!idx_header_page.has_value()) return std::unexpected(idx_header_page.error());
	const page_id_t header_page_id = idx_header_page->PageId();

	// root leaf
	auto root_leaf_page = bpm.NewPage();
	if (!root_leaf_page.has_value()) {
		// Free the header page, or nothing records its id and it leaks for good. DeletePage
		// rejects a pinned page, so drop the guard first. Best effort: if this fails too we leak
		// one page, and the caller still sees the error that actually caused the failure.
		idx_header_page->Drop();
		(void)bpm.DeletePage(header_page_id);
		return std::unexpected(root_leaf_page.error());
	}

	// NewPage hands back a zeroed page typed ALLOCATED; AsLeaf checks for INDEX_LEAF, so stamp
	// first. Init writes level 0, count 0, no next leaf, and the zeroed body already satisfies
	// "bytes past count are zero".
	root_leaf_page->SetPageType(PageType::INDEX_LEAF);
	auto root_leaf = AsLeaf(*root_leaf_page);
	if (!root_leaf.has_value()) return std::unexpected(root_leaf.error());
	root_leaf->Init();

	IndexHeader idx_header;
	idx_header.height = 1;
	idx_header.root_page_id = root_leaf_page->PageId();
	idx_header.leaf_max = leaf_max;
	idx_header.internal_max = internal_max;

	// Same reason as the leaf: WriteIndexHeader refuses anything not typed INDEX_HEADER.
	idx_header_page->SetPageType(PageType::INDEX_HEADER);
	Status s = WriteIndexHeader(*idx_header_page, idx_header);
	if (!s.ok()) return std::unexpected(s);

	return std::unique_ptr<BPlusTree>(new BPlusTree(bpm, header_page_id, leaf_max, internal_max));
}

Result<std::unique_ptr<BPlusTree>> BPlusTree::Open(BufferPoolManager& bpm,
                                                   page_id_t header_page_id) {
	auto idx_header_page = bpm.FetchPageRead(header_page_id);
	if (!idx_header_page.has_value()) return std::unexpected(idx_header_page.error());

	auto idx_header = ReadIndexHeader(idx_header_page.value());
	if (!idx_header.has_value()) return std::unexpected(idx_header.error());

	if (!FanoutInRange(idx_header->leaf_max) || !FanoutInRange(idx_header->internal_max))
		return std::unexpected(Status::Corruption("invalid page Fanout!"));

	return std::unique_ptr<BPlusTree>(
	    new BPlusTree(bpm, header_page_id, idx_header->leaf_max, idx_header->internal_max));
}

Result<ReadPageGuard> BPlusTree::FindLeafRead(index_key_t key) {
	page_id_t root_page_id{INVALID_PAGE};
	uint16_t tree_height{0};
	{  // Get root page from index header
		auto idx_header_page = bpm_.FetchPageRead(header_page_id_);
		if (!idx_header_page.has_value()) return std::unexpected(idx_header_page.error());

		auto idx_header = ReadIndexHeader(idx_header_page.value());
		if (!idx_header.has_value()) return std::unexpected(idx_header.error());

		root_page_id = idx_header->root_page_id;
		tree_height = idx_header->height;
	}  // Release

	if (tree_height == 0) return std::unexpected(Status::Corruption("index header height is zero"));

	// Every level above the leaf. A height too large or too small for the real tree surfaces as
	// a type mismatch: AsInternal here, or the caller's AsLeaf on what comes back.
	page_id_t next_page_id = root_page_id;
	for (uint16_t height = 0; height + 1 < tree_height; height++) {
		auto read_page = bpm_.FetchPageRead(next_page_id);
		if (!read_page.has_value()) return std::unexpected(read_page.error());

		auto internal = AsInternal(read_page.value());
		if (!internal.has_value()) return std::unexpected(internal.error());

		next_page_id = internal->ChildAt(internal->ChildIndexFor(key));
	}

	return bpm_.FetchPageRead(next_page_id);  // LEAF
}

Result<RID> BPlusTree::Get(index_key_t key) {
	auto leaf_page = FindLeafRead(key);
	if (!leaf_page.has_value()) return std::unexpected(leaf_page.error());

	auto leaf = AsLeaf(leaf_page.value());
	if (!leaf.has_value()) return std::unexpected(leaf.error());

	auto idx = leaf->LowerBound(key);
	if (idx == leaf->Count() || leaf->KeyAt(idx) != key)  // NOT FOUND
		return std::unexpected(Status::NotFound("key not in index"));

	return leaf->EntryAt(idx).rid;  // FOUND
}

Result<IndexIterator> BPlusTree::Scan(index_key_t lo, index_key_t hi) {
	auto leaf_page = FindLeafRead(lo);
	if (!leaf_page.has_value()) return std::unexpected(leaf_page.error());

	auto leaf = AsLeaf(leaf_page.value());
	if (!leaf.has_value()) return std::unexpected(leaf.error());

	// No equality check: lo need not be present, and idx may equal Count() — Next() handles both.
	// The guard drops on return; the iterator re-fetches the leaf on its first Next().
	return IndexIterator(bpm_, leaf_page->PageId(), leaf->LowerBound(lo), hi);
}

IndexIterator::~IndexIterator() = default;

IndexIterator::IndexIterator(IndexIterator&& other) noexcept
    : bpm_(other.bpm_),
      guard_(std::move(other.guard_)),
      page_id_(other.page_id_),
      index_(other.index_),
      hi_(other.hi_),
      current_(other.current_) {
	other.guard_.reset();
	other.page_id_ = INVALID_PAGE;
}

IndexIterator& IndexIterator::operator=(IndexIterator&& other) noexcept {
	if (this == &other) return *this;

	// release current guard
	this->guard_.reset();

	// move state from other
	this->bpm_ = other.bpm_;
	this->guard_ = std::move(other.guard_);
	this->page_id_ = other.page_id_;
	this->index_ = other.index_;
	this->hi_ = other.hi_;
	this->current_ = other.current_;

	// exhaust other
	other.guard_.reset();
	other.page_id_ = INVALID_PAGE;

	return *this;
}

Result<bool> IndexIterator::Next() {
	// Loops only to step over leaves with nothing left to report: Scan's one-past-the-end
	// position, an empty root leaf, or the end of each leaf as the scan crosses it.
	while (page_id_ != INVALID_PAGE) {
		// Fetch only on arriving at a leaf; every other call reuses the guard kept from the last.
		if (!guard_) {
			auto read_page = bpm_->FetchPageRead(page_id_);
			if (!read_page.has_value()) {
				page_id_ = INVALID_PAGE;
				return std::unexpected(read_page.error());
			}
			guard_.emplace(std::move(read_page.value()));
		}

		auto leaf = AsLeaf(*guard_);
		if (!leaf.has_value()) {
			guard_.reset();
			page_id_ = INVALID_PAGE;
			return std::unexpected(leaf.error());
		}

		// Past this leaf's last entry: not a result. Read the link BEFORE dropping the guard —
		// the view points into the frame, and the frame is not ours once the pin is gone.
		if (index_ >= leaf->Count()) {
			const page_id_t next = leaf->NextLeaf();
			guard_.reset();
			page_id_ = next;
			index_ = 0;
			continue;
		}

		const LeafEntry entry = leaf->EntryAt(index_);
		if (entry.key > hi_) {  // Past the interval: done, and hold nothing.
			guard_.reset();
			page_id_ = INVALID_PAGE;
			return false;
		}

		current_ = entry;
		index_++;
		return true;  // Still holding the leaf's guard, for the next call.
	}

	return false;
}

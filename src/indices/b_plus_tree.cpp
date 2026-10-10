#include "b_plus_tree.hpp"

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

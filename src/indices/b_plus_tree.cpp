#include "b_plus_tree.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/logger.hpp"
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

bool BPlusTree::CheckInvariants() {
	page_id_t root_page_id{INVALID_PAGE};
	uint16_t tree_height{0};
	{
		auto header_page_guard = bpm_.FetchPageRead(header_page_id_);
		if (!header_page_guard.has_value()) {
			LOG_DEBUG("unable to get index header page guard");
			return false;
		}

		auto index_header = ReadIndexHeader(header_page_guard.value());
		if (!index_header.has_value()) {
			const std::string_view message = index_header.error().message();
			LOG_DEBUG("unable to read index header: %.*s", static_cast<int>(message.size()),
			          message.data());
			return false;
		}

		root_page_id = index_header->root_page_id;
		tree_height = index_header->height;

		if (tree_height < 1) {
			LOG_DEBUG("incorrect tree height, got: %d", tree_height);
			return false;
		}

		if (index_header->leaf_max != leaf_max_) {
			LOG_DEBUG("incorrect leaf_max, expected: %d, got: %d", leaf_max_,
			          index_header->leaf_max);
			return false;
		}
		if (index_header->internal_max != internal_max_) {
			LOG_DEBUG("incorrect internal_max, expected: %d, got: %d", internal_max_,
			          index_header->internal_max);
			return false;
		}
	}

	// The header counts as seen, so a child pointer back at it fails the walk.
	std::set<page_id_t> seen_pages;
	seen_pages.insert(header_page_id_);

	std::vector<std::pair<page_id_t, page_id_t>> leaves;

	// height counts levels, the root's level is height - 1: a height-1 tree's root is a leaf.
	if (!CheckSubtree(root_page_id, static_cast<uint16_t>(tree_height - 1), INT64_MIN, std::nullopt,
	                  true, seen_pages, leaves)) {
		return false;
	}

	// The leaf chain against the walk: each leaf's NextLeaf() is the leaf the walk reached after
	// it, and the last one points nowhere. Compared against a list of known length, so a cycle in
	// the chain fails here instead of hanging a chain walk.
	for (std::size_t i = 0; i < leaves.size(); i++) {
		const page_id_t expected_next = i + 1 < leaves.size() ? leaves[i + 1].first : INVALID_PAGE;
		if (leaves[i].second != expected_next) {
			LOG_DEBUG("leaf chain broken at leaf %zu (page %d): NextLeaf is %d, expected %d", i,
			          leaves[i].first, leaves[i].second, expected_next);
			return false;
		}
	}
	return true;
}

bool BPlusTree::CheckSubtree(page_id_t page_id, uint16_t expected_level, index_key_t lo,
                             std::optional<index_key_t> hi, bool is_root,
                             std::set<page_id_t>& seen_pages,
                             std::vector<std::pair<page_id_t, page_id_t>>& leaves) {
	if (seen_pages.contains(page_id)) {
		LOG_DEBUG("page_id: %d, expected_level: %d, error: %d already in seen set", page_id,
		          expected_level, page_id);
		return false;
	}
	seen_pages.insert(page_id);

	auto read_guard = bpm_.FetchPageRead(page_id);
	if (!read_guard.has_value()) {
		LOG_DEBUG("page_id: %d, expected_level: %d, error: unable to fetch read_guard for page %d",
		          page_id, expected_level, page_id);
		return false;
	}
	if (expected_level > 0) {  // INTERNAL
		// Copied out under the guard, which is dropped before recursing: one pin at a time.
		std::vector<InternalEntry> entries;
		{
			auto internal_node = AsInternal(read_guard.value());
			if (!internal_node.has_value()) {
				const std::string_view message = internal_node.error().message();
				LOG_DEBUG("page_id: %d, expected_level: %d, error: %.*s", page_id, expected_level,
				          static_cast<int>(message.size()), message.data());
				return false;
			}

			// FIRST, and returning: past this line the loop trusts Count() to bound its reads,
			// which only the node's own check guarantees.
			if (!internal_node->CheckInvariants()) {
				LOG_DEBUG("page_id: %d, expected_level: %d, error: node-local invariants failed",
				          page_id, expected_level);
				return false;
			}

			if (internal_node->Level() != expected_level) {
				LOG_DEBUG("page_id: %d, expected_level: %d, error: stored level is %d", page_id,
				          expected_level, internal_node->Level());
				return false;
			}

			const uint16_t count = internal_node->Count();
			const uint16_t min_children = is_root ? 2 : InternalMinChildren(internal_max_);
			if (count < min_children || count > internal_max_) {
				LOG_DEBUG("page_id: %d, expected_level: %d, error: %d children, outside [%d, %d]",
				          page_id, expected_level, count, min_children, internal_max_);
				return false;
			}

			entries.reserve(count);
			for (uint16_t i = 0; i < count; i++) entries.push_back(internal_node->EntryAt(i));
		}
		read_guard->Drop();

		// Separators are held to this node's interval too, not just ordered among themselves.
		// entry[0].key is the unread INT64_MIN slot, not a separator.
		for (std::size_t i = 1; i < entries.size(); i++) {
			const index_key_t key = entries[i].key;
			if (key < lo || (hi.has_value() && key >= *hi)) {
				LOG_DEBUG("page_id: %d, expected_level: %d, error: separator %lld outside [lo, hi)",
				          page_id, expected_level, static_cast<long long>(key));
				return false;
			}
		}

		// Child i covers [separator i, separator i + 1). The two ends have no separator of their
		// own and inherit this node's bounds: child 0 gets lo — NOT entry[0]'s INT64_MIN — and the
		// last child gets hi.
		const auto child_level = static_cast<uint16_t>(expected_level - 1);
		for (std::size_t i = 0; i < entries.size(); i++) {
			const index_key_t child_lo = i == 0 ? lo : entries[i].key;
			const std::optional<index_key_t> child_hi =
			    i + 1 < entries.size() ? std::optional<index_key_t>(entries[i + 1].key) : hi;
			if (!CheckSubtree(entries[i].child, child_level, child_lo, child_hi, false, seen_pages,
			                  leaves)) {
				return false;
			}
		}
		return true;

	} else {  // LEAF
		auto leaf_node = AsLeaf(read_guard.value());
		if (!leaf_node.has_value()) {
			const std::string_view message = leaf_node.error().message();
			LOG_DEBUG("page_id: %d, expected_level: %d, error: %.*s", page_id, expected_level,
			          static_cast<int>(message.size()), message.data());
			return false;
		}

		if (!leaf_node->CheckInvariants()) {
			LOG_DEBUG("page_id: %d, expected_level: %d, error: node-local invariants failed",
			          page_id, expected_level);
			return false;
		}

		const uint16_t count = leaf_node->Count();
		// Level 0 is already part of the node's own check. A root leaf may be empty.
		const uint16_t min_entries = is_root ? 0 : LeafMinEntries(leaf_max_);
		if (count < min_entries || count > leaf_max_) {
			LOG_DEBUG("page_id: %d, expected_level: %d, error: %d entries, outside [%d, %d]",
			          page_id, expected_level, count, min_entries, leaf_max_);
			return false;
		}

		for (uint16_t i = 0; i < count; i++) {
			const index_key_t key = leaf_node->KeyAt(i);
			if (key < lo || (hi.has_value() && key >= *hi)) {
				// hi may be "no upper bound"; never .value() it unchecked, or the report crashes.
				LOG_DEBUG("page_id: %d, expected_level: %d, error: key %lld outside [%lld, %s)",
				          page_id, expected_level, static_cast<long long>(key),
				          static_cast<long long>(lo),
				          hi.has_value() ? std::to_string(*hi).c_str() : "+inf");
				return false;
			}
		}

		leaves.push_back({page_id, leaf_node->NextLeaf()});
		return true;
	}
}

Result<BPlusTree::WritePath> BPlusTree::FindLeafWrite(index_key_t key) {
	auto header_guard = bpm_.FetchPageWrite(header_page_id_);
	if (!header_guard.has_value()) {
		return std::unexpected(header_guard.error());
	}

	auto idx_header = ReadIndexHeader(header_guard.value());
	if (!idx_header.has_value()) {
		return std::unexpected(idx_header.error());
	}
	uint16_t tree_height = idx_header->height;
	page_id_t root_page_id = idx_header->root_page_id;

	if (tree_height == 0) {
		return std::unexpected(Status::Corruption("tree height can't be 0"));
	}

	WritePath wr_path{std::move(header_guard.value()), idx_header.value(), {}, {}};

	page_id_t next_page_id = root_page_id;
	// Traverse the internal nodes
	for (uint16_t height = 0; height + 1 < tree_height; height++) {
		auto write_page = bpm_.FetchPageWrite(next_page_id);
		if (!write_page.has_value()) return std::unexpected(write_page.error());

		auto internal = AsInternal(write_page.value());
		if (!internal.has_value()) return std::unexpected(internal.error());

		auto child = internal->ChildIndexFor(key);
		next_page_id = internal->ChildAt(child);

		wr_path.nodes.emplace_back(std::move(write_page.value()));
		wr_path.child_indexes.push_back(child);
	}

	// add the leaf node
	auto leaf_page = bpm_.FetchPageWrite(next_page_id);
	if (!leaf_page.has_value()) return std::unexpected(leaf_page.error());

	auto leaf_node = AsLeaf(leaf_page.value());
	if (!leaf_node.has_value()) return std::unexpected(leaf_node.error());

	wr_path.nodes.emplace_back(std::move(leaf_page.value()));
	return wr_path;
}

Status BPlusTree::Insert(index_key_t key, RID rid) {
	auto path = FindLeafWrite(key);
	if (!path.has_value()) return path.error();

	// Cannot fail: FindLeafWrite validated every page type on the path.
	auto leaf_node = AsLeaf(path->nodes.back()).value();
	const uint16_t idx = leaf_node.LowerBound(key);
	if (idx < leaf_node.Count() && leaf_node.KeyAt(idx) == key) {
		return Status::DuplicateKey("key " + std::to_string(key) + " already exists");
	}

	if (!IsLeafFull(leaf_node)) {
		leaf_node.InsertAt(idx, LeafEntry{key, rid});
		return Status::OK();
	}

	// Split. Plan and allocate first — the last point where anything may fail.
	const uint16_t splits = CountSplits(path.value());
	const bool new_root = (splits == path->nodes.size());
	auto pages = AllocatePages(static_cast<uint16_t>(splits + (new_root ? 1 : 0)));
	if (!pages.has_value()) return pages.error();

	// ---- Nothing below this line may fail: every write runs to completion. ----

	// pages[0] is the leaf's right half, pages[1 .. splits) the internal right halves going up,
	// pages[splits] the new root when there is one.
	auto& leaf_right_guard = (*pages)[0];
	leaf_right_guard.SetPageType(PageType::INDEX_LEAF);
	auto leaf_right = AsLeaf(leaf_right_guard).value();
	leaf_right.Init();
	index_key_t sep =
	    SplitLeaf(leaf_node, leaf_right, leaf_right_guard.PageId(), idx, LeafEntry{key, rid});
	page_id_t right_id = leaf_right_guard.PageId();

	// Walk up the internal nodes. Signed, so the loop can step past the root to -1.
	std::size_t next_page = 1;
	for (int i = static_cast<int>(path->nodes.size()) - 2; i >= 0; i--) {
		const auto level = static_cast<std::size_t>(i);
		auto parent = AsInternal(path->nodes[level]).value();
		const InternalEntry entry{sep, right_id, 0};
		// Right after the child that split — recorded on the way down, no second search.
		const auto pos = static_cast<uint16_t>(path->child_indexes[level] + 1);

		if (!IsInternalFull(parent)) {
			parent.InsertAt(pos, entry);
			return Status::OK();
		}

		auto& right_guard = (*pages)[next_page++];
		right_guard.SetPageType(PageType::INDEX_INTERNAL);
		auto right = AsInternal(right_guard).value();
		right.Init(parent.Level());
		sep = SplitInternal(parent, right, pos, entry);
		right_id = right_guard.PageId();
	}

	// The run of full nodes reached the root, so the root split: grow the tree by one level.
	assert(new_root && next_page == splits);
	auto& root_guard = (*pages)[next_page];
	root_guard.SetPageType(PageType::INDEX_INTERNAL);
	auto root = AsInternal(root_guard).value();

	// The old root sits at level height - 1, so the new one is at height.
	root.Init(path->header.height);
	const std::array<InternalEntry, 2> children{
	    InternalEntry{INT64_MIN, path->header.root_page_id, 0}, InternalEntry{sep, right_id, 0}};
	root.Assign(children);

	path->header.root_page_id = root_guard.PageId();
	path->header.height++;

	// Cannot fail: FindLeafWrite already read this page as INDEX_HEADER. Asserted rather than
	// returned, because returning here would report failure for an insert that has happened.
	[[maybe_unused]] const Status s = WriteIndexHeader(path->header_guard, path->header);
	assert(s.ok());
	return Status::OK();
}

uint16_t BPlusTree::CountSplits(WritePath& path) const {
	// AsLeaf/AsInternal cannot fail: FindLeafWrite validated every page type on the path.
	if (!IsLeafFull(*AsLeaf(path.nodes.back()))) return 0;
	uint16_t splits = 1;

	// Signed: a size_t counter is always >= 0 and would wrap past the root.
	for (int i = static_cast<int>(path.nodes.size()) - 2; i >= 0; i--) {
		if (!IsInternalFull(*AsInternal(path.nodes[static_cast<std::size_t>(i)]))) break;
		++splits;
	}
	return splits;
}

Result<std::vector<WritePageGuard>> BPlusTree::AllocatePages(uint16_t count) {
	std::vector<WritePageGuard> pages;
	pages.reserve(count);

	for (uint16_t i = 0; i < count; i++) {
		auto page = bpm_.NewPage();
		if (page.has_value()) {
			pages.push_back(std::move(page.value()));
			continue;
		}

		Status err = page.error();
		std::vector<page_id_t> ids;
		ids.reserve(pages.size());
		for (auto& p : pages) ids.push_back(p.PageId());

		// DeletePage rejects a pinned page, so drop every guard first. Best effort, as in Create:
		// a failed delete leaks a page, and the caller still sees the error that caused this.
		pages.clear();
		for (page_id_t id : ids) (void)bpm_.DeletePage(id);

		return std::unexpected(err);
	}

	return pages;
}

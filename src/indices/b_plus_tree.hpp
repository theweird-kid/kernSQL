#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "indices/b_plus_tree_page.hpp"

namespace kernsql {

/*
 * The primary-key index (DD-005): unique int64 keys, each mapped to the RID of the row holding it.
 *
 * Identified by its HEADER PAGE ID, which never changes and is what the catalog persists. The
 * header holds the root page id, which does change — on a root split and, in phase 2, on a root
 * collapse — so the root is read from the header at the start of every operation and never
 * cached in this object.
 *
 * THREADING: ONE WRITER OR MANY READERS, NEVER BOTH, ENFORCED BY THE CALLER. The table's
 * reader-writer lock covers its index. This class defines no lock and runs no latch protocol: no
 * crabbing, no B-link. It still takes a page guard for every access, because the pool needs the
 * pin to keep a frame resident. Two rules below depend on the table lock and are the first things
 * to revisit if concurrent writers ever return: readers drop the header guard before descending,
 * and writers hold the whole root-to-leaf path pessimistically.
 *
 * OWNS NOTHING IT USES, like TableHeap: a BufferPoolManager& and a page id.
 *
 * NOT ITS JOB: extracting the key from a row, or keeping the index in step with the heap. Both
 * belong to the table-level object that owns a TableHeap and its BPlusTree together (DD-005,
 * "Keeping the index and the heap in sync").
 */
class BPlusTree;

/*
 * A forward scan over a closed key interval [lo, hi]. Holds ONE leaf guard at a time and follows
 * the leaf's NextLeaf() link, for the same fixed-pool reason as TableIterator.
 *
 * Entries are copied out as they are reached. A (key, RID) pair is 16 bytes of value, so unlike a
 * tuple span there is no lifetime to get wrong: Key() and Rid() stay meaningful after the
 * iterator moves on.
 *
 * > NEVER MUTATE THE TREE THROUGH A LIVE ITERATOR. IT WILL HANG.
 *
 * A successful Next() returns holding a read guard on the current leaf. A Delete or Insert on the
 * same thread then asks for a write guard on that frame and blocks forever on the non-recursive
 * shared_mutex — DD-004's TableIterator rule, unchanged. Collect, let the iterator die, then
 * mutate.
 *
 * Move-only cursor with a fallible Next(), not an STL iterator, for TableIterator's two reasons:
 * it holds a guard, and advancing fetches a page and can fail.
 */
class IndexIterator {
  public:
	IndexIterator() = delete;

	IndexIterator(const IndexIterator&) = delete;
	IndexIterator& operator=(const IndexIterator&) = delete;
	IndexIterator(IndexIterator&&) noexcept;
	IndexIterator& operator=(IndexIterator&&) noexcept;
	~IndexIterator();

	/*
	 * Advance to the next entry with key <= hi. False at the end of the interval or the end of the
	 * chain; an error means a fetch failed and the scan cannot continue.
	 *
	 * Positioned BEFORE the first entry, so a scan is `while (*it.Next()) { ... }` with no special
	 * case for an empty interval.
	 *
	 * A position one past the last entry of a leaf is NOT a result: step to the next leaf before
	 * reporting anything. That case is routine, not exotic — lower_bound(lo) lands there whenever
	 * every key in the leaf the descent reached is below lo, which stale separators make common
	 * after deletes.
	 *
	 * Idempotent once exhausted: keeps answering false, and holds no guard.
	 */
	[[nodiscard]] Result<bool> Next();

	// The current entry. Only meaningful once Next() has returned true.
	[[nodiscard]] index_key_t Key() const { return current_.key; }
	[[nodiscard]] RID Rid() const { return current_.rid; }

  private:
	friend class BPlusTree;

	// Built by BPlusTree::Scan, which has already descended to the leaf that would hold `lo` and
	// computed LowerBound(lo) there. `index` may equal that leaf's count — see Next().
	IndexIterator(BufferPoolManager& bpm, page_id_t leaf_page_id, uint16_t index, index_key_t hi)
	    : bpm_(&bpm), page_id_(leaf_page_id), index_(index), hi_(hi) {}

	BufferPoolManager* bpm_;

	// No default constructor on a guard, so "no page held" is spelled with an optional.
	std::optional<ReadPageGuard> guard_;

	// INVALID_PAGE once exhausted. The leaf being read, and the NEXT index to examine in it.
	page_id_t page_id_;
	uint16_t index_;

	index_key_t hi_;
	LeafEntry current_{};
};

class BPlusTree {
  public:
	/*
	 * Create an empty index: allocate the header page and an empty root leaf, stamp both, and
	 * write root_page_id, height = 1 and the fanout into the header.
	 *
	 * Fanout defaults to the physical capacity. Tests pass something small (>= MIN_FANOUT) so that
	 * splits, redistributions and merges happen within a handful of keys — the most important
	 * testing decision in DD-005. kInvalidArgument outside [MIN_FANOUT, NODE_CAPACITY].
	 */
	[[nodiscard]] static Result<std::unique_ptr<BPlusTree>> Create(
	    BufferPoolManager& bpm, uint16_t leaf_max = NODE_CAPACITY,
	    uint16_t internal_max = NODE_CAPACITY);

	/*
	 * Open an existing index from the header page id the catalog stored.
	 *
	 * UNLIKE TableHeap::Open, THIS READS A PAGE. The fanout is fixed for the tree's life and every
	 * operation needs it, so it is read once here and cached; that read also validates the page as
	 * INDEX_HEADER (kCorruption otherwise) and the fanout as in range. The root is NOT cached — it
	 * moves, and is re-read from the header by every operation.
	 */
	[[nodiscard]] static Result<std::unique_ptr<BPlusTree>> Open(BufferPoolManager& bpm,
	                                                             page_id_t header_page_id);

	BPlusTree(const BPlusTree&) = delete;
	BPlusTree& operator=(const BPlusTree&) = delete;
	BPlusTree(BPlusTree&&) = delete;
	BPlusTree& operator=(BPlusTree&&) = delete;

	static bool FanoutInRange(uint16_t max) { return max >= MIN_FANOUT && max <= NODE_CAPACITY; }

	/*
	 * Insert `key -> rid`. kDuplicateKey if the key is present, reported before anything is
	 * modified.
	 *
	 * > ALLOCATE EVERYTHING, THEN MUTATE.
	 *
	 * Holds write guards on the header and the whole root-to-leaf path. Before touching any page,
	 * counts the consecutive full nodes upward from the leaf — the nodes that will split — and
	 * allocates that many new pages, plus one for a new root if the run reaches it. If any
	 * allocation fails, the pages already allocated are freed and the tree is untouched. After
	 * that nothing can fail, so a split never stops halfway. That is what makes insert
	 * all-or-nothing without a WAL.
	 *
	 * Frame budget at peak: header + H path + up to H + 1 new pages = 2H + 2.
	 */
	[[nodiscard]] Status Insert(index_key_t key, RID rid);

	/*
	 * Remove `key`. kNotFound if absent. Phase 2 of DD-005: built after insert, search and split
	 * are complete and tested.
	 *
	 * Underflow redistributes from, or merges with, a sibling under the SAME parent — the left one
	 * if it exists, else the right. Always merges right into left. Separators are left alone by a
	 * plain delete: they are routing information and stay correct bounds even when the key they
	 * were copied from is gone.
	 *
	 * All or nothing, like Insert: a read-only planning pass walks up from the leaf and fetches
	 * every sibling the underflow cascade will need BEFORE the first write, so any error means the
	 * tree is byte-for-byte unchanged. After that the only fallible step is freeing merged-away
	 * pages; they are unlinked first, so a failed DeletePage leaks a page, never leaves a dangling
	 * pointer, and is swallowed — OK means the key is gone.
	 *
	 * Frame budget at peak: header + H path + up to H - 1 siblings = 2H.
	 */
	[[nodiscard]] Status Delete(index_key_t key);

	// Point lookup. kNotFound if absent. One guard at a time on the way down.
	[[nodiscard]] Result<RID> Get(index_key_t key);

	/*
	 * Forward scan over the closed interval [lo, hi]. Integer keys make every comparison predicate
	 * a closed interval — `pk < 5` is [INT64_MIN, 4] — so this is the only range shape needed; the
	 * executor does that translation, including the predicates that are empty at the ends of the
	 * domain. An empty interval (lo > hi) yields an iterator whose first Next() returns false.
	 *
	 * Can fail, unlike TableHeap::Scan, because positioning descends the tree.
	 */
	[[nodiscard]] Result<IndexIterator> Scan(index_key_t lo, index_key_t hi);

	/*
	 * The whole-tree check (DD-005, "Invariants"): page types, levels, key order, SEPARATOR BOUNDS
	 * carried down the recursion as a [lo, hi) interval, occupancy against the fanout, the leaf
	 * chain matching the recursive walk, and no page reached twice.
	 *
	 * O(n). For tests, after every operation — never on a production path. Public so a test can
	 * check a tree it drove through the public API alone.
	 */
	[[nodiscard]] bool CheckInvariants();

	// What the catalog persists. Never changes.
	[[nodiscard]] page_id_t HeaderPageId() const { return header_page_id_; }

	[[nodiscard]] uint16_t LeafMax() const { return leaf_max_; }
	[[nodiscard]] uint16_t InternalMax() const { return internal_max_; }

  private:
	BPlusTree(BufferPoolManager& bpm, page_id_t header_page_id, uint16_t leaf_max,
	          uint16_t internal_max)
	    : bpm_(bpm),
	      header_page_id_(header_page_id),
	      leaf_max_(leaf_max),
	      internal_max_(internal_max) {}

	/*
	 * The read-path descent shared by Get and Scan: read the root and height from the header, drop
	 * the header guard, then go down one read guard at a time to the leaf that would hold `key`.
	 *
	 * Returns the leaf's guard WITHOUT validating it as a leaf — each caller does its own AsLeaf,
	 * which is also where a header height too large for the real tree is caught.
	 */
	[[nodiscard]] Result<ReadPageGuard> FindLeafRead(index_key_t key);

	/*
	 * Everything a writer holds for the length of one operation (DD-005, "Writers: the whole path
	 * is held"). Built by FindLeafWrite; every guard lives until the operation returns, so a split
	 * finds its parent already latched and unchanged.
	 */
	struct WritePath {
		// Held from the start although only a root change writes it: by then nothing may fail,
		// and fetching it then could.
		WritePageGuard header_guard;
		IndexHeader header;

		// Root first, leaf last. nodes.size() == header.height.
		std::vector<WritePageGuard> nodes;

		// child_indexes[i] is the child of nodes[i] the descent followed, so
		// child_indexes.size() == nodes.size() - 1. A separator produced by splitting
		// nodes[i + 1] goes into nodes[i] at child_indexes[i] + 1 — no second search.
		std::vector<uint16_t> child_indexes;
	};

	/*
	 * The write-path descent: write-latch the header, read root and height, then go down holding
	 * a write guard on EVERY node, recording the child index taken at each level.
	 *
	 * Unlike FindLeafRead, validates every page's type on the way (AsInternal above the leaf,
	 * AsLeaf at it), so after it returns, AsLeaf/AsInternal on the path cannot fail — Insert's
	 * mutate phase relies on that. kCorruption on a zero height or a type mismatch.
	 *
	 * Pins header + height frames, all held until the WritePath dies.
	 */
	[[nodiscard]] Result<WritePath> FindLeafWrite(index_key_t key);

	// Full against THIS TREE'S fanout (leaf_max_ / internal_max_), never NODE_CAPACITY — getting
	// this wrong makes every small-fanout test silently stop splitting.
	[[nodiscard]] bool IsLeafFull(ConstLeafNode leaf) const { return leaf.Count() >= leaf_max_; }
	[[nodiscard]] bool IsInternalFull(ConstInternalNode node) const {
		return node.Count() >= internal_max_;
	}

	/*
	 * How many nodes an insert into this path will split: walking up from the leaf, the length of
	 * the run of full nodes, stopping at the first one that is not full. A node splits only if it
	 * is full AND receives an entry, and a parent receives one only when its child split.
	 *
	 * 0 means the leaf has room. A result equal to path.nodes.size() means the root splits too, and
	 * Insert needs ONE MORE page for the new root. Reads only; called before any allocation.
	 */
	[[nodiscard]] uint16_t CountSplits(WritePath& path) const;

	/*
	 * Allocate `count` pages with NewPage, returned zeroed, typed ALLOCATED and write-latched —
	 * the caller stamps each one before taking a view of it.
	 *
	 * All or nothing: if any NewPage fails, drop every guard already obtained, DeletePage each of
	 * those pages (best effort — DeletePage rejects a pinned page, hence the drop first; a failed
	 * delete leaks a page, which is what Create accepts too), and return the ORIGINAL error.
	 */
	[[nodiscard]] Result<std::vector<WritePageGuard>> AllocatePages(uint16_t count);

	// At or below the floor for THIS TREE'S fanout: losing one entry would underflow it. One
	// predicate answers both of the planner's questions — "does this node underflow?" for the path
	// node, "can it lend?" (no, if true) for its sibling. Non-root nodes only.
	[[nodiscard]] bool IsLeafAtFloor(ConstLeafNode leaf) const {
		return leaf.Count() <= LeafMinEntries(leaf_max_);
	}
	[[nodiscard]] bool IsInternalAtFloor(ConstInternalNode node) const {
		return node.Count() <= InternalMinChildren(internal_max_);
	}

	/*
	 * How one underflowing level gets fixed (DD-005, "Delete", step 2). Built by PlanDelete,
	 * consumed by ApplyDeletePlan.
	 */
	struct DeleteStep {
		enum class Fix : uint8_t { kRedistribute, kMerge };

		// Under the SAME parent, write-latched, type and level already validated — so applying
		// cannot fail.
		WritePageGuard sibling;

		// Left if the path node's child index is > 0, else right. Decides which node is `left`
		// for the Rotate*/Merge* call, and where the separator between them sits in the parent:
		// at the path node's child index if the sibling is left, at that index + 1 if right.
		bool sibling_is_left{false};

		Fix fix{Fix::kMerge};
	};

	struct DeletePlan {
		// steps[k] fixes path.nodes[nodes.size() - 1 - k], so steps[0] is the leaf. Empty when the
		// leaf does not underflow. Every step is a merge except possibly the last, which is the
		// only one allowed to redistribute: a redistribute costs the parent nothing and ends the
		// cascade.
		std::vector<DeleteStep> steps;

		// The cascade's last merge empties an internal root down to one child, which then
		// replaces it.
		bool collapse_root{false};
	};

	/*
	 * Step 2 — the read-only planning pass, and the ONLY fallible part of Delete after the
	 * descent. Walks up from the leaf deciding each level from counts alone, assuming exactly one
	 * entry leaves each level it visits — which is exactly what ApplyDeletePlan then does. Writes
	 * nothing; on any error the caller returns it with the tree untouched.
	 *
	 * Pins up to height - 1 siblings on top of the path: 2H frames at peak with the header.
	 */
	[[nodiscard]] Result<DeletePlan> PlanDelete(WritePath& path);

	/*
	 * Steps 4 and 5: apply `plan` bottom-up, after Delete has removed the entry from the leaf.
	 * Cannot fail — every view it takes was validated by FindLeafWrite or PlanDelete, so it
	 * asserts rather than checks.
	 *
	 * Frees nothing itself. Returns the page ids to free — each merge's right node, and the old
	 * root on a collapse — ALREADY UNLINKED from the tree. The caller drops every guard (path and
	 * plan) and only then DeletePage's each one, best effort: DeletePage rejects a pinned page,
	 * and a failed free is a leak, swallowed (DD-005, "A failed free is a leak, not an error").
	 */
	[[nodiscard]] std::vector<page_id_t> ApplyDeletePlan(WritePath& path, DeletePlan& plan);

	/*
	 * CheckInvariants' recursive walk: check the subtree rooted at `page_id`, then each child's
	 * subtree, left to right. False on the first violation, logged with the page it was found on.
	 *
	 * Checks, for this node: not seen before (recorded BEFORE the fetch, so a cycle of child
	 * pointers fails instead of recursing forever); page type and stored level both match
	 * `expected_level` (0 = leaf); the node's own CheckInvariants; occupancy against the cached
	 * fanout — the root exempt from the floor, an internal root needing 2 children; and every key
	 * inside [lo, hi). For an internal node the separators are held to the same interval, and child
	 * i is checked against [separator i, separator i + 1), child 0 inheriting this node's `lo` and
	 * the last child its `hi`.
	 *
	 * `hi` is exclusive, so std::nullopt is "no upper bound" — INT64_MAX is a legal key and cannot
	 * stand in for it. `lo` is inclusive, so INT64_MIN needs no such case.
	 *
	 * Copies out what it needs and drops the node's guard BEFORE recursing: one pin at a time, as
	 * on the read path, never one per level.
	 *
	 * `leaves` collects (leaf page id, its NextLeaf()) in walk order, which is key order. The leaf
	 * chain is checked against it by the caller once the walk is done — without a re-fetch, and
	 * without a chain walk that a cycle could hang.
	 */
	[[nodiscard]] bool CheckSubtree(page_id_t page_id, uint16_t expected_level, index_key_t lo,
	                                std::optional<index_key_t> hi, bool is_root,
	                                std::set<page_id_t>& seen_pages,
	                                std::vector<std::pair<page_id_t, page_id_t>>& leaves);

	BufferPoolManager& bpm_;
	const page_id_t header_page_id_;

	// Copies of the header's fanout, fixed at Create and validated at Open. Cached because they
	// never change; the root is deliberately NOT cached, because it does.
	const uint16_t leaf_max_;
	const uint16_t internal_max_;
};

}  // namespace kernsql

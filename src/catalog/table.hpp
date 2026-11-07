#pragma once

#include <memory>
#include <string>

#include "buffer/buffer_pool_manager.hpp"
#include "catalog/schema.hpp"
#include "common/status.hpp"
#include "common/types.hpp"
#include "heap/table_heap.hpp"
#include "indices/b_plus_tree.hpp"
#include "indices/b_plus_tree_page.hpp"

namespace kernsql {

/*
 * One table: a schema, a heap holding the rows, and a B+tree on the primary key mapping each key
 * to its row's RID (DD-006, "Keeping the heap and the index in sync").
 *
 * > EVERY WRITE GOES THROUGH THIS CLASS.
 *
 * A row lives in two places and every write changes both. The heap and the tree are private, and
 * the reads callers need are forwarded below, so nothing outside this class can change one
 * without the other. (They are not exposed const instead: TableHeap::Scan/Get and
 * BPlusTree::Get/Scan are non-const, because reading fetches pages.)
 *
 * ORDER, AND WHY. B+tree Insert and Delete are all-or-nothing, so the tree step goes wherever its
 * failure is free:
 *
 *   Insert:  Encode (validates) -> tree.Get (duplicate?) -> heap.Insert -> tree.Insert
 *            a failed tree.Insert is undone with heap.Delete
 *   Delete:  tree.Get -> tree.Delete -> heap.Delete
 *            a failed tree.Delete has changed nothing; a failed heap.Delete is undone with
 *            tree.Insert
 *
 * Each undo needs one frame, and while the pool has a single user the step before it has just
 * released at least one, so only an I/O error can fail it. If an undo does fail, the heap and the
 * index disagree — a documented no-WAL limitation (DD-006). It is logged, and the ORIGINAL error
 * is returned.
 *
 * THREADING: none of its own. Single-threaded until the per-table statement lock of stage 5,
 * which then covers this object exactly as it covers the tree.
 *
 * OWNS its heap and its tree (unique_ptrs), and through them nothing but page ids — no guards, so
 * a Table holds no pins between calls. Owned in turn by the Catalog, which is how its page ids
 * reach disk.
 */
class Table {
  public:
	/*
	 * Create an empty table: a new heap and a new index. All or nothing — if the index cannot be
	 * created, the heap's page is freed (best effort, as BPlusTree::Create frees its header page)
	 * and the error returned.
	 *
	 * `index_fanout` is for tests only (DD-006, "Testing"): at the default a table needs ~250 rows
	 * before its index has two levels, at MIN_FANOUT a handful. The tree persists it, so Open
	 * needs nothing. It sets both leaf_max and internal_max.
	 *
	 * Does not validate `name` or `schema` beyond what Schema::Create already did: the Catalog
	 * decides whether a name is free.
	 */
	[[nodiscard]] static Result<std::unique_ptr<Table>> Create(
	    BufferPoolManager& bpm, table_id_t table_id, std::string name, Schema schema,
	    uint16_t index_fanout = NODE_CAPACITY);

	/*
	 * Open a table from what the catalog stored. Reads the index header (BPlusTree::Open);
	 * nothing else.
	 *
	 * ONLY THE HEAP'S FIRST PAGE IS STORED. TableHeap also wants its last page, which moves every
	 * time the chain grows — too often to rewrite the catalog for. It is passed `first` for both:
	 * TableHeap::Insert already treats the last page as a hint and follows next_page_id forward
	 * from it under the extension latch, so the first append after an Open walks the chain once
	 * and caches the real end (DD-006, "Amendments to DD-004").
	 */
	[[nodiscard]] static Result<std::unique_ptr<Table>> Open(BufferPoolManager& bpm,
	                                                         table_id_t table_id, std::string name,
	                                                         Schema schema,
	                                                         page_id_t heap_first_page_id,
	                                                         page_id_t index_header_page_id);

	Table(const Table&) = delete;
	Table& operator=(const Table&) = delete;
	Table(Table&&) = delete;
	Table& operator=(Table&&) = delete;
	~Table();

	/*
	 * Insert `row`. In order, each check before anything is modified:
	 *   kInvalidArgument  the row does not fit the schema (arity, type, string length);
	 *   kDuplicateKey     its primary key is already present.
	 * After that, an error from the heap or the tree — kBufferPoolFull, kIOError — with the
	 * heap row undone (see the class comment).
	 */
	[[nodiscard]] Status Insert(const Row& row);

	/*
	 * Delete the row whose primary key is `key`. kNotFound if there is none, with nothing
	 * touched. Otherwise as Insert: an error from the tree means nothing changed; an error from
	 * the heap is undone.
	 */
	[[nodiscard]] Status Delete(index_key_t key);

	/*
	 * The row whose primary key is `key`. kNotFound if there is none.
	 *
	 * kCorruption — NOT kNotFound — when the index has the key but its RID names no heap row:
	 * the two have drifted apart, and reporting that as "absent" would hide the bug.
	 */
	[[nodiscard]] Result<Row> Get(index_key_t key);

	// ---- Reads for the executor's scan operators. None of these can modify anything. ----

	/*
	 * Every row, in heap order, as raw bytes plus RID — the SeqScan source. Decode each with
	 * GetSchema().Decode().
	 *
	 * TableIterator's rule applies unchanged: NEVER Insert or Delete on this table while the
	 * iterator is alive, or the call hangs on the iterator's own read latch. Collect, let it die,
	 * then mutate.
	 */
	[[nodiscard]] TableIterator Scan();

	/*
	 * (key, RID) for every key in [lo, hi], in key order — the IndexScan source. Fetch each row
	 * with RowAt. IndexIterator's rule is the same as TableIterator's above.
	 */
	[[nodiscard]] Result<IndexIterator> ScanIndex(index_key_t lo, index_key_t hi);

	/*
	 * The row at `rid`, decoded. For IndexScan, which already holds the RID from the index —
	 * Get(key) would search the tree a second time. kNotFound if the slot holds no row.
	 */
	[[nodiscard]] Result<Row> RowAt(RID rid);

	/*
	 * The test-only whole-table check (DD-006, "Testing"), this layer's CheckInvariants: scan
	 * the heap, and require that every row decodes, that its key is in the index with exactly
	 * that row's RID, that no key appears twice, that the heap row count equals the index entry
	 * count, and that the tree's own CheckInvariants passes. False on the first violation,
	 * logged.
	 *
	 * O(rows), and it holds one iterator's guard at a time. Never on a production path.
	 */
	[[nodiscard]] bool CheckConsistency();

	[[nodiscard]] table_id_t Id() const { return table_id_; }
	[[nodiscard]] const std::string& Name() const { return name_; }
	[[nodiscard]] const Schema& GetSchema() const { return schema_; }

	// What the catalog persists. Neither ever changes.
	[[nodiscard]] page_id_t HeapFirstPageId() const;
	[[nodiscard]] page_id_t IndexHeaderPageId() const;

  private:
	Table(table_id_t table_id, std::string name, Schema schema, std::unique_ptr<TableHeap> heap,
	      std::unique_ptr<BPlusTree> index);

	const table_id_t table_id_;
	const std::string name_;
	const Schema schema_;

	std::unique_ptr<TableHeap> heap_;
	std::unique_ptr<BPlusTree> index_;
};

}  // namespace kernsql

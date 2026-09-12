#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "buffer/buffer_pool_manager.hpp"
#include "buffer/page_guard.hpp"
#include "common/status.hpp"
#include "common/types.hpp"

namespace kernsql {

/*
 * One table's rows, physically (DD-004). A chain of HEAP pages linked through
 * PageHeader::next_page_id, plus everything HeapPage deliberately refuses to know.
 *
 * HeapPage arranges tuples inside ONE body. It cannot say which table it belongs to, cannot
 * reach another page, and answers "no room" with an error rather than a decision. TableHeap is
 * the layer that holds the chain and makes those decisions, which is what lets it turn a
 * meaningless slot_id into a RID{page_id, slot} that names a row for as long as the row lives.
 *
 * OWNS NOTHING IT USES. It holds a BufferPoolManager& — the same shape as BufferPoolManager
 * holding a DiskManager& — plus two page ids and a cursor. Pages belong to the pool, the file
 * belongs to the DiskManager, and the two page ids below belong, eventually, to the catalog.
 *
 * THREADING. Many threads may call one TableHeap at once. It defines no lock of its own: every
 * operation takes a page guard, works under that frame's content latch, and releases it, so a
 * heap page is single-threaded by construction. The one cross-page hazard is chain extension,
 * and it is solved with a latch that was already required — see Insert.
 *
 * NOT ITS JOB: what a row means (column types, nulls, encoding — it moves opaque blobs, exactly
 * like HeapPage), transaction locking (2PL, later), statement semantics such as the
 * collect-then-mutate rule DELETE and UPDATE must follow, and crash recovery (there is no WAL,
 * which is why nothing here spans pages atomically).
 */
class TableHeap;

/*
 * A forward scan over one table. Holds ONE page guard at a time and copies each tuple out.
 *
 * One guard at a time because the pool is fixed-size: a scan that pinned a frame per page it had
 * visited would exhaust it and then fail to fetch the next page of its own table.
 *
 * THE TUPLE BYTES ARE COPIED, and the copy lives in this object rather than in the page. That is
 * the ConstHeapPage lifetime rule at the one place it is easiest to reintroduce: advancing drops
 * the guard, the frame becomes evictable, and a span into the body then reads another page's
 * bytes as valid memory — which ASan and valgrind both consider fine. Tuple() therefore points
 * into this iterator, stays valid across the guard drop, and is invalidated by the next Next().
 *
 * Deliberately NOT an STL iterator. It is move-only, so it cannot satisfy forward_iterator; and
 * advancing fetches a page, so it can fail, which operator++ has no way to report. A cursor with
 * a fallible Next() says both of those out loud instead of hiding them behind a sentinel and a
 * throwing increment.
 */
class TableIterator {
  public:
	TableIterator() = delete;

	TableIterator(const TableIterator&) = delete;
	TableIterator& operator=(const TableIterator&) = delete;
	TableIterator(TableIterator&&) noexcept;
	TableIterator& operator=(TableIterator&&) noexcept;
	~TableIterator();

	/*
	 * Advance to the next live row. Returns false at the end of the table; an error means a
	 * fetch failed and the scan cannot continue.
	 *
	 * Must be called before the first Rid()/Tuple(): a freshly constructed iterator is
	 * positioned BEFORE the first row, so a scan is `while (*it.Next()) { ... }` with no
	 * special case for an empty table.
	 *
	 * Skips pages whose live_count is zero without walking their slot arrays — the reason that
	 * counter is maintained rather than derived.
	 */
	[[nodiscard]] Result<bool> Next();

	// The current row's identity. Always available, never behind a flag or a second iterator
	// type: the iterator already holds the page id to walk the chain and the slot id as its
	// cursor, so the RID is something it would otherwise throw away. SELECT ignores it, DELETE
	// ... WHERE needs it to name the row it matched, and a B+tree build needs {key -> RID} for
	// every row — that pair IS the index's payload. Postgres carries t_self on every seqscan
	// tuple for the same reason, with no caller opting in.
	[[nodiscard]] RID Rid() const;

	// The current row's bytes. Points into this iterator, NOT into the page, and is invalidated
	// by the next Next() — copy out anything that must outlive the step. Unlike the RID, which
	// is a value and stays meaningful forever.
	[[nodiscard]] std::span<const std::byte> Tuple() const;

  private:
	friend class TableHeap;

	TableIterator(BufferPoolManager& bpm, page_id_t first_page_id);

	BufferPoolManager* bpm_;

	// No default constructor on a guard, by design, so "no page held" has to be spelled with an
	// optional rather than an inert guard object.
	std::optional<ReadPageGuard> guard_;

	page_id_t page_id_;
	slot_id_t slot_;
	std::vector<std::byte> tuple_;
};

class TableHeap {
  public:
	/*
	 * Create a new, empty table: allocate one page, stamp page_type = HEAP through the guard,
	 * Init() its body, and leave next_page_id invalid. The caller persists FirstPageId() and
	 * LastPageId() — today into a test, later into the catalog.
	 *
	 * Open an existing one from the two ids the catalog handed back. Neither reads a page to
	 * validate the chain: the first fetch does that anyway, through AsHeapPage.
	 *
	 * Factories rather than constructors because both can fail, and unique_ptr rather than a
	 * value because insert_hint_ is an atomic and atomics are not movable — the same reason
	 * DiskManager::Open returns one.
	 */
	[[nodiscard]] static Result<std::unique_ptr<TableHeap>> Create(BufferPoolManager& bpm);
	[[nodiscard]] static Result<std::unique_ptr<TableHeap>> Open(BufferPoolManager& bpm,
	                                                             page_id_t first_page_id,
	                                                             page_id_t last_page_id);

	TableHeap(const TableHeap&) = delete;
	TableHeap& operator=(const TableHeap&) = delete;
	TableHeap(TableHeap&&) = delete;
	TableHeap& operator=(TableHeap&&) = delete;

	/*
	 * Insert a row and return the RID that names it.
	 *
	 * Tries candidate pages, and when every one is full the chain must be extended — the only
	 * place two threads collide in this layer.
	 *
	 * > THE WRITE LATCH ON THE CURRENT LAST PAGE IS THE EXTENSION LOCK.
	 *
	 * A thread that finds the last page full KEEPS its write guard while it calls NewPage,
	 * stamps HEAP, Inits, sets the old page's next_page_id, and publishes the new last_page_id.
	 * A second inserter blocks on that guard; when it acquires it, next_page_id is no longer
	 * INVALID_PAGE, so it follows the link instead of allocating. No new lock and no new
	 * ordering rule — the latch that was already required does the job.
	 *
	 * Getting it wrong is not subtle in consequence and very subtle in symptom: both threads
	 * allocate, both set next_page_id, one link is overwritten, and a page is leaked and
	 * unreachable forever with no error raised anywhere.
	 *
	 * Note this holds a content latch across NewPage, which can block on disk allocation. That
	 * is allowed: DD-002's prohibitions are about the metadata mutex and the shard locks. A
	 * content latch is held across I/O routinely — it is what the Loading state exists for.
	 *
	 * Errors: kInvalidArgument for an empty tuple or one over MAX_TUPLE_SIZE (HeapPage enforces
	 * both), and whatever NewPage reports when the chain cannot grow.
	 */
	[[nodiscard]] Result<RID> Insert(std::span<const std::byte> tuple);

	/*
	 * Read a row. Returns a COPY, and that is not negotiable: a span into a page body dies when
	 * this function's guard drops, at which point the frame may be evicted and refilled with a
	 * different page — still-valid memory holding another table's bytes, which no sanitizer
	 * flags. If per-row allocation ever shows up in a profile, add an overload that fills a
	 * caller-supplied buffer; do not "optimise" this by handing back a view.
	 *
	 * kNotFound for a row that has been deleted, or a slot that never existed. A RID handed out
	 * before a delete gets an answer, never bytes.
	 */
	[[nodiscard]] Result<std::vector<std::byte>> Get(RID rid);

	// kNotFound if the row is already gone. The bytes are not moved; the space is reclaimed by
	// the next compaction of that page.
	[[nodiscard]] Status Delete(RID rid);

	/*
	 * Overwrite a row and return the RID IT NOW HAS, which may differ from the one passed in.
	 *
	 * HeapPage::Update resolves to kSamePage or kDoesNotFit. The second case is this layer's to
	 * answer: the row outgrew its page, so it is deleted here and inserted elsewhere, and its
	 * identity changes. Returning the new RID forces every caller to hold the new value —
	 * returning a Status instead would let an index entry, or a collected RID list, go stale
	 * with nothing at the call site to notice.
	 *
	 * BEWARE THE RELOCATION DURING A SCAN. A row moved this way can land on a page the scan has
	 * not reached yet, match the predicate again, and be updated twice — the Halloween problem.
	 * Postgres is immune because a tuple written by the current command is invisible to it
	 * (cmin/cmax); this engine has no visibility machinery, so the executor must collect RIDs to
	 * completion first and only then apply. That rule is imposed by this layer and enforced
	 * above it.
	 *
	 * kNotFound if the row is gone.
	 */
	[[nodiscard]] Result<RID> Update(RID rid, std::span<const std::byte> tuple);

	// A scan positioned before the first row; call Next() to reach it.
	[[nodiscard]] TableIterator Scan();

	// For the catalog to persist. LastPageId() changes as the chain grows, which is exactly why
	// whoever stores it has to be told again.
	[[nodiscard]] page_id_t FirstPageId() const { return first_page_id_; }
	[[nodiscard]] page_id_t LastPageId() const { return last_page_id_.load(); }

  private:
	TableHeap(BufferPoolManager& bpm, page_id_t first_page_id, page_id_t last_page_id);

	BufferPoolManager& bpm_;

	// Fixed for the life of the table. The chain is only ever appended to.
	const page_id_t first_page_id_;

	// Atomic because Insert publishes it under the extension latch but every other caller reads
	// it without one.
	std::atomic<page_id_t> last_page_id_;

	/*
	 * v1 free-space reuse (DD-004): where the next Insert starts looking. Rotates along the
	 * chain for a bounded number of probes and falls back to appending at the last page.
	 *
	 * A HINT, NEVER A TRUTH. Never persisted, re-verified against the real page before use, and
	 * harmless when stale — a wrong value costs one wasted fetch, never corruption. That single
	 * property is what lets a cross-page structure exist in an engine with no WAL, and it is why
	 * this needs no latch of its own and imposes no lock-ordering rule.
	 *
	 * Its honest weakness is that it stumbles into space rather than finding it. When that stops
	 * being enough, DD-004 designs a per-table free-space map to replace it; the FSM changes
	 * nothing about the on-disk page format, which is why it can wait.
	 */
	std::atomic<page_id_t> insert_hint_;
};

}  // namespace kernsql

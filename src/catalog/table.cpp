#include "catalog/table.hpp"

#include <cstddef>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "catalog/schema.hpp"
#include "common/logger.hpp"
#include "common/status.hpp"
#include "heap/table_heap.hpp"
#include "indices/b_plus_tree.hpp"

using namespace kernsql;

Result<std::unique_ptr<Table>> Table::Create(BufferPoolManager& bpm, table_id_t table_id,
                                             std::string name, Schema schema,
                                             uint16_t index_fanout) {
	auto table_heap = TableHeap::Create(bpm);
	if (!table_heap.has_value()) return std::unexpected(table_heap.error());

	auto bplus_tree = BPlusTree::Create(bpm, index_fanout, index_fanout);
	if (!bplus_tree.has_value()) {
		(void)bpm.DeletePage(table_heap.value()->FirstPageId());  // best-effort release table heap
		return std::unexpected(bplus_tree.error());
	}

	return std::unique_ptr<Table>(new Table(table_id, std::move(name), std::move(schema),
	                                        std::move(table_heap.value()),
	                                        std::move(bplus_tree.value())));
}

Result<std::unique_ptr<Table>> Table::Open(BufferPoolManager& bpm, table_id_t table_id,
                                           std::string name, Schema schema,
                                           page_id_t heap_first_page_id,
                                           page_id_t index_header_page_id) {
	auto table_heap = TableHeap::Open(bpm, heap_first_page_id, heap_first_page_id);
	if (!table_heap.has_value()) return std::unexpected(table_heap.error());

	auto bplus_tree = BPlusTree::Open(bpm, index_header_page_id);
	if (!bplus_tree.has_value()) return std::unexpected(bplus_tree.error());

	return std::unique_ptr<Table>(new Table(table_id, std::move(name), std::move(schema),
	                                        std::move(table_heap.value()),
	                                        std::move(bplus_tree.value())));
}

Table::Table(table_id_t table_id, std::string name, Schema schema, std::unique_ptr<TableHeap> heap,
             std::unique_ptr<BPlusTree> index)
    : table_id_(table_id),
      name_(std::move(name)),
      schema_(std::move(schema)),
      heap_(std::move(heap)),
      index_(std::move(index)) {}

Status Table::Insert(const Row& row) {
	// Encode validates, so this is also the kInvalidArgument check.
	auto bytes = schema_.Encode(row);
	if (!bytes.has_value()) return bytes.error();
	const index_key_t key = schema_.KeyOf(row);

	// Duplicate check before the heap is touched (DD-005). Safe because there is one writer: no
	// one can insert this key between the probe and the tree insert below.
	auto existing = index_->Get(key);
	if (existing.has_value()) {
		return Status::DuplicateKey("key " + std::to_string(key) + " already exists in " + name_);
	}
	if (existing.error().code() != ErrorCode::kNotFound) return existing.error();

	auto rid = heap_->Insert(bytes.value());
	if (!rid.has_value()) return rid.error();

	// The tree needs the RID, so it cannot go first, and its failure needs an undo.
	Status st = index_->Insert(key, rid.value());
	if (st.ok()) return st;

	// Needs one frame, and heap_->Insert just released one, so while the pool has a single user
	// only an I/O error can fail this. If it does, the heap holds a row the index doesn't know —
	// a documented no-WAL limitation (DD-006). Report the error that caused all this.
	if (Status undo = heap_->Delete(rid.value()); !undo.ok()) {
		const auto msg = undo.message();
		LOG_INFO("table %s: orphan heap row (%d, %u) for key %lld, undo failed: %.*s",
		         name_.c_str(), rid->page_id, static_cast<unsigned>(rid->slot),
		         static_cast<long long>(key), static_cast<int>(msg.size()), msg.data());
	}
	return st;
}

Status Table::Delete(index_key_t key) {
	auto rid = index_->Get(key);
	if (!rid.has_value()) return rid.error();  // kNotFound included, nothing touched

	// Tree first: Delete is all or nothing, so a failure here has changed nothing.
	if (Status st = index_->Delete(key); !st.ok()) return st;

	Status st = heap_->Delete(rid.value());
	if (st.ok()) return st;

	// Needs frames index_->Delete just released, so while the pool has a single user only an I/O
	// error gets here. Put the index entry back so the row stays whole; if that fails too, the
	// heap holds a row the index doesn't know — the same no-WAL limitation as in Insert.
	if (Status undo = index_->Insert(key, rid.value()); !undo.ok()) {
		const auto msg = undo.message();
		LOG_INFO("table %s: orphan heap row (%d, %u) for key %lld, undo failed: %.*s",
		         name_.c_str(), rid->page_id, static_cast<unsigned>(rid->slot),
		         static_cast<long long>(key), static_cast<int>(msg.size()), msg.data());
	}
	return st;
}

Result<Row> Table::Get(index_key_t key) {
	auto rid = index_->Get(key);
	if (!rid.has_value()) return std::unexpected(rid.error());

	auto bytes = heap_->Get(rid.value());
	if (!bytes.has_value()) {
		if (bytes.error().code() != ErrorCode::kNotFound) return std::unexpected(bytes.error());
		return std::unexpected(
		    Status::Corruption("table " + name_ + ": index maps key " + std::to_string(key) +
		                       " to (" + std::to_string(rid->page_id) + ", " +
		                       std::to_string(rid->slot) + "), which holds no row"));
	}
	return schema_.Decode(bytes.value());
}

TableIterator Table::Scan() {
	return heap_->Scan();
}

Result<IndexIterator> Table::ScanIndex(index_key_t lo, index_key_t hi) {
	return index_->Scan(lo, hi);
}

Result<Row> Table::RowAt(RID rid) {
	auto bytes = heap_->Get(rid);
	if (!bytes.has_value()) return std::unexpected(bytes.error());
	return schema_.Decode(bytes.value());
}

bool Table::CheckConsistency() {
	std::map<index_key_t, RID> heap_rows;
	{
		TableIterator it = heap_->Scan();
		while (true) {
			auto more = it.Next();
			if (!more.has_value()) {
				LOG_DEBUG("table %s: heap scan failed", name_.c_str());
				return false;
			}
			if (!more.value()) break;

			const RID rid = it.Rid();
			auto row = schema_.Decode(it.Tuple());
			if (!row.has_value()) {
				LOG_DEBUG("table %s: row (%d, %u) does not decode", name_.c_str(), rid.page_id,
				          static_cast<unsigned>(rid.slot));
				return false;
			}
			const index_key_t key = schema_.KeyOf(row.value());
			if (!heap_rows.emplace(key, rid).second) {
				LOG_DEBUG("table %s: key %lld appears twice in the heap", name_.c_str(),
				          static_cast<long long>(key));
				return false;
			}
		}
	}

	size_t index_entries = 0;
	{
		auto it = index_->Scan(std::numeric_limits<index_key_t>::min(),
		                       std::numeric_limits<index_key_t>::max());
		if (!it.has_value()) {
			LOG_DEBUG("table %s: index scan failed to start", name_.c_str());
			return false;
		}
		while (true) {
			auto more = it->Next();
			if (!more.has_value()) {
				LOG_DEBUG("table %s: index scan failed", name_.c_str());
				return false;
			}
			if (!more.value()) break;

			++index_entries;
			auto found = heap_rows.find(it->Key());
			if (found == heap_rows.end()) {
				LOG_DEBUG("table %s: index key %lld has no heap row", name_.c_str(),
				          static_cast<long long>(it->Key()));
				return false;
			}
			if (found->second != it->Rid()) {
				LOG_DEBUG("table %s: index maps key %lld to (%d, %u), heap row is at (%d, %u)",
				          name_.c_str(), static_cast<long long>(it->Key()), it->Rid().page_id,
				          static_cast<unsigned>(it->Rid().slot), found->second.page_id,
				          static_cast<unsigned>(found->second.slot));
				return false;
			}
		}
	}

	if (index_entries != heap_rows.size()) {
		LOG_DEBUG("table %s: %zu heap rows but %zu index entries", name_.c_str(), heap_rows.size(),
		          index_entries);
		return false;
	}

	return index_->CheckInvariants();
}

Table::~Table() = default;

page_id_t Table::HeapFirstPageId() const {
	return heap_->FirstPageId();
}

page_id_t Table::IndexHeaderPageId() const {
	return index_->HeaderPageId();
}

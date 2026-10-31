#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>

#include "buffer/buffer_pool_manager.hpp"
#include "catalog/catalog.hpp"
#include "common/status.hpp"
#include "storage/disk_manager.hpp"

namespace kernsql {

/*
 * One open database file, and the one object everything above storage starts from — tests,
 * then the executor and the shell (DD-006, "Database").
 *
 * Builds the stack bottom-up and owns all of it: DiskManager, then the BufferPoolManager on top,
 * then the Catalog on top of that. The members are declared in that order, so they are destroyed
 * in reverse — the catalog's tables before the pool they read through, the pool before the file
 * it flushes to.
 *
 * Holds no pins: Catalog, Table, TableHeap and BPlusTree all hold page ids, never guards.
 *
 * > SHUTDOWN IS THE DURABILITY POINT.
 *
 * As in DD-002, nothing is on disk until Shutdown() succeeds. Destroying a Database without it is
 * a caller bug, handled exactly as the pool's own destructor handles it — log, flush best effort,
 * abort. This class does not invent a second policy.
 */
class Database {
  public:
	// 256 frames is 1 MiB of pool — plenty for the shell. Tests pass their own.
	static constexpr std::size_t DEFAULT_POOL_FRAMES = 256;

	/*
	 * Open `path`, creating it if it does not exist, and load its catalog. Errors from
	 * DiskManager::Open and Catalog::Open are returned as they come; on any error nothing is left
	 * open.
	 */
	[[nodiscard]] static Result<std::unique_ptr<Database>> Open(
	    const std::filesystem::path& path, std::size_t pool_frames = DEFAULT_POOL_FRAMES);

	Database(const Database&) = delete;
	Database& operator=(const Database&) = delete;
	Database(Database&&) = delete;
	Database& operator=(Database&&) = delete;
	~Database();

	// BufferPoolManager::Shutdown(): verify nothing is pinned, flush every dirty page, sync the
	// file. Idempotent, as the pool's is.
	[[nodiscard]] Status Shutdown();

	[[nodiscard]] Catalog& GetCatalog() { return *catalog_; }

	// For tests that pin frames or read pool stats. Nothing on a production path needs it.
	[[nodiscard]] BufferPoolManager& Pool() { return *bpm_; }

  private:
	Database(std::unique_ptr<DiskManager> dm, std::unique_ptr<BufferPoolManager> bpm,
	         std::unique_ptr<Catalog> catalog);

	// DECLARATION ORDER IS DESTRUCTION ORDER, reversed. Do not reorder.
	std::unique_ptr<DiskManager> dm_;
	std::unique_ptr<BufferPoolManager> bpm_;
	std::unique_ptr<Catalog> catalog_;
};

}  // namespace kernsql

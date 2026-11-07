#include "catalog/database.hpp"

#include <expected>
#include <memory>
#include <utility>

using namespace kernsql;

Result<std::unique_ptr<Database>> Database::Open(const std::filesystem::path& path,
                                                 std::size_t pool_frames) {
	auto dm = DiskManager::Open(path);
	if (!dm.has_value()) return std::unexpected(dm.error());

	auto bpm = std::make_unique<BufferPoolManager>(*dm.value(), pool_frames);

	auto catalog = Catalog::Open(*bpm);
	if (!catalog.has_value()) {
		// A failed Catalog::Open holds no pins. Shut the pool down properly, or its destructor
		// takes the caller-bug path (DD-002). Its own error, if any, is secondary to this one.
		(void)bpm->Shutdown();
		return std::unexpected(catalog.error());
	}

	return std::unique_ptr<Database>(
	    new Database(std::move(dm.value()), std::move(bpm), std::move(catalog.value())));
}

Database::Database(std::unique_ptr<DiskManager> dm, std::unique_ptr<BufferPoolManager> bpm,
                   std::unique_ptr<Catalog> catalog)
    : dm_(std::move(dm)), bpm_(std::move(bpm)), catalog_(std::move(catalog)) {}

Database::~Database() = default;

Status Database::Shutdown() {
	return bpm_->Shutdown();
}

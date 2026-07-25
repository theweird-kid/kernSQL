#include <filesystem>
#include <print>

#include "common/status.hpp"
#include "storage/disk_manager.hpp"

int main() {
	using namespace kernsql;

	std::println("Hello kernSQL");
	Status s = Status::OK();
	std::println("Testing: {}", s.message());

	auto DB_PATH = std::filesystem::temp_directory_path() / "kernsql.db";
	auto dm = DiskManager::Open(DB_PATH);
	if (dm) return 0;
}

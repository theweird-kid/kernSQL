#include <print>

#include "common/status.hpp"

int main() {
	using namespace kernsql;

	std::println("Hello kernSQL");
	Status s = Status::OK();
	std::println("Testing: {}", s.message());
	return 0;
}

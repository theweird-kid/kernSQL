#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace kernsql {

enum class ErrorCode : uint8_t {
	kOk = 0,
	kNotFound,
	kCorruption,
	kIOError,
	kInvalidArgument,
	kBufferPoolFull,
	kPageFull,
	kDuplicateKey,
	kSerializationConflict,
	kInternal,
};

class [[nodiscard]] Status {
  public:
	static Status OK() { return Status{}; }
	static Status IOError(std::string msg) { return {ErrorCode::kIOError, std::move(msg)}; }
	static Status NotFound(std::string msg) { return {ErrorCode::kNotFound, std::move(msg)}; }
	static Status Corruption(std::string msg) { return {ErrorCode::kCorruption, std::move(msg)}; }
	static Status InvalidArgument(std::string msg) {
		return {ErrorCode::kInvalidArgument, std::move(msg)};
	}
	static Status BufferPoolFull(std::string msg) {
		return {ErrorCode::kBufferPoolFull, std::move(msg)};
	}
	// A page has no room for the write. Distinct from kBufferPoolFull, which means the pool has
	// no free frame: that one is fatal to the operation, this one is a routing decision — the
	// caller can compact this page and retry, or place the row on a different page.
	static Status PageFull(std::string msg) { return {ErrorCode::kPageFull, std::move(msg)}; }
	static Status DuplicateKey(std::string msg) {
		return {ErrorCode::kDuplicateKey, std::move(msg)};
	}
	static Status SerializationConflict(std::string msg) {
		return {ErrorCode::kSerializationConflict, std::move(msg)};
	}
	static Status Internal(std::string msg) { return {ErrorCode::kInternal, std::move(msg)}; }

	bool ok() const { return code_ == ErrorCode::kOk; }
	ErrorCode code() const { return code_; }
	std::string_view message() const { return msg_; }

  private:
	Status() = default;
	Status(ErrorCode c, std::string m) : code_(c), msg_(std::move(m)) {}
	ErrorCode code_{ErrorCode::kOk};
	std::string msg_;
};

template <typename T>
using Result = std::expected<T, Status>;

}  // namespace kernsql

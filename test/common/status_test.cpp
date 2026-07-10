#include "common/status.hpp"

#include <gtest/gtest.h>

namespace kernsql {
namespace {

TEST(StatusTest, OkHasNoMessageAndOkCode) {
	Status s = Status::OK();
	EXPECT_TRUE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kOk);
	EXPECT_TRUE(s.message().empty());
}

TEST(StatusTest, IOErrorCarriesCodeAndMessage) {
	Status s = Status::IOError("disk full");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kIOError);
	EXPECT_EQ(s.message(), "disk full");
}

TEST(StatusTest, NotFoundCarriesCodeAndMessage) {
	Status s = Status::NotFound("no such key");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kNotFound);
	EXPECT_EQ(s.message(), "no such key");
}

TEST(StatusTest, CorruptionCarriesCodeAndMessage) {
	Status s = Status::Corruption("bad checksum");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kCorruption);
	EXPECT_EQ(s.message(), "bad checksum");
}

TEST(StatusTest, InvalidArgumentCarriesCodeAndMessage) {
	Status s = Status::InvalidArgument("bad column type");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kInvalidArgument);
	EXPECT_EQ(s.message(), "bad column type");
}

TEST(StatusTest, BufferPoolFullCarriesCodeAndMessage) {
	Status s = Status::BufferPoolFull("no free frames");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kBufferPoolFull);
	EXPECT_EQ(s.message(), "no free frames");
}

TEST(StatusTest, DuplicateKeyCarriesCodeAndMessage) {
	Status s = Status::DuplicateKey("pk violation");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kDuplicateKey);
	EXPECT_EQ(s.message(), "pk violation");
}

TEST(StatusTest, SerializationConflictCarriesCodeAndMessage) {
	Status s = Status::SerializationConflict("write-write conflict");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kSerializationConflict);
	EXPECT_EQ(s.message(), "write-write conflict");
}

TEST(StatusTest, InternalCarriesCodeAndMessage) {
	Status s = Status::Internal("unreachable");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code(), ErrorCode::kInternal);
	EXPECT_EQ(s.message(), "unreachable");
}

TEST(ResultTest, HoldsValueOnSuccess) {
	Result<int> r = 42;
	ASSERT_TRUE(r.has_value());
	EXPECT_EQ(*r, 42);
}

TEST(ResultTest, HoldsStatusOnFailure) {
	Result<int> r = std::unexpected(Status::NotFound("missing"));
	ASSERT_FALSE(r.has_value());
	EXPECT_EQ(r.error().code(), ErrorCode::kNotFound);
	EXPECT_EQ(r.error().message(), "missing");
}

}  // namespace
}  // namespace kernsql

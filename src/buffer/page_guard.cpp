#include "page_guard.hpp"

#include <utility>

#include "buffer_pool_manager.hpp"

using namespace kernsql;

ReadPageGuard& ReadPageGuard::operator=(ReadPageGuard&& other) noexcept {
	if (this == &other) return *this;

	Drop();

	bpm_ = std::exchange(other.bpm_, nullptr);
	frame_ = std::exchange(other.frame_, nullptr);
	frame_id_ = other.frame_id_;
	page_id_ = other.page_id_;
	latch_ = std::move(other.latch_);

	return *this;
}

void ReadPageGuard::Drop() {
	if (bpm_ == nullptr) return;

	latch_.unlock();
	bpm_->UnpinPage(frame_id_);

	bpm_ = nullptr;
	frame_ = nullptr;
}

WritePageGuard& WritePageGuard::operator=(WritePageGuard&& other) noexcept {
	if (this == &other) return *this;

	Drop();

	bpm_ = std::exchange(other.bpm_, nullptr);
	frame_ = std::exchange(other.frame_, nullptr);
	frame_id_ = other.frame_id_;
	page_id_ = other.page_id_;
	latch_ = std::move(other.latch_);

	return *this;
}

void WritePageGuard::Drop() {
	if (bpm_ == nullptr) return;

	frame_->MarkDirty();
	latch_.unlock();
	bpm_->UnpinPage(frame_id_);

	bpm_ = nullptr;
	frame_ = nullptr;
}

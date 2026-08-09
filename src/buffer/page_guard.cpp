#include "page_guard.hpp"

#include "buffer_pool_manager.hpp"

using namespace kernsql;

void ReadPageGuard::Drop() {
	if (bpm_ == nullptr) return;

	latch_.unlock();
	bpm_->UnpinPage(frame_id_);

	bpm_ = nullptr;
	frame_ = nullptr;
}

void WritePageGuard::Drop() {
	if (bpm_ == nullptr) return;

	frame_->MarkDirty();
	latch_.unlock();
	bpm_->UnpinPage(frame_id_);

	bpm_ = nullptr;
	frame_ = nullptr;
}

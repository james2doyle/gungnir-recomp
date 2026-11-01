#include "ge/frame_queue.hpp"

#include <utility>

namespace gungnir::ge {

void FrameQueue::publish(std::vector<Draw> draws) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_frame_) ++dropped_;
    latest_ = std::move(draws);
    has_frame_ = true;
    ++published_;
}

bool FrameQueue::take_latest(std::vector<Draw> &draws) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_) return false;
    draws = std::move(latest_);
    latest_.clear();
    has_frame_ = false;
    return true;
}

std::uint64_t FrameQueue::published() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

std::uint64_t FrameQueue::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

} // namespace gungnir::ge

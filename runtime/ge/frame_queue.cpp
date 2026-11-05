#include "ge/frame_queue.hpp"

#include <utility>

namespace gungnir::ge {

void FrameQueue::publish(std::vector<Draw> draws, Clock::time_point present_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The presenter did not take the previous frame before this one arrived:
    // the host fell behind, so the newest frame wins and the loss is counted.
    if (has_frame_) ++dropped_;
    latest_ = std::move(draws);
    present_at_ = present_at;
    has_frame_ = true;
    ++published_;
    due_.notify_all();
}

bool FrameQueue::take_due(Clock::time_point now, std::vector<Draw> &draws, bool &late) {
    late = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_ || now < present_at_) return false;
    late = now - present_at_ > kLateThreshold;
    if (late) {
        ++late_;
        last_late_us_ = std::chrono::duration_cast<std::chrono::microseconds>(now - present_at_).count();
    }
    draws = std::move(latest_);
    latest_.clear();
    has_frame_ = false;
    ++presented_;
    return true;
}

std::optional<FrameQueue::Clock::time_point> FrameQueue::pending_deadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_frame_) return std::nullopt;
    return present_at_;
}

FrameQueue::Clock::duration idle_wait_slice(FrameQueue::Clock::time_point now,
    std::optional<FrameQueue::Clock::time_point> deadline,
    FrameQueue::Clock::duration ceiling) noexcept {
    if (!deadline.has_value()) return ceiling;
    const auto remaining = *deadline - now;
    if (remaining <= FrameQueue::Clock::duration::zero())
        return FrameQueue::Clock::duration::zero();
    return remaining < ceiling ? remaining : ceiling;
}

void FrameQueue::wait_for_next(Clock::duration max_wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    const Clock::time_point limit = Clock::now() + max_wait;
    for (;;) {
        if (has_frame_) {
            if (Clock::now() >= present_at_) return;
            if (present_at_ > limit) {
                due_.wait_until(lock, limit);
                return;
            }
            due_.wait_until(lock, present_at_);
            continue;
        }
        if (Clock::now() >= limit) return;
        due_.wait_until(lock, limit);
    }
}

std::uint64_t FrameQueue::published() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

std::uint64_t FrameQueue::presented() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return presented_;
}

std::uint64_t FrameQueue::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

std::uint64_t FrameQueue::late() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return late_;
}

std::int64_t FrameQueue::last_late_us() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_late_us_;
}

} // namespace gungnir::ge

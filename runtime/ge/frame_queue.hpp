#pragma once

// One-frame handoff between the guest session thread and the presentation
// thread (T13b), with the moment the frame is due (T17a.2).
//
// The guest publishes at its flip, carrying the host time that flip maps to
// (the kernel's pacing hold knows it). The presenter takes a frame only once
// that moment has arrived, so a frame is shown when the guest produced it
// rather than one fixed sleep later. Publishing over a frame the presenter has
// not taken yet is a drop: the host fell behind, the newest frame wins, and
// the loss is counted and named instead of hidden.

#include "ge/state_machine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace gungnir::ge {

class FrameQueue {
public:
    using Clock = std::chrono::steady_clock;
    // A present later than this past its deadline is counted as late. A frame
    // cannot be shown before it exists (the deadline is its flip moment), and
    // waking the presenter costs a scheduler slice, so a few milliseconds are
    // the presenter's own latency, not a miss. Past this, the frame is stale
    // when it reaches the screen.
    static constexpr std::chrono::milliseconds kLateThreshold{5};

    void publish(std::vector<Draw> draws, Clock::time_point present_at);
    // True when a frame was pending and its deadline has arrived. `late` is set
    // when it was taken past kLateThreshold, which means the host fell behind
    // without losing the frame.
    bool take_due(Clock::time_point now, std::vector<Draw> &draws, bool &late);
    // Deadline of the pending frame, empty when nothing is waiting.
    [[nodiscard]] std::optional<Clock::time_point> pending_deadline() const;
    // Blocks until a frame is due or `max_wait` elapses, without taking it.
    // Publishing wakes the presenter immediately, so a frame is never delayed
    // by the loop's own sleep slice.
    void wait_for_next(Clock::duration max_wait);

    [[nodiscard]] std::uint64_t published() const;
    [[nodiscard]] std::uint64_t presented() const;
    [[nodiscard]] std::uint64_t dropped() const;
    [[nodiscard]] std::uint64_t late() const;
    // How late the last late present was, in microseconds (0 when none).
    [[nodiscard]] std::int64_t last_late_us() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable due_;
    std::vector<Draw> latest_;
    Clock::time_point present_at_{};
    bool has_frame_{};
    std::uint64_t published_{};
    std::uint64_t presented_{};
    std::uint64_t dropped_{};
    std::uint64_t late_{};
    std::int64_t last_late_us_{};
};

// Host idle-wait policy (behavioural reference: PPSSPP's WaitUntil()). How long
// the main loop may sleep before its next deadline: exactly the time to that
// deadline, but never more than `ceiling`, so SDL_PollEvent keeps touch and the
// overlay responsive when no input event can wake the wait. A missing or
// already-past deadline yields `ceiling` or zero respectively.
[[nodiscard]] FrameQueue::Clock::duration idle_wait_slice(FrameQueue::Clock::time_point now,
    std::optional<FrameQueue::Clock::time_point> deadline,
    FrameQueue::Clock::duration ceiling) noexcept;

} // namespace gungnir::ge

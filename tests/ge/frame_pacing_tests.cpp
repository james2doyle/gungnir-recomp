// T17a.2 — present-pacing contracts.
//
// A guest frame is due at the host moment its flip maps to. With injected
// time, N produced frames must produce N presents in order, never two presents
// of one frame and never a present of a frame the guest did not produce. A
// host that falls behind drops the pending frame (counted) or presents late
// (counted); neither may corrupt the order.
//
// Red-first: written before FrameQueue carried a deadline.

#include "ge/frame_queue.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using gungnir::ge::Draw;
using gungnir::ge::FrameQueue;
using Clock = FrameQueue::Clock;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

Draw frame(float id) {
    Draw draw;
    draw.vertices.resize(1u);
    draw.vertices[0].x = id;
    return draw;
}

Clock::time_point at_ms(std::int64_t milliseconds) {
    return Clock::time_point{} + std::chrono::milliseconds(milliseconds);
}

void one_present_per_produced_frame_in_order() {
    FrameQueue queue;
    for (int i = 0; i < 5; ++i) {
        queue.publish({frame(static_cast<float>(i))}, at_ms(50 * i));
        std::vector<Draw> out;
        bool late = false;
        check(!queue.take_due(at_ms(50 * i) - std::chrono::milliseconds(1), out, late),
            "a frame is not due before its deadline");
        check(queue.take_due(at_ms(50 * i), out, late),
            "a frame is due at its deadline");
        check(out.size() == 1u && out[0].vertices[0].x == static_cast<float>(i),
            "frames present in the order the guest produced them");
        check(!late, "an on-time present is not late");
        check(!queue.take_due(at_ms(50 * i) + std::chrono::milliseconds(1), out, late),
            "one frame presents exactly once");
    }
    check(queue.published() == 5u, "every produced frame is counted");
    check(queue.presented() == 5u, "every produced frame is presented");
    check(queue.dropped() == 0u, "a host that keeps up drops nothing");
    check(queue.late() == 0u, "a host that keeps up presents nothing late");
}

void overdue_frame_presents_immediately() {
    FrameQueue queue;
    queue.publish({frame(7.0f)}, at_ms(0));
    std::vector<Draw> out;
    bool late = false;
    check(queue.take_due(at_ms(1), out, late), "an overdue frame presents on the next take");
    check(!late, "a present inside the late threshold is on time");
    queue.publish({frame(8.0f)}, at_ms(10));
    check(queue.take_due(at_ms(17), out, late), "a late frame is still presented, never skipped");
    check(late, "a present past the threshold is counted as late");
    check(queue.late() == 1u, "the late counter tracks it");
}

void a_frame_still_pending_is_dropped_counted() {
    FrameQueue queue;
    queue.publish({frame(1.0f)}, at_ms(10));
    queue.publish({frame(2.0f)}, at_ms(20));
    check(queue.dropped() == 1u, "an untaken frame replaced by a newer one is a counted drop");
    std::vector<Draw> out;
    bool late = false;
    check(queue.take_due(at_ms(20), out, late), "the newest frame is the one presented");
    check(out.size() == 1u && out[0].vertices[0].x == 2.0f, "the newest frame wins");
    check(queue.presented() == 1u && queue.published() == 2u,
        "a drop is a lost present, not a lost production");
}

void waiting_returns_as_soon_as_a_frame_is_due() {
    FrameQueue queue;
    queue.publish({frame(4.0f)}, Clock::now());
    const auto started = Clock::now();
    queue.wait_for_next(std::chrono::milliseconds(8));
    const auto elapsed = Clock::now() - started;
    check(elapsed < std::chrono::milliseconds(4),
        "an already-due frame does not wait out the slice");
}

void waiting_without_a_frame_is_bounded() {
    FrameQueue queue;
    const auto started = Clock::now();
    queue.wait_for_next(std::chrono::milliseconds(3));
    const auto elapsed = Clock::now() - started;
    check(elapsed >= std::chrono::milliseconds(2) && elapsed < std::chrono::milliseconds(100),
        "an empty queue still returns in about the requested slice");
}

void pending_deadline_tracks_the_publish() {
    FrameQueue queue;
    check(!queue.pending_deadline().has_value(), "an empty queue has no deadline");
    queue.publish({frame(3.0f)}, at_ms(40));
    const auto deadline = queue.pending_deadline();
    check(deadline.has_value() && *deadline == at_ms(40), "the deadline follows the frame");
    std::vector<Draw> out;
    bool late = false;
    (void)queue.take_due(at_ms(40), out, late);
    check(!queue.pending_deadline().has_value(), "taking the frame clears the deadline");
}

} // namespace

int main() {
    try {
        one_present_per_produced_frame_in_order();
        overdue_frame_presents_immediately();
        a_frame_still_pending_is_dropped_counted();
        waiting_returns_as_soon_as_a_frame_is_due();
        waiting_without_a_frame_is_bounded();
        pending_deadline_tracks_the_publish();
        std::cout << "frame_pacing_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

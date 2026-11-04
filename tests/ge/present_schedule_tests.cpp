// T17a.6 — display schedule contracts for in-between frames.
//
// Time is injected, so the whole point is pinned without a GPU: one window per
// guest frame, one present per refresh inside it, the real frame at the end,
// nothing at all when the toggle is off, and a skipped tick, never a burst,
// when the presenter cannot hold the cadence.
//
// Red-first: written before runtime/ge/present_schedule.* existed.

#include "ge/present_schedule.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

using gungnir::ge::PresentSchedule;
using Clock = PresentSchedule::Clock;
using Kind = PresentSchedule::Kind;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void check_near(float actual, float expected, const char *contract) {
    if (actual < expected - 0.01f || actual > expected + 0.01f) throw std::runtime_error(contract);
}

Clock::time_point at_ms(std::int64_t milliseconds) {
    return Clock::time_point{} + std::chrono::milliseconds(milliseconds);
}

PresentSchedule schedule() {
    PresentSchedule schedule;
    schedule.set_enabled(true);
    schedule.set_refresh_interval(std::chrono::milliseconds(10));
    return schedule;
}

void off_presents_every_frame_at_once() {
    PresentSchedule idle;
    idle.set_refresh_interval(std::chrono::milliseconds(10));
    check(idle.on_guest_frame(at_ms(0), true), "interpolation off presents the frame at once");
    check(!idle.active(), "interpolation off starts no window");
    check(idle.due(at_ms(50)).kind == Kind::None, "interpolation off never presents in between");
}

void the_first_frame_has_nothing_to_interpolate() {
    PresentSchedule s = schedule();
    check(s.on_guest_frame(at_ms(0), false), "the first frame is presented immediately");
    check(!s.active(), "no window without a previous frame");
    check(s.due(at_ms(10)).kind == Kind::None, "and no in-betweens either");
}

void a_window_presents_one_in_between_per_refresh() {
    PresentSchedule s = schedule();
    (void)s.on_guest_frame(at_ms(0), false);
    check(!s.on_guest_frame(at_ms(50), true), "the second frame opens a display window");
    check(s.active(), "the window is live");
    check_near(s.due(at_ms(50) + std::chrono::milliseconds(10)).t, 0.2f,
        "the first tick is one refresh into a 50 ms window");
    check_near(s.due(at_ms(50) + std::chrono::milliseconds(20)).t, 0.4f,
        "the second tick is two refreshes in");
    check_near(s.due(at_ms(50) + std::chrono::milliseconds(30)).t, 0.6f,
        "the third tick is three refreshes in");
    check(s.due(at_ms(50) + std::chrono::milliseconds(35)).kind == Kind::None,
        "no tick between refreshes");
}

void no_tick_before_its_time_or_twice() {
    PresentSchedule s = schedule();
    (void)s.on_guest_frame(at_ms(0), false);
    (void)s.on_guest_frame(at_ms(50), true);
    check(s.due(at_ms(55)).kind == Kind::None, "a tick is not early");
    check(s.due(at_ms(60)).kind == Kind::Interpolated, "the tick fires at its time");
    check(s.due(at_ms(60)).kind == Kind::None, "one tick never presents twice");
}

void the_window_ends_with_the_real_frame() {
    PresentSchedule s = schedule();
    (void)s.on_guest_frame(at_ms(0), false);
    (void)s.on_guest_frame(at_ms(50), true);
    const auto due = s.due(at_ms(100));
    check(due.kind == Kind::Held, "at the end of the window the real frame is shown");
    check(!s.active(), "the window closes when the guest frame is late");
    check(s.held() == 1u, "the hold is counted");
    check(s.due(at_ms(110)).kind == Kind::None, "a closed window presents nothing more");
}

void a_skipped_tick_never_bursts() {
    PresentSchedule s;
    s.set_enabled(true);
    s.set_refresh_interval(std::chrono::milliseconds(10));
    (void)s.on_guest_frame(at_ms(0), false);
    // A 100 ms window with the presenter 45 ms behind: the tick that was due at
    // 10 ms is skipped forward, and only one present happens.
    (void)s.on_guest_frame(at_ms(100), true);
    // First new window needs a previous interval; use a second frame to set it.
    (void)s.on_guest_frame(at_ms(200), true);
    const auto due = s.due(at_ms(225));
    check(due.kind == Kind::Interpolated, "the late tick still presents");
    check(due.late_tick, "a missed refresh is reported, not hidden");
    check(s.skipped() >= 1u, "the skip is counted");
    check(s.due(at_ms(226)).kind == Kind::None, "the presenter does not burst to catch up");
}

void a_guest_frame_cuts_the_window_short() {
    PresentSchedule s = schedule();
    (void)s.on_guest_frame(at_ms(0), false);
    (void)s.on_guest_frame(at_ms(50), true);
    check(!s.on_guest_frame(at_ms(70), true), "an early frame starts the next window");
    check_near(s.due(at_ms(80)).t, 0.5f,
        "the new window is as long as the interval it came from");
}

} // namespace

int main() {
    try {
        off_presents_every_frame_at_once();
        the_first_frame_has_nothing_to_interpolate();
        a_window_presents_one_in_between_per_refresh();
        no_tick_before_its_time_or_twice();
        the_window_ends_with_the_real_frame();
        a_skipped_tick_never_bursts();
        a_guest_frame_cuts_the_window_short();
        std::cout << "present_schedule_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

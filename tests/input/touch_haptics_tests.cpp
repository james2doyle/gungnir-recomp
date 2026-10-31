// T11c touch-haptics policy: one tick for a new button or d-pad down, none
// for the stick, edit, move or release, and no input changes when the sink is
// disabled, absent or throws.

#include "input/touch_haptics.hpp"
#include "input/touch_overlay.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using gungnir::input::TouchControl;
using gungnir::input::TouchHaptics;
using gungnir::input::TouchOverlay;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void policy_table() {
    TouchHaptics haptics([] {});
    check(haptics.is_haptic_control(TouchControl::Cross), "Cross ticks");
    check(haptics.is_haptic_control(TouchControl::Circle), "Circle ticks");
    check(haptics.is_haptic_control(TouchControl::Square), "Square ticks");
    check(haptics.is_haptic_control(TouchControl::Triangle), "Triangle ticks");
    check(haptics.is_haptic_control(TouchControl::L), "L ticks");
    check(haptics.is_haptic_control(TouchControl::R), "R ticks");
    check(haptics.is_haptic_control(TouchControl::Start), "Start ticks");
    check(haptics.is_haptic_control(TouchControl::Select), "Select ticks");
    check(haptics.is_haptic_control(TouchControl::Dpad), "D-pad ticks");
    check(!haptics.is_haptic_control(TouchControl::Stick), "the stick is silent");
    check(!haptics.is_haptic_control(TouchControl::Count), "no control is silent");
}

void one_tick_per_down() {
    int ticks = 0;
    TouchHaptics haptics([&ticks] { ++ticks; });
    check(haptics.press(TouchControl::Cross), "the first Cross down requests a tick");
    check(ticks == 1, "one sink call");
    check(haptics.press(TouchControl::Cross), "a second Cross down requests another tick");
    check(ticks == 2, "rapid presses are not swallowed");
    for (int i = 0; i < 5; ++i) check(haptics.press(TouchControl::Dpad), "d-pad down requests a tick");
    check(ticks == 7, "five rapid d-pad downs produce five requests");

    check(!haptics.press(TouchControl::Stick), "stick down is silent");
    check(!haptics.press(TouchControl::Count), "an empty hit is silent");
    check(ticks == 7, "silent controls do not call the sink");
}

void disabled_and_absent() {
    int ticks = 0;
    TouchHaptics haptics([&ticks] { ++ticks; });
    haptics.set_enabled(false);
    check(!haptics.press(TouchControl::Cross), "disabled haptics are a no-op");
    check(ticks == 0, "disabled haptics do not call the sink");
    haptics.set_enabled(true);
    check(haptics.press(TouchControl::Cross), "enabling restores the tick");
    check(ticks == 1, "enabled haptics call the sink");

    TouchHaptics absent;
    check(!absent.press(TouchControl::Cross), "an absent sink is a no-op");
}

void sink_failure_is_contained() {
    int attempts = 0;
    TouchHaptics haptics([&attempts] {
        ++attempts;
        throw std::runtime_error("vibrator failed");
    });
    check(!haptics.press(TouchControl::Cross), "a throwing sink reports no tick");
    check(attempts == 1, "the sink was attempted once");
    check(!haptics.press(TouchControl::Cross), "a later press still does not throw");
    check(attempts == 2, "a failing sink does not get stuck");

    int ticks = 0;
    haptics.set_sink([&ticks] { ++ticks; });
    check(haptics.press(TouchControl::Cross), "a recovered sink ticks again");
    check(ticks == 1, "the recovered sink is called");
}

void overlay_hit_result() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, gungnir::input::TouchInsets{});
    check(overlay.touch_down(1, 0.82f * 1280.0f, 0.74f * 720.0f) == TouchControl::Cross,
          "a Cross touch-down reports Cross");
    overlay.touch_up(1);
    check(overlay.touch_down(2, 200.0f, 400.0f) == TouchControl::Stick,
          "a left-half touch-down reports the floating stick");
    overlay.touch_up(2);
    check(overlay.touch_down(3, 0.50f * 1280.0f, 0.30f * 720.0f) == TouchControl::Count,
          "an empty touch-down reports no control");
    overlay.touch_up(3);

    overlay.set_editing(true);
    check(overlay.touch_down(4, 0.82f * 1280.0f, 0.74f * 720.0f) == TouchControl::Count,
          "edit-mode touches never request a game haptic");
    overlay.touch_up(4);
}

} // namespace

int main() {
    try {
        policy_table();
        one_tick_per_down();
        disabled_and_absent();
        sink_failure_is_contained();
        overlay_hit_result();
    } catch (const std::exception &error) {
        std::cerr << "touch_haptics_tests: FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "touch_haptics_tests: OK\n";
    return 0;
}

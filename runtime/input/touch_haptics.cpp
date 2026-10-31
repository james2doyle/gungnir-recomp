#include "input/touch_haptics.hpp"

#include <utility>

namespace gungnir::input {

TouchHaptics::TouchHaptics(Sink sink) : sink_(std::move(sink)) {}

void TouchHaptics::set_sink(Sink sink) {
    sink_ = std::move(sink);
}

bool TouchHaptics::is_haptic_control(TouchControl control) noexcept {
    switch (control) {
    case TouchControl::Triangle:
    case TouchControl::Circle:
    case TouchControl::Cross:
    case TouchControl::Square:
    case TouchControl::L:
    case TouchControl::R:
    case TouchControl::Start:
    case TouchControl::Select:
    case TouchControl::Dpad:
        return true;
    case TouchControl::Stick:
    case TouchControl::Count:
        break;
    }
    return false;
}

bool TouchHaptics::press(TouchControl control) {
    if (!enabled_ || !sink_ || !is_haptic_control(control)) return false;
    try {
        sink_();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace gungnir::input

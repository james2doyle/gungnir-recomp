#include "input/gamepad_state.hpp"

#include <algorithm>
#include <cmath>

namespace gungnir::input {

void GamepadState::set_profile(const GamepadProfile &profile) {
    profile_ = profile;
    held_.fill(false);
    axes_.fill(0.0f);
}

void GamepadState::button_down(int index) {
    if (index < 0 || index >= kGamepadButtonCount) return;
    held_[static_cast<std::size_t>(index)] = true;
}

void GamepadState::button_up(int index) {
    if (index < 0 || index >= kGamepadButtonCount) return;
    held_[static_cast<std::size_t>(index)] = false;
}

void GamepadState::axis(int index, float value) {
    if (index < 0 || index >= kGamepadAxisCount) return;
    axes_[static_cast<std::size_t>(index)] = std::clamp(value, -1.0f, 1.0f);
}

void GamepadState::release_all() {
    held_.fill(false);
    axes_.fill(0.0f);
}

ButtonMask GamepadState::buttons() const noexcept {
    ButtonMask mask = 0u;
    for (int i = 0; i < kGamepadButtonCount; ++i) {
        if (held_[static_cast<std::size_t>(i)])
            mask |= profile_.buttons[static_cast<std::size_t>(i)];
    }
    return mask;
}

Analog GamepadState::analog() const noexcept {
    const float x = axes_[static_cast<std::size_t>(profile_.left_x_axis)];
    float y = axes_[static_cast<std::size_t>(profile_.left_y_axis)];
    if (profile_.invert_y) y = -y;
    return Analog{x, y};
}

bool GamepadState::active() const noexcept {
    if (buttons() != 0u) return true;
    const Analog value = analog();
    return std::fabs(value.x) > profile_.deadzone || std::fabs(value.y) > profile_.deadzone;
}

} // namespace gungnir::input

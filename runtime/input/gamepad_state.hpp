#pragma once

// Gamepad button/axis state mapped through a GamepadProfile (T11d).

#include "input/gamepad_profile.hpp"

#include <array>
#include <cstdint>

namespace gungnir::input {

class GamepadState {
public:
    void set_profile(const GamepadProfile &profile);
    [[nodiscard]] const GamepadProfile &profile() const noexcept { return profile_; }

    void button_down(int index);
    void button_up(int index);
    void axis(int index, float value);
    void release_all();

    [[nodiscard]] ButtonMask buttons() const noexcept;
    [[nodiscard]] Analog analog() const noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    GamepadProfile profile_{};
    std::array<bool, kGamepadButtonCount> held_{};
    std::array<float, kGamepadAxisCount> axes_{};
};

} // namespace gungnir::input

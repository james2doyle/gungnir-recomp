#pragma once

// Remap wizard state machine for a gamepad profile (T11d).

#include "input/gamepad_profile.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace gungnir::input {

class GamepadRemap {
public:
    void begin(const GamepadProfile &profile);
    void cancel();

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] int progress() const noexcept { return index_; }
    [[nodiscard]] int target_count() const noexcept;
    [[nodiscard]] std::string status() const;

    // Captures the next physical input. Returns true when the target moved.
    bool button(int index);
    bool axis(int index, float value);

    [[nodiscard]] const GamepadProfile &profile() const noexcept { return profile_; }

private:
    struct Target {
        const char *prompt;
        ButtonMask mask;
        bool axis;
        int axis_index;
    };

    [[nodiscard]] static const std::array<Target, 14> &targets() noexcept;
    void advance() noexcept;

    GamepadProfile profile_{};
    int index_{};
    bool active_{};
    bool done_{};
};

} // namespace gungnir::input

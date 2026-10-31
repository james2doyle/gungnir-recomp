#include "input/gamepad_remap.hpp"

#include <cmath>

namespace gungnir::input {
namespace {

constexpr int kTargetCount = 14;
constexpr float kAxisCapture = 0.5f;

} // namespace

const std::array<GamepadRemap::Target, 14> &GamepadRemap::targets() noexcept {
    static const std::array<Target, 14> kTargets{{
        {"Cross", kCrossBit, false, 0},
        {"Circle", kCircleBit, false, 0},
        {"Square", kSquareBit, false, 0},
        {"Triangle", kTriangleBit, false, 0},
        {"L", kLBit, false, 0},
        {"R", kRBit, false, 0},
        {"Start", kStartBit, false, 0},
        {"Select", kSelectBit, false, 0},
        {"D-pad Up", kUpBit, false, 0},
        {"D-pad Down", kDownBit, false, 0},
        {"D-pad Left", kLeftBit, false, 0},
        {"D-pad Right", kRightBit, false, 0},
        {"Push the left stick left/right", 0u, true, 0},
        {"Push the left stick up/down", 0u, true, 1},
    }};
    return kTargets;
}

int GamepadRemap::target_count() const noexcept {
    return kTargetCount;
}

void GamepadRemap::begin(const GamepadProfile &profile) {
    profile_ = profile;
    profile_.id = "user";
    profile_.match.clear();
    index_ = 0;
    active_ = true;
    done_ = false;
}

void GamepadRemap::cancel() {
    active_ = false;
    done_ = false;
}

void GamepadRemap::advance() noexcept {
    ++index_;
    if (index_ >= kTargetCount) {
        index_ = kTargetCount;
        active_ = false;
        done_ = true;
    }
}

bool GamepadRemap::button(int index) {
    if (!active_ || done_ || index < 0 || index >= kGamepadButtonCount) return false;
    const Target &target = targets()[static_cast<std::size_t>(index_)];
    if (target.axis) return false;
    for (ButtonMask &mask : profile_.buttons) mask &= ~target.mask;
    profile_.buttons[static_cast<std::size_t>(index)] = target.mask;
    advance();
    return true;
}

bool GamepadRemap::axis(int index, float value) {
    if (!active_ || done_ || index < 0 || index >= kGamepadAxisCount) return false;
    const Target &target = targets()[static_cast<std::size_t>(index_)];
    if (!target.axis || std::fabs(value) < kAxisCapture) return false;
    if (target.axis_index == 0) {
        profile_.left_x_axis = index;
    } else {
        if (index == profile_.left_x_axis) return false;
        profile_.left_y_axis = index;
    }
    advance();
    return true;
}

std::string GamepadRemap::status() const {
    if (done_) return "done";
    if (!active_) return "cancelled";
    const Target &target = targets()[static_cast<std::size_t>(index_)];
    const std::string suffix = " (" + std::to_string(index_ + 1) + "/" +
        std::to_string(kTargetCount) + ")";
    if (target.axis) return std::string(target.prompt) + suffix;
    return std::string("Press ") + target.prompt + suffix;
}

} // namespace gungnir::input

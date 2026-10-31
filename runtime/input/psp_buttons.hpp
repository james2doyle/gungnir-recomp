#pragma once

// PSP SceCtrlButtons bit values used by the input layers.

#include "input/input_state.hpp"

namespace gungnir::input {

inline constexpr ButtonMask kSelectBit = button_mask(Button::Select);
inline constexpr ButtonMask kStartBit = button_mask(Button::Start);
inline constexpr ButtonMask kUpBit = button_mask(Button::Up);
inline constexpr ButtonMask kRightBit = button_mask(Button::Right);
inline constexpr ButtonMask kDownBit = button_mask(Button::Down);
inline constexpr ButtonMask kLeftBit = button_mask(Button::Left);
inline constexpr ButtonMask kLBit = button_mask(Button::L);
inline constexpr ButtonMask kRBit = button_mask(Button::R);
inline constexpr ButtonMask kTriangleBit = button_mask(Button::Triangle);
inline constexpr ButtonMask kCircleBit = button_mask(Button::Circle);
inline constexpr ButtonMask kCrossBit = button_mask(Button::Cross);
inline constexpr ButtonMask kSquareBit = button_mask(Button::Square);

} // namespace gungnir::input

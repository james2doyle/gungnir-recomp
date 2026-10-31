#pragma once

// Desktop keyboard and mouse fallback (T11a).
//
// Key identities are SDL scancode values (USB HID positions), so W A S D stay
// where they are on any layout. The map carries no SDL dependency; the SDL
// adapter static-asserts the few positions it publishes.

#include "input/input_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gungnir::input {

namespace scancode {
inline constexpr int A = 4;
inline constexpr int B = 5;
inline constexpr int C = 6;
inline constexpr int D = 7;
inline constexpr int E = 8;
inline constexpr int F = 9;
inline constexpr int G = 10;
inline constexpr int H = 11;
inline constexpr int I = 12;
inline constexpr int J = 13;
inline constexpr int K = 14;
inline constexpr int L = 15;
inline constexpr int M = 16;
inline constexpr int N = 17;
inline constexpr int O = 18;
inline constexpr int P = 19;
inline constexpr int Q = 20;
inline constexpr int R = 21;
inline constexpr int S = 22;
inline constexpr int T = 23;
inline constexpr int U = 24;
inline constexpr int V = 25;
inline constexpr int W = 26;
inline constexpr int X = 27;
inline constexpr int Y = 28;
inline constexpr int Z = 29;
inline constexpr int Return = 40;
inline constexpr int Escape = 41;
inline constexpr int Backspace = 42;
inline constexpr int Tab = 43;
inline constexpr int Space = 44;
inline constexpr int Right = 79;
inline constexpr int Left = 80;
inline constexpr int Down = 81;
inline constexpr int Up = 82;
} // namespace scancode

inline constexpr std::size_t kKeyPositions = 512u;
inline constexpr std::size_t kMouseButtons = 8u;

class KeyboardMap {
public:
    // W A S D and the arrow keys for the D-pad, J K L I for the face buttons,
    // Q/E for L/R, Return/Backspace for Start/Select; left/right/middle mouse
    // for Cross/Circle/Square.
    [[nodiscard]] static KeyboardMap defaults();

    void clear();
    void bind(int key, ButtonMask buttons);
    void bind_mouse(int button, ButtonMask buttons);

    [[nodiscard]] ButtonMask key_mask(int key) const noexcept;
    [[nodiscard]] ButtonMask mouse_mask(int button) const noexcept;

private:
    std::array<ButtonMask, kKeyPositions> keys_{};
    std::array<ButtonMask, kMouseButtons> mouse_{};
};

} // namespace gungnir::input

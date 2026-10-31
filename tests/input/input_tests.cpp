// T11a input core contracts: source-neutral buttons and analog state, the
// deterministic source merge, PSP pad mapping and the desktop key map.
//
// Usage: input_tests
// Exit 0 on success, 1 on a failed contract.

#include "input/input_state.hpp"
#include "input/keyboard_map.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using gungnir::input::Analog;
using gungnir::input::Button;
using gungnir::input::ButtonMask;
using gungnir::input::Edges;
using gungnir::input::InputState;
using gungnir::input::KeyboardMap;
using gungnir::input::PadState;
using gungnir::input::Source;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

ButtonMask mask(Button button) {
    return gungnir::input::button_mask(button);
}

void button_bits() {
    check(mask(Button::Select) == 0x000001u, "Select uses the PSP bit");
    check(mask(Button::Start) == 0x000008u, "Start uses the PSP bit");
    check(mask(Button::Up) == 0x000010u, "Up uses the PSP bit");
    check(mask(Button::Right) == 0x000020u, "Right uses the PSP bit");
    check(mask(Button::Down) == 0x000040u, "Down uses the PSP bit");
    check(mask(Button::Left) == 0x000080u, "Left uses the PSP bit");
    check(mask(Button::L) == 0x000100u, "L uses the PSP bit");
    check(mask(Button::R) == 0x000200u, "R uses the PSP bit");
    check(mask(Button::Triangle) == 0x001000u, "Triangle uses the PSP bit");
    check(mask(Button::Circle) == 0x002000u, "Circle uses the PSP bit");
    check(mask(Button::Cross) == 0x004000u, "Cross uses the PSP bit");
    check(mask(Button::Square) == 0x008000u, "Square uses the PSP bit");
}

void axis_mapping() {
    check(gungnir::input::axis_to_byte(0.0f) == 128u, "centred axis is 0x80");
    check(gungnir::input::axis_to_byte(1.0f) == 255u, "full right is 0xFF");
    check(gungnir::input::axis_to_byte(-1.0f) == 0u, "full left is 0x00");
    check(gungnir::input::axis_to_byte(2.0f) == 255u, "over-range is clamped high");
    check(gungnir::input::axis_to_byte(-2.0f) == 0u, "over-range is clamped low");
    check(gungnir::input::axis_to_byte(0.11f, 0.12f) == 128u, "inside the dead zone is centre");
    check(gungnir::input::axis_to_byte(0.12f, 0.12f) == 128u, "the dead-zone edge is centre");
    check(gungnir::input::axis_to_byte(0.5f, 0.0f) == 192u, "half right maps through 127.5");
    check(gungnir::input::axis_to_byte(-0.5f, 0.0f) == 64u, "half left maps through 127.5");

    const PadState centred = gungnir::input::to_pad(0u, Analog{});
    check(centred.lx == 128u && centred.ly == 128u, "primary stick starts centred");
    check(centred.rx == 128u && centred.ry == 128u, "secondary stick starts centred");
    check(centred.buttons == 0u, "no buttons by default");

    const PadState pushed = gungnir::input::to_pad(mask(Button::Cross), Analog{1.0f, -1.0f});
    check(pushed.buttons == mask(Button::Cross), "buttons pass through");
    check(pushed.lx == 255u && pushed.ly == 0u, "analog signs reach the PSP range");
}

void press_release_edges() {
    InputState state;
    Edges edges = state.take_edges();
    check(edges.pressed == 0u && edges.released == 0u, "a fresh state has no edges");

    state.set_buttons(Source::Keyboard, 0, 0, mask(Button::Cross));
    check(state.buttons() == mask(Button::Cross), "a press is held");
    edges = state.take_edges();
    check(edges.pressed == mask(Button::Cross) && edges.released == 0u,
          "the press produces one make edge");

    state.set_buttons(Source::Keyboard, 0, 0, mask(Button::Cross));
    edges = state.take_edges();
    check(edges.pressed == 0u && edges.released == 0u, "holding does not repeat the edge");

    state.set_buttons(Source::Keyboard, 0, 0, mask(Button::Cross) | mask(Button::Circle));
    edges = state.take_edges();
    check(edges.pressed == mask(Button::Circle) && edges.released == 0u,
          "adding a button makes only the new one");

    state.set_buttons(Source::Keyboard, 0, 0, 0u);
    edges = state.take_edges();
    check(edges.pressed == 0u && edges.released == (mask(Button::Cross) | mask(Button::Circle)),
          "releasing produces break edges");
}

void source_merge() {
    InputState state;
    state.set_buttons(Source::Touch, 0, 1, mask(Button::Cross));
    state.set_buttons(Source::Gamepad, 0, 0, mask(Button::Cross));
    check(state.buttons() == mask(Button::Cross), "two sources holding one button stay held");

    (void)state.take_edges();
    state.release(Source::Gamepad, 0, 0);
    check(state.buttons() == mask(Button::Cross), "one source releasing does not release the button");
    Edges edges = state.take_edges();
    check(edges.released == 0u, "no break edge while another source holds");

    state.release(Source::Touch, 0, 1);
    check(state.buttons() == 0u, "the last source releases the button");
    edges = state.take_edges();
    check(edges.released == mask(Button::Cross), "the last release produces the break edge");

    state.set_buttons(Source::Touch, 0, 1, mask(Button::Cross));
    state.set_buttons(Source::Gamepad, 0, 0, mask(Button::Circle));
    check(state.buttons() == (mask(Button::Cross) | mask(Button::Circle)),
          "distinct sources OR their buttons");
}

void analog_merge() {
    InputState state;
    state.set_analog(Source::Touch, 0, 1, Analog{0.25f, 0.0f});
    state.set_analog(Source::Gamepad, 0, 0, Analog{-0.75f, 0.5f});
    Analog value = state.analog();
    check(value.x == 0.25f && value.y == 0.0f, "touch has priority over gamepad");

    state.release(Source::Touch, 0, 1);
    value = state.analog();
    check(value.x == -0.75f && value.y == 0.5f, "releasing the owner exposes the next source");

    state.set_analog(Source::Touch, 0, 2, Analog{0.0f, 0.5f});
    state.set_analog(Source::Touch, 0, 3, Analog{0.0f, -0.25f});
    value = state.analog();
    check(value.y == -0.25f, "the latest touch pointer wins inside one source");

    state.set_analog(Source::Touch, 0, 3, Analog{0.0f, 0.0f});
    value = state.analog();
    check(value.y == 0.5f, "a recentre releases the axis back to the other pointer");
}

void release_all() {
    InputState state;
    state.set_buttons(Source::Touch, 0, 1, mask(Button::Cross));
    state.set_buttons(Source::Gamepad, 0, 0, mask(Button::Start));
    state.set_analog(Source::Touch, 0, 1, Analog{0.5f, -0.5f});
    (void)state.take_edges();

    state.release_all();
    check(state.buttons() == 0u, "release-all clears every button");
    check(state.analog().x == 0.0f && state.analog().y == 0.0f, "release-all centres the analog");
    const Edges edges = state.take_edges();
    check(edges.released == (mask(Button::Cross) | mask(Button::Start)),
          "release-all reports every held button as released");
}

void unknown_release_is_noop() {
    InputState state;
    state.set_buttons(Source::Keyboard, 0, 0, mask(Button::Cross));
    (void)state.take_edges();
    state.release(Source::Keyboard, 0, 7);
    state.release(Source::Mouse, 3, 0);
    check(state.buttons() == mask(Button::Cross), "releasing an unknown slot changes nothing");
    const Edges edges = state.take_edges();
    check(edges.pressed == 0u && edges.released == 0u, "an unknown release makes no edge");
}

void pad_from_state() {
    InputState state;
    state.set_buttons(Source::Touch, 0, 1, mask(Button::Cross) | mask(Button::Up));
    state.set_analog(Source::Touch, 0, 1, Analog{0.5f, -0.5f});
    const PadState pad = state.pad();
    check(pad.buttons == (mask(Button::Cross) | mask(Button::Up)), "pad exposes merged buttons");
    check(pad.lx == 183u && pad.ly == 73u, "pad exposes merged analog through the dead zone");
    check(pad.rx == 128u && pad.ry == 128u, "unused secondary stick stays centred");
}

void keyboard_map() {
    const KeyboardMap map = KeyboardMap::defaults();
    check(map.key_mask(gungnir::input::scancode::W) == mask(Button::Up), "W is Up");
    check(map.key_mask(gungnir::input::scancode::Up) == mask(Button::Up), "Arrow Up is Up");
    check(map.key_mask(gungnir::input::scancode::S) == mask(Button::Down), "S is Down");
    check(map.key_mask(gungnir::input::scancode::A) == mask(Button::Left), "A is Left");
    check(map.key_mask(gungnir::input::scancode::D) == mask(Button::Right), "D is Right");
    check(map.key_mask(gungnir::input::scancode::J) == mask(Button::Cross), "J is Cross");
    check(map.key_mask(gungnir::input::scancode::K) == mask(Button::Circle), "K is Circle");
    check(map.key_mask(gungnir::input::scancode::L) == mask(Button::Square), "L is Square");
    check(map.key_mask(gungnir::input::scancode::I) == mask(Button::Triangle), "I is Triangle");
    check(map.key_mask(gungnir::input::scancode::Q) == mask(Button::L), "Q is L");
    check(map.key_mask(gungnir::input::scancode::E) == mask(Button::R), "E is R");
    check(map.key_mask(gungnir::input::scancode::Return) == mask(Button::Start), "Return is Start");
    check(map.key_mask(gungnir::input::scancode::Backspace) == mask(Button::Select), "Backspace is Select");
    check(map.key_mask(999) == 0u, "an unmapped key is silent");
    check(map.mouse_mask(1) == mask(Button::Cross), "mouse left is Cross");
    check(map.mouse_mask(2) == mask(Button::Circle), "mouse right is Circle");
    check(map.mouse_mask(3) == mask(Button::Square), "mouse middle is Square");
    check(map.mouse_mask(8) == 0u, "an unmapped mouse button is silent");

    KeyboardMap rebound = KeyboardMap::defaults();
    rebound.clear();
    rebound.bind(gungnir::input::scancode::F, mask(Button::Start));
    rebound.bind_mouse(4, mask(Button::L));
    check(rebound.key_mask(gungnir::input::scancode::W) == 0u, "clear removes the default map");
    check(rebound.key_mask(gungnir::input::scancode::F) == mask(Button::Start), "a rebound key is live");
    check(rebound.mouse_mask(4) == mask(Button::L), "a rebound mouse button is live");
}

void reset_between_tests() {
    gungnir::input::reset_state();
    check(gungnir::input::state().buttons() == 0u, "reset_state clears the global state");
}

} // namespace

int main() {
    try {
        button_bits();
        axis_mapping();
        press_release_edges();
        source_merge();
        analog_merge();
        release_all();
        unknown_release_is_noop();
        pad_from_state();
        keyboard_map();
        reset_between_tests();
    } catch (const std::exception &error) {
        std::cerr << "input_tests: FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "input_tests: OK\n";
    return 0;
}

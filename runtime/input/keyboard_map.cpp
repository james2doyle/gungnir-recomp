#include "input/keyboard_map.hpp"

namespace gungnir::input {

KeyboardMap KeyboardMap::defaults() {
    KeyboardMap map;
    map.bind(scancode::W, button_mask(Button::Up));
    map.bind(scancode::Up, button_mask(Button::Up));
    map.bind(scancode::S, button_mask(Button::Down));
    map.bind(scancode::Down, button_mask(Button::Down));
    map.bind(scancode::A, button_mask(Button::Left));
    map.bind(scancode::Left, button_mask(Button::Left));
    map.bind(scancode::D, button_mask(Button::Right));
    map.bind(scancode::Right, button_mask(Button::Right));

    map.bind(scancode::J, button_mask(Button::Cross));
    map.bind(scancode::K, button_mask(Button::Circle));
    map.bind(scancode::L, button_mask(Button::Square));
    map.bind(scancode::I, button_mask(Button::Triangle));

    map.bind(scancode::Q, button_mask(Button::L));
    map.bind(scancode::E, button_mask(Button::R));
    map.bind(scancode::Return, button_mask(Button::Start));
    map.bind(scancode::Backspace, button_mask(Button::Select));

    map.bind_mouse(1, button_mask(Button::Cross));
    map.bind_mouse(2, button_mask(Button::Circle));
    map.bind_mouse(3, button_mask(Button::Square));
    return map;
}

void KeyboardMap::clear() {
    keys_.fill(0u);
    mouse_.fill(0u);
}

void KeyboardMap::bind(int key, ButtonMask buttons) {
    if (key < 0 || static_cast<std::size_t>(key) >= keys_.size()) return;
    keys_[static_cast<std::size_t>(key)] = buttons;
}

void KeyboardMap::bind_mouse(int button, ButtonMask buttons) {
    if (button < 0 || static_cast<std::size_t>(button) >= mouse_.size()) return;
    mouse_[static_cast<std::size_t>(button)] = buttons;
}

ButtonMask KeyboardMap::key_mask(int key) const noexcept {
    if (key < 0 || static_cast<std::size_t>(key) >= keys_.size()) return 0u;
    return keys_[static_cast<std::size_t>(key)];
}

ButtonMask KeyboardMap::mouse_mask(int button) const noexcept {
    if (button < 0 || static_cast<std::size_t>(button) >= mouse_.size()) return 0u;
    return mouse_[static_cast<std::size_t>(button)];
}

} // namespace gungnir::input

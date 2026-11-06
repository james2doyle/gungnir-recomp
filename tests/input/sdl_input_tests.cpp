// SdlInput touch-mouse contract: synthetic mouse events from touches
// (SDL_TOUCH_MOUSEID) must never map to guest buttons. The touch overlay
// owns touch input via JNI; without this filter a tap that falls through to
// the SDL surface maps to Cross through the desktop mouse bindings.

#include "input/input_state.hpp"
#include "input/menu_navigation.hpp"
#include "input/sdl_input.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using gungnir::input::Button;
using gungnir::input::SdlInput;
using gungnir::input::Source;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

SDL_Event mouse_event(Uint32 type, SDL_MouseID which, Uint8 button) {
    SDL_Event event{};
    event.type = type;
    event.button.which = which;
    event.button.button = button;
    return event;
}

void synthetic_touch_mouse_is_ignored() {
    gungnir::input::reset_state();
    SdlInput input;
    SDL_Event down = mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_TOUCH_MOUSEID, 1);
    check(input.handle(down), "synthetic touch-mouse down is still consumed");
    check((gungnir::input::state().buttons() &
           gungnir::input::button_mask(Button::Cross)) == 0u,
          "a touch tap never becomes Cross");
    SDL_Event up = mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, SDL_TOUCH_MOUSEID, 1);
    check(input.handle(up), "synthetic touch-mouse up is still consumed");
    check(gungnir::input::state().buttons() == 0u, "no buttons leak from touch taps");
}

SDL_Event key_event(Uint32 type, SDL_Scancode scancode, bool repeat = false) {
    SDL_Event event{};
    event.type = type;
    event.key.scancode = scancode;
    event.key.repeat = repeat;
    return event;
}

void drain_menu() {
    while (gungnir::input::menu_navigation().take().has_value()) {
    }
}

void system_back_toggles_menu() {
    using namespace gungnir::input;
    drain_menu();
    reset_state();
    SdlInput input;
    SDL_Event down = key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_AC_BACK);
    check(input.handle(down), "system back is consumed");
    check((state().buttons() & button_mask(Button::Select)) == 0u,
          "system back never reaches the guest");
    const auto toggled = menu_navigation().take();
    check(toggled.has_value() && toggled->action == MenuAction::Toggle,
          "system back enqueues a menu toggle");
    drain_menu();
    // Repeats and releases must not toggle again: one press, one action.
    check(input.handle(key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_AC_BACK, true)),
          "repeat back is consumed");
    check(input.handle(key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_AC_BACK)),
          "back release is consumed");
    check(!menu_navigation().take().has_value(), "repeat/release toggle nothing");
}

void real_mouse_still_maps() {
    gungnir::input::reset_state();
    SdlInput input;
    SDL_Event down = mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, 0, 1);
    check(input.handle(down), "real mouse down is consumed");
    check((gungnir::input::state().buttons() &
           gungnir::input::button_mask(Button::Cross)) != 0u,
          "real mouse left is still Cross");
    SDL_Event up = mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, 0, 1);
    input.handle(up);
    check(gungnir::input::state().buttons() == 0u, "real mouse release clears Cross");
}

} // namespace

int main() {
    try {
        synthetic_touch_mouse_is_ignored();
        system_back_toggles_menu();
        real_mouse_still_maps();
        std::cout << "sdl_input_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

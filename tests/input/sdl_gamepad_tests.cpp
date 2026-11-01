// T11d SDL adapter contracts with a virtual gamepad: synthetic added/button/
// axis/removed events, mapping through the profile, remap save and release.

#include "input/input_state.hpp"
#include "input/menu_navigation.hpp"
#include "input/touch_overlay.hpp"
#include "platform/sdl_gamepad.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using gungnir::platform::SdlGamepad;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

SDL_Event button_event(SDL_JoystickID id, int button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = id;
    event.gbutton.button = static_cast<Uint8>(button);
    event.gbutton.down = down;
    return event;
}

SDL_Event axis_event(SDL_JoystickID id, int axis, Sint16 value) {
    SDL_Event event{};
    event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.which = id;
    event.gaxis.axis = static_cast<Uint8>(axis);
    event.gaxis.value = value;
    return event;
}

SDL_Event device_event(SDL_EventType type, SDL_JoystickID id) {
    SDL_Event event{};
    event.type = type;
    event.gdevice.which = id;
    return event;
}

void gamepad_contracts() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "gungnir-sdl-gamepad-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);

    check(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD), "SDL gamepad subsystem initializes");

    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = 6;
    description.nbuttons = 15;
    description.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) |
        (1u << SDL_GAMEPAD_BUTTON_EAST) | (1u << SDL_GAMEPAD_BUTTON_WEST) |
        (1u << SDL_GAMEPAD_BUTTON_NORTH) | (1u << SDL_GAMEPAD_BUTTON_BACK) |
        (1u << SDL_GAMEPAD_BUTTON_GUIDE) | (1u << SDL_GAMEPAD_BUTTON_START) |
        (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
        (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
        (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
        (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    description.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    description.name = "Gungnir Test Pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
    check(id != 0, "virtual joystick attaches");
    SDL_Joystick *joystick = SDL_OpenJoystick(id);
    check(joystick != nullptr, "virtual joystick opens");

    char guid_text[33]{};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_text, sizeof(guid_text));
    check(guid_text[0] != '\0', "virtual joystick has a GUID");
    const std::string mapping = std::string(guid_text) +
        ",Gungnir Test Pad,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftshoulder:b9,"
        "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,";
    check(SDL_AddGamepadMapping(mapping.c_str()) >= 0, "virtual mapping accepted");

    SdlGamepad gamepad("", directory);
    check(gamepad.handle_event(device_event(SDL_EVENT_GAMEPAD_ADDED, id)), "added event handled");
    check(gamepad.connected(), "gamepad connects");
    check(gamepad.info().find("Gungnir Test Pad") != std::string::npos, "name is reported");

    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_SOUTH, true));
    check((gungnir::input::state().buttons() & gungnir::input::kCrossBit) != 0u,
          "South maps to Cross");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_SOUTH, false));
    check((gungnir::input::state().buttons() & gungnir::input::kCrossBit) == 0u,
          "release clears Cross");

    auto &menu = gungnir::input::menu_navigation();
    menu.end();
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_BACK, true));
    check((gungnir::input::state().buttons() & gungnir::input::kSelectBit) != 0u,
          "Back presses Select");
    check(!menu.take().has_value(), "Back no longer opens the menu");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_BACK, false));
    check((gungnir::input::state().buttons() & gungnir::input::kSelectBit) == 0u,
          "releasing Back clears Select");

    check(gamepad.guide_available(), "the pad reports a Guide button");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_GUIDE, true));
    const auto toggle = menu.take();
    check(toggle && toggle->action == gungnir::input::MenuAction::Toggle,
          "Guide requests the overlay while closed");
    check((gungnir::input::state().buttons() & gungnir::input::kSelectBit) == 0u,
          "Guide is not the guest's Select");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_GUIDE, false));

    menu.begin(3);
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true));
    const auto move = menu.take();
    check(move && move->action == gungnir::input::MenuAction::Move && move->index == 1,
          "D-pad down moves the open selection");
    check((gungnir::input::state().buttons() & gungnir::input::kDownBit) == 0u,
          "the d-pad is withheld from the guest while the overlay is open");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false));

    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_SOUTH, true));
    const auto activate = menu.take();
    check(activate && activate->action == gungnir::input::MenuAction::Activate &&
              activate->index == 1,
          "South activates the open selection");
    check((gungnir::input::state().buttons() & gungnir::input::kCrossBit) == 0u,
          "Cross is withheld from the guest while the overlay is open");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_SOUTH, false));

    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_EAST, true));
    check((gungnir::input::state().buttons() & gungnir::input::kCircleBit) == 0u,
          "other pad buttons do not play the game while the overlay is open");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_EAST, false));
    menu.end();

    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_SOUTH, true));
    check((gungnir::input::state().buttons() & gungnir::input::kCrossBit) != 0u,
          "gamepad control returns after the menu closes");
    gamepad.handle_event(button_event(id, SDL_GAMEPAD_BUTTON_SOUTH, false));

    gamepad.handle_event(axis_event(id, SDL_GAMEPAD_AXIS_LEFTX, 16384));
    check(gungnir::input::state().analog().x > 0.4f, "left x reaches the state");

    gamepad.begin_remap();
    for (int button = 0; button < 12; ++button)
        gamepad.handle_event(button_event(id, button, true));
    gamepad.handle_event(axis_event(id, 2, 30000));
    gamepad.handle_event(axis_event(id, 3, -30000));
    check(gamepad.remap_done(), "remap completes");
    std::string error;
    check(gamepad.save_remap(error), "remap profile saves");
    check(error.empty(), "save has no error");

    check(gamepad.reset_profile(error), "profile resets");
    check(error.empty(), "reset has no error");

    gamepad.handle_event(device_event(SDL_EVENT_GAMEPAD_REMOVED, id));
    check(!gamepad.connected(), "removed event disconnects");
    check(gungnir::input::state().buttons() == 0u, "disconnect releases buttons");

    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
    gamepad.shutdown();
    SDL_Quit();
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    try {
        gamepad_contracts();
    } catch (const std::exception &error) {
        std::cerr << "sdl_gamepad_tests: FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "sdl_gamepad_tests: OK\n";
    return 0;
}

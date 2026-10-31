// T11d gamepad profile, state and remap contracts.

#include "input/gamepad_profile.hpp"
#include "input/gamepad_remap.hpp"
#include "input/gamepad_state.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using gungnir::input::Analog;
using gungnir::input::GamepadProfile;
using gungnir::input::GamepadProfileStore;
using gungnir::input::GamepadRemap;
using gungnir::input::GamepadState;
using gungnir::input::ShippedGamepadProfiles;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

bool near(float a, float b, float epsilon = 0.001f) {
    return std::fabs(a - b) <= epsilon;
}

const char *kShipped = R"({
  "version": 1,
  "profiles": [
    {"id":"default","match":"*","name":"Default","buttons":[16384,8192,32768,4096,1,0,8,0,0,256,512,16,64,128,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"left_x_axis":0,"left_y_axis":1,"deadzone":0.12,"invert_y":false},
    {"id":"ayn_odin","match":"Ayn Odin","name":"Ayn Odin","buttons":[16384,8192,32768,4096,1,0,8,0,0,256,512,16,64,128,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"left_x_axis":0,"left_y_axis":1,"deadzone":0.15,"invert_y":true}
  ]
})";

void default_profile() {
    const GamepadProfile profile = gungnir::input::default_gamepad_profile();
    check(profile.buttons[0] == gungnir::input::kCrossBit, "South is Cross");
    check(profile.buttons[1] == gungnir::input::kCircleBit, "East is Circle");
    check(profile.buttons[2] == gungnir::input::kSquareBit, "West is Square");
    check(profile.buttons[3] == gungnir::input::kTriangleBit, "North is Triangle");
    check(profile.buttons[4] == gungnir::input::kSelectBit, "Back is Select");
    check(profile.buttons[6] == gungnir::input::kStartBit, "Start is Start");
    check(profile.buttons[9] == gungnir::input::kLBit, "Left shoulder is L");
    check(profile.buttons[10] == gungnir::input::kRBit, "Right shoulder is R");
    check(profile.buttons[11] == gungnir::input::kUpBit, "D-pad up is Up");
    check(profile.buttons[12] == gungnir::input::kDownBit, "D-pad down is Down");
    check(profile.buttons[13] == gungnir::input::kLeftBit, "D-pad left is Left");
    check(profile.buttons[14] == gungnir::input::kRightBit, "D-pad right is Right");
    check(profile.left_x_axis == 0 && profile.left_y_axis == 1, "left stick axes");
}

void profile_round_trip() {
    GamepadProfile profile = gungnir::input::default_gamepad_profile();
    profile.id = "player";
    profile.guid = "03000000100800000100000000000000";
    profile.name = "Test Pad";
    profile.buttons[0] = gungnir::input::kCircleBit;
    profile.buttons[1] = gungnir::input::kCrossBit;
    profile.left_x_axis = 2;
    profile.left_y_axis = 3;
    profile.invert_y = true;
    profile.deadzone = 0.2f;

    const std::string json = gungnir::input::to_json(profile);
    GamepadProfile loaded;
    check(gungnir::input::parse_gamepad_profile(json, loaded) ==
              gungnir::input::GamepadProfileStatus::Ok,
          "profile parses");
    check(loaded == profile, "profile round-trips");
}

void shipped_profiles() {
    ShippedGamepadProfiles shipped;
    check(gungnir::input::parse_shipped_profiles(kShipped, shipped) ==
              gungnir::input::GamepadProfileStatus::Ok,
          "shipped profiles parse");
    const GamepadProfile ayn = gungnir::input::select_shipped_profile(shipped, "Ayn Odin Controller");
    check(ayn.id == "ayn_odin" && near(ayn.deadzone, 0.15f), "Ayn match wins");
    const GamepadProfile unknown = gungnir::input::select_shipped_profile(shipped, "Some Pad");
    check(unknown.id == "default", "unknown falls back to default");

    ShippedGamepadProfiles corrupt;
    check(gungnir::input::parse_shipped_profiles("{", corrupt) ==
              gungnir::input::GamepadProfileStatus::Corrupt,
          "malformed shipped profiles are named");
}

void state_mapping() {
    GamepadState state;
    state.set_profile(gungnir::input::default_gamepad_profile());
    check(state.buttons() == 0u && state.analog() == Analog{}, "state starts neutral");
    state.button_down(0);
    check(state.buttons() == gungnir::input::kCrossBit, "button down maps through the profile");
    state.button_down(1);
    check(state.buttons() == (gungnir::input::kCrossBit | gungnir::input::kCircleBit),
          "two buttons merge");
    state.button_up(0);
    check(state.buttons() == gungnir::input::kCircleBit, "button up clears only that button");
    state.axis(0, 0.5f);
    state.axis(1, -0.5f);
    check(near(state.analog().x, 0.5f) && near(state.analog().y, -0.5f), "axes pass through");
    check(state.active(), "a deflected axis is active");
    state.release_all();
    check(state.buttons() == 0u && state.analog() == Analog{}, "release-all clears the pad");
}

void remap_round_trip() {
    GamepadRemap remap;
    remap.begin(gungnir::input::default_gamepad_profile());
    check(remap.active() && !remap.done(), "remap starts");
    for (int button = 0; button < 12; ++button) {
        check(remap.button(button), "each button is captured");
    }
    check(remap.status().find("Push the left stick left/right") != std::string::npos,
          "the stick prompt says push, not press");
    check(remap.axis(2, 0.9f), "left x axis is captured");
    check(!remap.axis(2, 0.9f), "the same axis cannot be reused for left y");
    check(remap.axis(3, -0.9f), "left y axis is captured");
    check(remap.done(), "remap completes");

    const GamepadProfile &profile = remap.profile();
    check(profile.buttons[0] == gungnir::input::kCrossBit, "first target maps South to Cross");
    check(profile.buttons[1] == gungnir::input::kCircleBit, "second target maps East to Circle");
    check(profile.left_x_axis == 2 && profile.left_y_axis == 3, "axis targets are stored");

    GamepadProfile loaded;
    check(gungnir::input::parse_gamepad_profile(gungnir::input::to_json(profile), loaded) ==
              gungnir::input::GamepadProfileStatus::Ok,
          "remapped profile parses");
    check(loaded == profile, "remapped profile round-trips");

    GamepadProfile same_axes = gungnir::input::default_gamepad_profile();
    same_axes.left_y_axis = same_axes.left_x_axis;
    GamepadProfile rejected;
    check(gungnir::input::parse_gamepad_profile(gungnir::input::to_json(same_axes), rejected) ==
              gungnir::input::GamepadProfileStatus::Corrupt,
          "a profile with one axis for both directions is corrupt");
}

void store_round_trip() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "gungnir-gamepad-profile-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);

    GamepadProfileStore store(kShipped, directory);
    std::string error;
    const auto fallback = store.load("unknown-guid", "Some Pad", error);
    check(fallback.id == "default", "unknown device gets the default profile");
    check(error.empty(), "unknown device is not an error");

    GamepadProfile changed = fallback;
    changed.buttons[0] = gungnir::input::kCircleBit;
    changed.name = "My Pad";
    check(store.save("03000000100800000100000000000000", changed, error), "profile saves");
    const auto loaded = store.load("03000000100800000100000000000000", "My Pad", error);
    check(loaded.buttons[0] == gungnir::input::kCircleBit, "saved profile reloads");

    {
        std::ofstream corrupt(directory / "broken.json");
        corrupt << "{not json";
    }
    const auto corrupt = store.load("broken", "Broken", error);
    check(corrupt.id == "default" && !error.empty(), "corrupt profile falls back with a name");
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    try {
        default_profile();
        profile_round_trip();
        shipped_profiles();
        state_mapping();
        remap_round_trip();
        store_round_trip();
    } catch (const std::exception &error) {
        std::cerr << "gamepad_tests: FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "gamepad_tests: OK\n";
    return 0;
}

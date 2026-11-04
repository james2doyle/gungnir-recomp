#include "platform/sdl_gamepad.hpp"

#if defined(GUNGNIR_HAVE_SDL3)

#include "input/host_controls.hpp"
#include "input/input_state.hpp"
#include "input/menu_navigation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gungnir::platform {
namespace {

static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
static_assert(SDL_GAMEPAD_BUTTON_EAST == 1);
static_assert(SDL_GAMEPAD_BUTTON_WEST == 2);
static_assert(SDL_GAMEPAD_BUTTON_NORTH == 3);
static_assert(SDL_GAMEPAD_BUTTON_BACK == 4);
static_assert(SDL_GAMEPAD_BUTTON_GUIDE == 5);
static_assert(SDL_GAMEPAD_BUTTON_START == 6);
static_assert(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER == 9);
static_assert(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER == 10);
static_assert(SDL_GAMEPAD_BUTTON_DPAD_UP == 11);
static_assert(SDL_GAMEPAD_BUTTON_DPAD_DOWN == 12);
static_assert(SDL_GAMEPAD_BUTTON_DPAD_LEFT == 13);
static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);
static_assert(SDL_GAMEPAD_AXIS_LEFTX == 0);
static_assert(SDL_GAMEPAD_AXIS_LEFTY == 1);

std::string guid_string(SDL_JoystickID id) {
    char text[33]{};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), text, sizeof(text));
    return text;
}

std::optional<gungnir::input::MenuButton> menu_button(Uint8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return gungnir::input::MenuButton::Guide;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return gungnir::input::MenuButton::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return gungnir::input::MenuButton::Down;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return gungnir::input::MenuButton::South;
    default:
        return std::nullopt;
    }
}

} // namespace

SdlGamepad::SdlGamepad(std::string shipped_profiles_json, std::filesystem::path user_directory)
    : store_(std::move(shipped_profiles_json), std::move(user_directory)) {}

SdlGamepad::~SdlGamepad() {
    shutdown();
}

bool SdlGamepad::handle_event(const SDL_Event &event) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        open(event.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ != nullptr && event.gdevice.which == id_) close();
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (pad_ == nullptr || event.gbutton.which != id_) return true;
        if (!remap_.active()) {
            if (const auto menu = menu_button(event.gbutton.button)) {
                if (event.gbutton.down) gungnir::input::menu_navigation().press(*menu);
                // Guide toggles the overlay; the d-pad and South navigate it.
                // None of them reaches the guest, and while the overlay is
                // open no pad button plays the game behind it.
                const bool toggle = *menu == gungnir::input::MenuButton::Guide;
                if (toggle || gungnir::input::menu_navigation().open()) {
                    state_.button_up(event.gbutton.button);
                    publish();
                    return true;
                }
            } else if (gungnir::input::menu_navigation().open()) {
                // The overlay is modal: Select, Start, face and shoulder
                // buttons must not act on the game behind it.
                state_.button_up(event.gbutton.button);
                publish();
                return true;
            }
        }
        if (remap_.active()) {
            if (event.gbutton.down) remap_.button(event.gbutton.button);
        } else if (!gungnir::input::menu_navigation().open() &&
                   event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK &&
                   state_.profile().buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK] == 0u) {
            // R3 is not a PSP button: while the profile leaves it unbound, a
            // press toggles fast-forward (R2 is the momentary hold), and a pad
            // that maps R3 keeps its binding.
            if (event.gbutton.down)
                gungnir::input::host_controls().toggle(gungnir::input::HostControl::FastForward);
        } else if (event.gbutton.down) {
            state_.button_down(event.gbutton.button);
        } else {
            state_.button_up(event.gbutton.button);
        }
        publish();
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (pad_ == nullptr || event.gaxis.which != id_) return true;
        if (remap_.active()) {
            remap_.axis(event.gaxis.axis, static_cast<float>(event.gaxis.value) / 32767.0f);
        } else {
            // The right trigger is the R2 fast-forward hold, so it never
            // reaches the guest unless a profile uses it as a stick axis.
            state_.axis(event.gaxis.axis, static_cast<float>(event.gaxis.value) / 32767.0f);
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
                !gungnir::input::menu_navigation().open()) {
                gungnir::input::host_controls().hold(
                    gungnir::input::HostControl::FastForward, event.gaxis.value > 16384);
            }
        }
        publish();
        return true;
    default:
        return false;
    }
}

void SdlGamepad::open(SDL_JoystickID id) {
    if (pad_ != nullptr) close();
    pad_ = SDL_OpenGamepad(id);
    if (pad_ == nullptr) {
        last_error_ = "gamepad_open_failed";
        SDL_Log("gamepad_open_failed: %s", SDL_GetError());
        return;
    }
    id_ = id;
    const char *name = SDL_GetGamepadName(pad_);
    name_ = name != nullptr ? name : "Gamepad";
    guid_ = guid_string(id);
    std::string error;
    gungnir::input::GamepadProfile profile = store_.load(guid_, name_, error);
    last_error_ = error;
    state_.set_profile(profile);
    profile_id_ = profile.id;
    connected_ = true;
    publish();
    SDL_Log("gamepad_connected name=%s guid=%s profile=%s", name_.c_str(), guid_.c_str(),
            profile_id_.c_str());
    if (!last_error_.empty())
        SDL_Log("gamepad_profile_fallback: %s", last_error_.c_str());
}

void SdlGamepad::close() {
    if (pad_ == nullptr) return;
    state_.release_all();
    gungnir::input::host_controls().release_holds();
    publish();
    SDL_Log("gamepad_removed name=%s", name_.c_str());
    SDL_CloseGamepad(pad_);
    pad_ = nullptr;
    id_ = 0;
    connected_ = false;
    remap_.cancel();
    name_.clear();
    guid_.clear();
    profile_id_.clear();
}

void SdlGamepad::publish() {
    auto &input = gungnir::input::state();
    input.set_buttons(gungnir::input::Source::Gamepad, static_cast<int>(id_), 0, state_.buttons());
    input.set_analog(gungnir::input::Source::Gamepad, static_cast<int>(id_), 0, state_.analog());
}

bool SdlGamepad::needs_remap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || guid_.empty()) return false;
    std::error_code file_error;
    if (std::filesystem::exists(store_.file_for(guid_), file_error)) return false;
    char *mapping = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(guid_.c_str()));
    const bool mapped = mapping != nullptr && mapping[0] != '\0';
    SDL_free(mapping);
    return !mapped;
}

void SdlGamepad::release_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.release_all();
    remap_.cancel();
    gungnir::input::host_controls().release_holds();
    publish();
}

void SdlGamepad::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    close();
}

bool SdlGamepad::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool SdlGamepad::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool SdlGamepad::guide_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && pad_ != nullptr && SDL_GamepadHasButton(pad_, SDL_GAMEPAD_BUTTON_GUIDE);
}

std::string SdlGamepad::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return "No gamepad connected";
    std::string text = name_ + "\n" + guid_ + "\nprofile: " + profile_id_;
    if (!last_error_.empty()) text += "\nerror: " + last_error_;
    return text;
}

void SdlGamepad::begin_remap() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return;
    remap_.begin(state_.profile());
    state_.release_all();
    publish();
}

void SdlGamepad::cancel_remap() {
    std::lock_guard<std::mutex> lock(mutex_);
    remap_.cancel();
}

bool SdlGamepad::remap_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remap_.active();
}

bool SdlGamepad::remap_done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remap_.done();
}

std::string SdlGamepad::remap_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return "No gamepad connected";
    if (remap_.done()) return "done";
    if (!remap_.active()) return "Press Remap to start";
    return remap_.status();
}

bool SdlGamepad::save_remap(std::string &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (!connected_) {
        error = "gamepad_not_connected";
        return false;
    }
    if (!remap_.done()) {
        error = "gamepad_remap_incomplete";
        return false;
    }
    gungnir::input::GamepadProfile profile = remap_.profile();
    profile.guid = guid_;
    profile.name = name_;
    if (!store_.save(guid_, profile, error)) return false;
    state_.set_profile(profile);
    profile_id_ = profile.id;
    publish();
    return true;
}

bool SdlGamepad::reset_profile(std::string &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (!connected_) {
        error = "gamepad_not_connected";
        return false;
    }
    gungnir::input::GamepadProfile profile = gungnir::input::default_gamepad_profile();
    profile.guid = guid_;
    profile.name = name_;
    if (!store_.save(guid_, profile, error)) return false;
    state_.set_profile(profile);
    profile_id_ = profile.id;
    publish();
    return true;
}

} // namespace gungnir::platform

#endif

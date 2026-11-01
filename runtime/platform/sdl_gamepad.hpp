#pragma once

// SDL3 gamepad adapter (T11d). Profiles, state and remap are pure; this file
// owns the SDL device, event pump and hotplug lifecycle.

#if defined(GUNGNIR_HAVE_SDL3)

#include "input/gamepad_profile.hpp"
#include "input/gamepad_remap.hpp"
#include "input/gamepad_state.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace gungnir::platform {

class SdlGamepad {
public:
    SdlGamepad(std::string shipped_profiles_json, std::filesystem::path user_directory);
    ~SdlGamepad();
    SdlGamepad(const SdlGamepad &) = delete;
    SdlGamepad &operator=(const SdlGamepad &) = delete;

    // Feeds one SDL event. Returns true when it belonged to the gamepad.
    bool handle_event(const SDL_Event &event);

    [[nodiscard]] bool connected() const;
    [[nodiscard]] bool active() const;
    [[nodiscard]] std::string info() const;

    void release_all();
    void shutdown();

    void begin_remap();
    void cancel_remap();
    [[nodiscard]] bool remap_active() const;
    [[nodiscard]] bool remap_done() const;
    [[nodiscard]] std::string remap_status() const;
    [[nodiscard]] bool save_remap(std::string &error);
    [[nodiscard]] bool reset_profile(std::string &error);
    // True when the connected pad is likely unusable: SDL has no mapping for
    // its GUID (buttons arrive at raw indices) and no user remap was saved.
    // The overlay toasts once per connection in that case.
    [[nodiscard]] bool needs_remap() const;

private:
    void open(SDL_JoystickID id);
    void close();
    void publish();
    [[nodiscard]] std::string profile_error() const;

    mutable std::mutex mutex_;
    gungnir::input::GamepadProfileStore store_;
    gungnir::input::GamepadState state_;
    gungnir::input::GamepadRemap remap_;
    SDL_Gamepad *pad_{};
    SDL_JoystickID id_{};
    bool connected_{};
    std::string name_;
    std::string guid_;
    std::string profile_id_;
    std::string last_error_;
};

} // namespace gungnir::platform

#endif

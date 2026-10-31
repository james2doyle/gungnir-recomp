#pragma once

// Gamepad profiles (T11d): the SDL-standard button/axis layout to PSP mapping.
//
// Profiles are pure data and JSON; the SDL adapter and the Android remap UI
// sit outside this header. Button indices are SDL_GamepadButton values and
// axis indices are SDL_GamepadAxis values, checked by static_assert in the
// adapter.

#include "input/psp_buttons.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gungnir::input {

inline constexpr int kGamepadButtonCount = 32;
inline constexpr int kGamepadAxisCount = 8;

struct GamepadProfile {
    std::string id{"default"};
    std::string match; // shipped profiles: case-insensitive device-name substring
    std::string name;
    std::string guid;
    std::array<ButtonMask, kGamepadButtonCount> buttons{};
    int left_x_axis{0};
    int left_y_axis{1};
    float deadzone{0.12f};
    bool invert_y{false};

    friend bool operator==(const GamepadProfile &, const GamepadProfile &) = default;
};

[[nodiscard]] GamepadProfile default_gamepad_profile();

enum class GamepadProfileStatus : std::uint8_t { Ok, Older, Corrupt };

[[nodiscard]] std::string to_json(const GamepadProfile &profile);
[[nodiscard]] GamepadProfileStatus parse_gamepad_profile(
    std::string_view text, GamepadProfile &profile);

struct ShippedGamepadProfiles {
    std::vector<GamepadProfile> profiles;
};

[[nodiscard]] GamepadProfileStatus parse_shipped_profiles(
    std::string_view text, ShippedGamepadProfiles &shipped);
// Picks the first specific `match` substring, else the "*" entry, else the
// built-in default. Never returns an empty profile.
[[nodiscard]] GamepadProfile select_shipped_profile(
    const ShippedGamepadProfiles &shipped, std::string_view device_name);

// Per-device user profiles under app-private storage, keyed by SDL GUID.
// A missing user file selects a shipped profile; a corrupt one falls back and
// reports a named error. Saves use a sibling temporary file and rename.
class GamepadProfileStore {
public:
    GamepadProfileStore(std::string shipped_json, std::filesystem::path user_directory);

    [[nodiscard]] GamepadProfile load(
        std::string_view guid, std::string_view name, std::string &error) const;
    [[nodiscard]] bool save(
        std::string_view guid, const GamepadProfile &profile, std::string &error) const;
    [[nodiscard]] std::filesystem::path file_for(std::string_view guid) const;

private:
    ShippedGamepadProfiles shipped_;
    bool shipped_valid_{};
    std::filesystem::path user_directory_;
};

} // namespace gungnir::input

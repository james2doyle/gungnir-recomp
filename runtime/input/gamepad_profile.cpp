#include "input/gamepad_profile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace gungnir::input {
namespace {

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (char &c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

bool contains_ascii_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return false;
    return lower_ascii(haystack).find(lower_ascii(needle)) != std::string::npos;
}

std::string clean_id(std::string_view guid) {
    std::string id;
    for (const char c : guid) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') id.push_back(c);
    }
    return id.empty() ? "unknown" : id;
}

GamepadProfileStatus profile_from_json(const nlohmann::json &root, GamepadProfile &profile) {
    if (!root.is_object()) return GamepadProfileStatus::Corrupt;
    GamepadProfile candidate = default_gamepad_profile();
    if (root.contains("id")) {
        if (!root["id"].is_string()) return GamepadProfileStatus::Corrupt;
        candidate.id = root["id"].get<std::string>();
    }
    if (root.contains("match")) {
        if (!root["match"].is_string()) return GamepadProfileStatus::Corrupt;
        candidate.match = root["match"].get<std::string>();
    }
    if (root.contains("name")) {
        if (!root["name"].is_string()) return GamepadProfileStatus::Corrupt;
        candidate.name = root["name"].get<std::string>();
    }
    if (root.contains("guid")) {
        if (!root["guid"].is_string()) return GamepadProfileStatus::Corrupt;
        candidate.guid = root["guid"].get<std::string>();
    }
    if (!root.contains("buttons") || !root["buttons"].is_array() ||
        root["buttons"].size() != kGamepadButtonCount)
        return GamepadProfileStatus::Corrupt;
    for (int i = 0; i < kGamepadButtonCount; ++i) {
        const nlohmann::json &value = root["buttons"][i];
        if (!value.is_number_unsigned() && !value.is_number_integer())
            return GamepadProfileStatus::Corrupt;
        const auto mask = value.get<std::uint64_t>();
        if (mask > 0xFFFFFFFFull) return GamepadProfileStatus::Corrupt;
        candidate.buttons[static_cast<std::size_t>(i)] = static_cast<ButtonMask>(mask);
    }
    if (root.contains("left_x_axis")) {
        if (!root["left_x_axis"].is_number_integer()) return GamepadProfileStatus::Corrupt;
        candidate.left_x_axis = root["left_x_axis"].get<int>();
    }
    if (root.contains("left_y_axis")) {
        if (!root["left_y_axis"].is_number_integer()) return GamepadProfileStatus::Corrupt;
        candidate.left_y_axis = root["left_y_axis"].get<int>();
    }
    if (candidate.left_x_axis < 0 || candidate.left_x_axis >= kGamepadAxisCount ||
        candidate.left_y_axis < 0 || candidate.left_y_axis >= kGamepadAxisCount ||
        candidate.left_x_axis == candidate.left_y_axis)
        return GamepadProfileStatus::Corrupt;
    if (root.contains("deadzone")) {
        if (!root["deadzone"].is_number()) return GamepadProfileStatus::Corrupt;
        candidate.deadzone = root["deadzone"].get<float>();
    }
    if (!std::isfinite(candidate.deadzone) || candidate.deadzone < 0.0f || candidate.deadzone >= 1.0f)
        return GamepadProfileStatus::Corrupt;
    if (root.contains("invert_y")) {
        if (!root["invert_y"].is_boolean()) return GamepadProfileStatus::Corrupt;
        candidate.invert_y = root["invert_y"].get<bool>();
    }
    profile = std::move(candidate);
    return GamepadProfileStatus::Ok;
}

} // namespace

GamepadProfile default_gamepad_profile() {
    GamepadProfile profile;
    profile.buttons[0] = kCrossBit;
    profile.buttons[1] = kCircleBit;
    profile.buttons[2] = kSquareBit;
    profile.buttons[3] = kTriangleBit;
    profile.buttons[4] = kSelectBit;
    profile.buttons[6] = kStartBit;
    profile.buttons[9] = kLBit;
    profile.buttons[10] = kRBit;
    profile.buttons[11] = kUpBit;
    profile.buttons[12] = kDownBit;
    profile.buttons[13] = kLeftBit;
    profile.buttons[14] = kRightBit;
    return profile;
}

std::string to_json(const GamepadProfile &profile) {
    nlohmann::json root;
    root["version"] = 1;
    root["id"] = profile.id;
    root["match"] = profile.match;
    root["name"] = profile.name;
    root["guid"] = profile.guid;
    nlohmann::json buttons = nlohmann::json::array();
    for (const ButtonMask mask : profile.buttons) buttons.push_back(mask);
    root["buttons"] = std::move(buttons);
    root["left_x_axis"] = profile.left_x_axis;
    root["left_y_axis"] = profile.left_y_axis;
    root["deadzone"] = profile.deadzone;
    root["invert_y"] = profile.invert_y;
    return root.dump();
}

GamepadProfileStatus parse_gamepad_profile(std::string_view text, GamepadProfile &profile) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::exception &) {
        return GamepadProfileStatus::Corrupt;
    }
    if (!root.is_object() || !root.contains("version") || !root["version"].is_number_integer())
        return GamepadProfileStatus::Corrupt;
    const int version = root["version"].get<int>();
    if (version < 1) return GamepadProfileStatus::Older;
    if (version != 1) return GamepadProfileStatus::Corrupt;
    return profile_from_json(root, profile);
}

GamepadProfileStatus parse_shipped_profiles(std::string_view text, ShippedGamepadProfiles &shipped) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::exception &) {
        return GamepadProfileStatus::Corrupt;
    }
    if (!root.is_object() || !root.contains("version") || !root["version"].is_number_integer())
        return GamepadProfileStatus::Corrupt;
    const int version = root["version"].get<int>();
    if (version < 1) return GamepadProfileStatus::Older;
    if (version != 1) return GamepadProfileStatus::Corrupt;
    if (!root.contains("profiles") || !root["profiles"].is_array() ||
        root["profiles"].empty() || root["profiles"].size() > 32)
        return GamepadProfileStatus::Corrupt;
    ShippedGamepadProfiles parsed;
    for (const nlohmann::json &entry : root["profiles"]) {
        GamepadProfile profile;
        if (profile_from_json(entry, profile) != GamepadProfileStatus::Ok)
            return GamepadProfileStatus::Corrupt;
        parsed.profiles.push_back(std::move(profile));
    }
    shipped = std::move(parsed);
    return GamepadProfileStatus::Ok;
}

GamepadProfile select_shipped_profile(
    const ShippedGamepadProfiles &shipped, std::string_view device_name) {
    const GamepadProfile *fallback = nullptr;
    for (const GamepadProfile &profile : shipped.profiles) {
        if (profile.match == "*" || profile.match.empty()) {
            if (fallback == nullptr) fallback = &profile;
            continue;
        }
        if (contains_ascii_ci(device_name, profile.match)) return profile;
    }
    if (fallback != nullptr) return *fallback;
    if (!shipped.profiles.empty()) return shipped.profiles.front();
    return default_gamepad_profile();
}

GamepadProfileStore::GamepadProfileStore(
    std::string shipped_json, std::filesystem::path user_directory)
    : user_directory_(std::move(user_directory)) {
    shipped_valid_ =
        parse_shipped_profiles(shipped_json, shipped_) == GamepadProfileStatus::Ok;
}

std::filesystem::path GamepadProfileStore::file_for(std::string_view guid) const {
    return user_directory_ / (clean_id(guid) + ".json");
}

GamepadProfile GamepadProfileStore::load(
    std::string_view guid, std::string_view name, std::string &error) const {
    error.clear();
    const std::filesystem::path path = file_for(guid);
    std::error_code file_error;
    if (std::filesystem::exists(path, file_error)) {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            const std::string text(
                (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            GamepadProfile profile;
            const GamepadProfileStatus status = parse_gamepad_profile(text, profile);
            if (status == GamepadProfileStatus::Ok) {
                if (profile.guid.empty()) profile.guid = std::string(guid);
                if (profile.name.empty()) profile.name = std::string(name);
                return profile;
            }
            error = status == GamepadProfileStatus::Older ? "gamepad_profile_older"
                                                          : "gamepad_profile_corrupt";
        } else {
            error = "gamepad_profile_unreadable";
        }
    } else if (file_error) {
        error = "gamepad_profile_unreadable";
    }

    if (!shipped_valid_ && error.empty()) error = "gamepad_profiles_shipped_invalid";
    GamepadProfile profile = select_shipped_profile(shipped_, name);
    if (profile.guid.empty()) profile.guid = std::string(guid);
    if (profile.name.empty()) profile.name = std::string(name);
    return profile;
}

bool GamepadProfileStore::save(
    std::string_view guid, const GamepadProfile &profile, std::string &error) const {
    error.clear();
    std::error_code directory_error;
    std::filesystem::create_directories(user_directory_, directory_error);
    if (directory_error) {
        error = "gamepad_profile_directory_failed";
        return false;
    }
    const std::filesystem::path path = file_for(guid);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "gamepad_profile_open_failed";
            return false;
        }
        const std::string text = to_json(profile);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "gamepad_profile_write_failed";
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "gamepad_profile_rename_failed";
        return false;
    }
    return true;
}

} // namespace gungnir::input

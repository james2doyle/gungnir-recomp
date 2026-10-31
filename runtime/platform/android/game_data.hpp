#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gungnir::platform {

struct GameIdentity {
    std::uint64_t size{};
    std::uint32_t crc32{};
};

inline constexpr GameIdentity kGungnirUsaIdentity{262766592ull, 0x183F3EDCu};

struct GameDataSource {
    std::filesystem::path path;
    std::string origin;
};

struct GameDataResult {
    std::filesystem::path path;
    std::string origin;
    std::uint64_t size{};
    std::uint32_t crc32{};
    std::string error;
    bool ok{false};
};

std::vector<GameDataSource> find_game_data(
    const std::vector<std::filesystem::path> &directories);
GameDataResult verify_game_data(const std::filesystem::path &path, GameIdentity expected);
GameDataResult select_game_data(const std::vector<std::filesystem::path> &directories,
                                GameIdentity expected);

} // namespace gungnir::platform

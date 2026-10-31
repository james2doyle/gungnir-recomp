#include "platform/android/game_data.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>

#include <zlib.h>

namespace gungnir::platform {
namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool is_iso(const std::filesystem::path &path) {
    return lowercase(path.extension().string()) == ".iso";
}

} // namespace

std::vector<GameDataSource> find_game_data(
    const std::vector<std::filesystem::path> &directories) {
    std::vector<GameDataSource> found;
    for (const std::filesystem::path &directory : directories) {
        const std::filesystem::path game_directory = directory / "game";
        std::error_code error;
        if (!std::filesystem::is_directory(game_directory, error) || error) continue;
        for (std::filesystem::directory_iterator it(game_directory, error), end; !error && it != end;
             it.increment(error)) {
            std::error_code file_error;
            if (!it->is_regular_file(file_error) || file_error) continue;
            if (!is_iso(it->path())) continue;
            found.push_back({it->path(), directory.string()});
        }
    }
    std::sort(found.begin(), found.end(), [](const GameDataSource &a, const GameDataSource &b) {
        return a.path.string() < b.path.string();
    });
    return found;
}

GameDataResult verify_game_data(const std::filesystem::path &path, GameIdentity expected) {
    GameDataResult result;
    result.path = path;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        result.error = "game_data_open_failed";
        return result;
    }
    result.size = size;
    if (size != expected.size) {
        result.error = "identity_size_mismatch";
        return result;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "game_data_open_failed";
        return result;
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    std::array<unsigned char, 1u << 20> buffer{};
    while (file) {
        file.read(reinterpret_cast<char *>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = file.gcount();
        if (read > 0) crc = crc32(crc, buffer.data(), static_cast<uInt>(read));
    }
    if (!file.eof()) {
        result.error = "game_data_read_failed";
        return result;
    }
    result.crc32 = static_cast<std::uint32_t>(crc);
    if (result.crc32 != expected.crc32) {
        result.error = "identity_crc_mismatch";
        return result;
    }
    result.ok = true;
    return result;
}

GameDataResult select_game_data(const std::vector<std::filesystem::path> &directories,
                                GameIdentity expected) {
    const std::vector<GameDataSource> candidates = find_game_data(directories);
    if (candidates.empty()) {
        GameDataResult result;
        result.error = "no_game_data";
        return result;
    }

    GameDataResult first_failure;
    for (const GameDataSource &candidate : candidates) {
        GameDataResult result = verify_game_data(candidate.path, expected);
        result.origin = candidate.origin;
        if (result.ok) return result;
        if (first_failure.error.empty()) first_failure = result;
    }
    return first_failure;
}

} // namespace gungnir::platform

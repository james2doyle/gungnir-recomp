#pragma once

// RGBA8 PNG I/O for texture dump/replacement. libpng performs the codec; this
// module only normalises input to RGBA8 and names failures.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gungnir::tex {

struct PngImage {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};

struct PngReadResult {
    PngImage image;
    std::string error;
    bool ok{false};
};

struct PngWriteResult {
    std::string error;
    bool ok{false};
};

PngReadResult read_png(const std::filesystem::path &path);
PngWriteResult write_png(const std::filesystem::path &path, int width, int height,
                         std::span<const std::uint8_t> rgba);

} // namespace gungnir::tex

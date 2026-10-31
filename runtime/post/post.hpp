#pragma once

// Deterministic post-processing chain (T7g). The CPU path is the parity
// oracle and screenshot-hash fixture; the same pass parameters drive the
// GLSL programs in assets/shaders/post/.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gungnir::post {

struct Image {
    int width{};
    int height{};
    std::vector<std::uint32_t> pixels;

    bool operator==(const Image &) const = default;
};

enum class PassType : std::uint8_t { IntegerScale, Bilinear, FsrLite, Scanlines, LcdGrid };

struct Pass {
    PassType type{};
    int factor{1};
    float scale{1.0f};
    float strength{};
    float sharpness{};
    int period{1};

    bool operator==(const Pass &) const = default;
};

struct Preset {
    std::string name;
    std::vector<Pass> passes;

    bool operator==(const Preset &) const = default;
};

struct LoadResult {
    Preset preset;
    std::string error;
    bool ok{false};
};

LoadResult load_preset(const std::string &json);
LoadResult load_preset_file(const std::string &path);
Preset default_preset();

Image apply(const Image &source, const Preset &preset);
std::uint64_t hash(const Image &image);

} // namespace gungnir::post

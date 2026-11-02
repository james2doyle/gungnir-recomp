#pragma once

// Deterministic CPU rasterizer (T7c): the parity oracle for the Vulkan path.
// Draws triangles, strips, fans, sprites, lines and points into an RGBA8/
// depth16/stencil8 memory target, with scissor, depth, alpha test, stencil-lite,
// blending and GE texture sampling. No Vulkan dependency.

#include "ge/display_list.hpp"
#include "ge/texture.hpp"
#include "ge/vertex.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace gungnir::ge {

enum class CompareFunc : std::uint8_t {
    Never,
    Always,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

bool compare(CompareFunc function, std::uint32_t incoming, std::uint32_t stored);

enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    // PSP BLENDMODE codes 0/1 are the "other color": the destination color
    // for the source operand, the source color for the destination operand.
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    DoubleSrcAlpha,
    DoubleOneMinusSrcAlpha,
    DoubleDstAlpha,
    DoubleOneMinusDstAlpha,
    // Codes 10+: per-channel FIX color from BLENDFIXEDA (source) / B (dest).
    Fix,
};

enum class BlendEquation : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

struct BlendState {
    bool enabled{};
    BlendFactor source{BlendFactor::One};
    BlendFactor destination{BlendFactor::Zero};
    BlendEquation equation{BlendEquation::Add};
    std::uint32_t fixed_source{0u};
    std::uint32_t fixed_destination{0u};

    bool operator==(const BlendState &) const = default;
};

struct StencilState {
    bool test{};
    bool write{};
    std::uint8_t reference{};
    CompareFunc function{CompareFunc::Always};

    bool operator==(const StencilState &) const = default;
};

struct RasterState {
    int scissor_x0{};
    int scissor_y0{};
    int scissor_x1{};
    int scissor_y1{};
    bool depth_test{};
    bool depth_write{};
    CompareFunc depth_function{CompareFunc::Less};
    bool alpha_test{};
    CompareFunc alpha_function{CompareFunc::Greater};
    std::uint8_t alpha_reference{};
    StencilState stencil{};
    BlendState blend{};
    bool texture_enabled{};
    const Texture *texture{};
    // Write mask: bits set = keep destination (PMASK 0xE8/0xE9, or the
    // CLEAR-mode bits). Zero means a full write.
    std::uint32_t write_mask{};
    // Fog: mix toward fog_color (RGB) by the per-vertex fog factor.
    bool fog_enabled{};
    std::uint32_t fog_color{};

    bool operator==(const RasterState &) const = default;
};

class Framebuffer {
public:
    Framebuffer(int width, int height);

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    void clear(std::uint32_t color, std::uint16_t depth = 0xFFFFu, std::uint8_t stencil = 0u);
    [[nodiscard]] std::uint32_t color_at(int x, int y) const;
    [[nodiscard]] std::uint16_t depth_at(int x, int y) const;
    [[nodiscard]] std::uint8_t stencil_at(int x, int y) const;
    [[nodiscard]] std::uint64_t hash() const;

    std::vector<std::uint32_t> color;
    std::vector<std::uint16_t> depth;
    std::vector<std::uint8_t> stencil;

private:
    int width_{};
    int height_{};
};

class SoftwareRasterizer {
public:
    SoftwareRasterizer(Framebuffer &target, RasterState state = RasterState{});

    void draw_primitive(Primitive primitive, std::span<const ScreenVertex> vertices);
    void draw_triangle(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c);
    void draw_line(const ScreenVertex &a, const ScreenVertex &b);
    void draw_point(const ScreenVertex &point);
    void draw_sprite(const ScreenVertex &top_left, const ScreenVertex &bottom_right);

    [[nodiscard]] RasterState &state() { return state_; }
    [[nodiscard]] const RasterState &state() const { return state_; }
    [[nodiscard]] Framebuffer &target() { return target_; }

private:
    struct Fragment {
        float z{};
        float inv_w{1.0f};
        std::array<float, 3> texcoord{0.0f, 0.0f, 1.0f};
        std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
        float fog{1.0f};
    };

    void shade_pixel(int x, int y, const Fragment &fragment);
    [[nodiscard]] bool in_scissor(int x, int y) const;

    Framebuffer &target_;
    RasterState state_;
};

} // namespace gungnir::ge

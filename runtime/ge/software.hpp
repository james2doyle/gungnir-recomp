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

// Stencil ops (STENCILOP 0xDD): applied to the stencil value when the
// stencil test fails, the depth test fails, or everything passes.
enum class StencilOp : std::uint8_t { Keep, Zero, Replace, Invert, Increment, Decrement };

// Maps a GE stencil-op code (STENCILOP 0xDD fields) to an op; codes above
// Decrement behave as Decrement (both behavioral references agree).
[[nodiscard]] StencilOp stencil_op_from_ge(std::uint32_t code);
// Applies a stencil op to the stored value with the given reference.
[[nodiscard]] std::uint8_t apply_stencil_op(StencilOp op, std::uint8_t stored, std::uint8_t reference);

// GE texture function (TEXFUNC 0xC9 low 3 bits): how the texel combines
// with the vertex color. Bit 8 selects RGBA mode (texture alpha feeds
// the result alpha); bit 16 doubles the color result. Blend reads the
// TEXENVCOLOR register (0xCA).
enum class TextureFunc : std::uint8_t { Modulate, Decal, Blend, Replace, Add };

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
    std::uint8_t reference{};
    CompareFunc function{CompareFunc::Always};
    // Low 8 bits of the stencil value participate in the test.
    std::uint8_t mask{0xFFu};
    StencilOp sfail{StencilOp::Keep};
    StencilOp zfail{StencilOp::Keep};
    StencilOp zpass{StencilOp::Keep};

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
    // Texture combine (TEXFUNC 0xC9 / TEXENVCOLOR 0xCA).
    TextureFunc tex_func{TextureFunc::Modulate};
    bool tex_rgba{};
    bool color_double{};
    std::uint32_t tex_env{};
    bool texture_enabled{};
    const Texture *texture{};
    // Write mask: bits set = keep destination (PMASK 0xE8/0xE9, or the
    // CLEAR-mode bits). Zero means a full write.
    std::uint32_t write_mask{};
    // CLEAR mode (0xD3): bypasses alpha/depth/stencil tests and blending;
    // writes vertex color/depth/alpha straight through the masks.
    bool clear{};
    // 0xD3 bit 10 set keeps depth (does not clear it).
    bool clear_depth_write{true};
    // Draw-target pixel format (FBPPIXFORMAT 0xD2): 5650/5551/4444
    // targets quantize on write; RGBA8888 stores exactly.
    TextureFormat fb_format{TextureFormat::Rgba8888};
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

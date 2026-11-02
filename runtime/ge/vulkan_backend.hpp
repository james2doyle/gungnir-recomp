#pragma once

// Vulkan 1.1 GE backend (T7e): instance/device/queue setup, headless
// offscreen rendering with pixel readback, and a windowed swapchain whose
// surface is supplied by the platform layer. Shaders are compiled to SPIR-V
// at build time and embedded in the binary.

#include "platform/adreno/loader.hpp"

#if defined(__ANDROID__)
#include <volk.h>
#else
#include <vulkan/vulkan.h>
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ge/software.hpp"

namespace gungnir::ge {

struct VulkanFrame {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};

struct VulkanDrawVertex {
    float x{};
    float y{};
    float z{};
    float u{};
    float v{};
    float r{1.0f};
    float g{1.0f};
    float b{1.0f};
    float a{1.0f};
    float w{1.0f};
    float q{1.0f};
    float fog{1.0f};
};

struct VulkanDrawState {
    int scissor_x0{};
    int scissor_y0{};
    int scissor_x1{};
    int scissor_y1{};
    bool depth_test{};
    bool depth_write{};
    CompareFunc depth_function{CompareFunc::Less};
    CullMode cull{CullMode::None};
    FrontFace front_face{FrontFace::Clockwise};
    BlendState blend{};
    bool alpha_test{};
    CompareFunc alpha_function{CompareFunc::Greater};
    std::uint8_t alpha_reference{};
    const TextureImage *texture{};
    std::uint64_t texture_key{};
    bool linear{};
    // TEXWRAP (0xC7) clamp flags: true means clamp to edge, false repeats.
    bool clamp_u{};
    bool clamp_v{};
    // Write mask: bits set = keep destination. Channel-exact masks map to
    // the pipeline color write mask; anything else falls back to a full
    // write (named gap, logged at pipeline build).
    std::uint32_t write_mask{};
    bool fog_enabled{};
    std::uint32_t fog_color{};
};

struct VulkanDrawCall {
    std::span<const VulkanDrawVertex> vertices;
    std::span<const std::uint16_t> indices;
    VulkanDrawState state;
};

struct ScissorRect {
    int x0{};
    int y0{};
    int x1{};
    int y1{};
};

struct ViewportRect {
    int x{};
    int y{};
    int width{};
    int height{};
};

// Fits a guest framebuffer into the target surface without changing its
// aspect ratio, centring the result (pillar/letterbox).
ViewportRect fit_viewport(int virtual_width, int virtual_height, int target_width, int target_height);

// Maps a scissor expressed in the guest virtual framebuffer onto the
// swapchain extent. Edges round outward; an inverted or empty input yields
// an empty rect.
ScissorRect scale_scissor_to_extent(ScissorRect rect, int source_width, int source_height, int target_width,
                                    int target_height);

inline VulkanDrawVertex to_vulkan_vertex(const ScreenVertex &vertex) {
    return {vertex.x,
            vertex.y,
            vertex.z,
            vertex.texcoord[0],
            vertex.texcoord[1],
            vertex.color[0],
            vertex.color[1],
            vertex.color[2],
            vertex.color[3],
            vertex.inv_w != 0.0f ? 1.0f / vertex.inv_w : 1.0f,
            vertex.texcoord[2],
            vertex.fog};
}

class VulkanBackend {
public:
    struct Config {
        int width{256};
        int height{256};
        // Guest framebuffer size in pixels. Zero means "same as width/height".
        // Window resizes change width/height but never this virtual extent.
        int virtual_width{};
        int virtual_height{};
        bool headless{true};
        bool validation{false};
        // Bound on retained GPU textures. Eviction runs while the device is
        // idle at the start of the next frame, so descriptors in flight stay
        // valid. A single oversized texture is always retained.
        std::size_t texture_cache_limit{512};
        std::size_t texture_cache_bytes{128u * 1024u * 1024u};
        std::vector<const char *> instance_extensions;
        gungnir::platform::VulkanDriverSelection driver;
        std::function<VkSurfaceKHR(VkInstance)> create_surface;
    };

    static bool available(std::string &reason);

    VulkanBackend();
    ~VulkanBackend();
    VulkanBackend(const VulkanBackend &) = delete;
    VulkanBackend &operator=(const VulkanBackend &) = delete;

    bool initialize(const Config &config, std::string &error);
    bool render_headless(VulkanFrame &frame, std::string &error);
    bool render_headless_draw(std::span<const VulkanDrawVertex> vertices, std::span<const std::uint16_t> indices,
                              const VulkanDrawState &state, VulkanFrame &frame, std::string &error);
    bool render_headless_draws(std::span<const VulkanDrawCall> calls, VulkanFrame &frame, std::string &error);
    bool render_swapchain(std::string &error);
    // Presents a frame built from guest GE draws. An empty call list clears
    // the swapchain without drawing the synthetic triangle.
    bool render_swapchain_draws(std::span<const VulkanDrawCall> calls, std::string &error);
    bool recreate_swapchain(int width, int height, std::string &error);
    bool recreate_surface(std::string &error);
    void invalidate_texture(std::uint64_t key);
    void wait_idle();
    void shutdown();

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool headless() const;
    [[nodiscard]] std::size_t pipeline_count() const;
    [[nodiscard]] std::size_t texture_count() const;
    [[nodiscard]] std::size_t texture_bytes() const;
    [[nodiscard]] int swapchain_width() const;
    [[nodiscard]] int swapchain_height() const;
    [[nodiscard]] VkSurfaceTransformFlagBitsKHR surface_transform() const;
    [[nodiscard]] const gungnir::platform::VulkanDriverInfo &driver_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::ge

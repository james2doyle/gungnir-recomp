#pragma once

// Vulkan 1.1 GE backend (T7e): instance/device/queue setup, headless
// offscreen rendering with pixel readback, and a windowed swapchain whose
// surface is supplied by the platform layer. Shaders are compiled to SPIR-V
// at build time and embedded in the binary.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gungnir::ge {

struct VulkanFrame {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};

class VulkanBackend {
public:
    struct Config {
        int width{256};
        int height{256};
        bool headless{true};
        bool validation{false};
        std::vector<const char *> instance_extensions;
        std::function<VkSurfaceKHR(VkInstance)> create_surface;
    };

    static bool available(std::string &reason);

    VulkanBackend();
    ~VulkanBackend();
    VulkanBackend(const VulkanBackend &) = delete;
    VulkanBackend &operator=(const VulkanBackend &) = delete;

    bool initialize(const Config &config, std::string &error);
    bool render_headless(VulkanFrame &frame, std::string &error);
    bool render_swapchain(std::string &error);
    bool recreate_swapchain(int width, int height, std::string &error);
    void wait_idle();
    void shutdown();

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool headless() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::ge

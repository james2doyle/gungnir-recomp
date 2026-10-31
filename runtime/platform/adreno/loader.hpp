#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace gungnir::platform {

struct VulkanDriverSelection {
    bool custom{false};
    std::string custom_driver_dir;
    std::string custom_driver_name;
    std::string hook_lib_dir;
    std::string tmp_lib_dir;
    std::string file_redirect_dir;
};

struct VulkanDriverInfo {
    bool loaded{false};
    bool custom{false};
    bool fell_back{false};
    std::string fallback_error;
    std::string driver_name;
    std::string driver_info;
    std::uint32_t api_version{0};
};

using SystemOpenFn = std::function<void *(const char *name)>;
using SymbolLookupFn = std::function<void *(void *handle, const char *name)>;
using CustomOpenFn = std::function<void *(const VulkanDriverSelection &selection)>;
using CloseFn = std::function<void(void *handle)>;
using PathCheckFn = std::function<std::string(const std::string &path)>;

struct LoaderHooks {
    SystemOpenFn open_system;
    SymbolLookupFn symbol;
    CustomOpenFn open_custom;
    CloseFn close;
    PathCheckFn check_driver_dir;
    PathCheckFn check_hook_dir;
};

struct VulkanLoaderResult {
    bool ok{false};
    std::string error;
    VulkanDriverInfo info;
};

VulkanLoaderResult initialize_vulkan_loader(const VulkanDriverSelection &selection,
                                            const LoaderHooks &hooks);

VulkanLoaderResult initialize_vulkan_loader(const VulkanDriverSelection &selection);

bool read_driver_selection(const std::filesystem::path &path,
                           VulkanDriverSelection &selection, std::string &error);

void shutdown_vulkan_loader();

} // namespace gungnir::platform

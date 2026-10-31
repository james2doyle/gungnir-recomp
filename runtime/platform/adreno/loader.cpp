#include "platform/adreno/loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <volk.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(GUNGNIR_HAVE_ADRENOTOOLS) && GUNGNIR_HAVE_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif

namespace gungnir::platform {
namespace {

void *g_module = nullptr;
CloseFn g_close;

void *default_open_system(const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(LoadLibraryA(name));
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *default_symbol(void *handle, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void default_close(void *handle) {
    if (handle == nullptr) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

void *default_open_custom(const VulkanDriverSelection &selection) {
#if defined(GUNGNIR_HAVE_ADRENOTOOLS) && GUNGNIR_HAVE_ADRENOTOOLS
    return adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
        selection.tmp_lib_dir.empty() ? nullptr : selection.tmp_lib_dir.c_str(),
        selection.hook_lib_dir.c_str(), selection.custom_driver_dir.c_str(),
        selection.custom_driver_name.c_str(),
        selection.file_redirect_dir.empty() ? nullptr
                                            : selection.file_redirect_dir.c_str(),
        nullptr);
#else
    (void)selection;
    return nullptr;
#endif
}

void install_entry_points(void *symbol) {
#if defined(__ANDROID__)
    volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol));
#else
    (void)symbol;
#endif
}

bool valid_custom_selection(const VulkanDriverSelection &selection) {
    return !selection.custom_driver_dir.empty() && !selection.custom_driver_name.empty() &&
           !selection.hook_lib_dir.empty();
}

} // namespace

VulkanLoaderResult initialize_vulkan_loader(const VulkanDriverSelection &selection,
                                            const LoaderHooks &hooks) {
    shutdown_vulkan_loader();

    LoaderHooks resolved = hooks;
    if (!resolved.open_system) resolved.open_system = default_open_system;
    if (!resolved.symbol) resolved.symbol = default_symbol;
    if (!resolved.close) resolved.close = default_close;
    if (!resolved.open_custom) {
#if defined(GUNGNIR_HAVE_ADRENOTOOLS) && GUNGNIR_HAVE_ADRENOTOOLS
        resolved.open_custom = default_open_custom;
#endif
    }

    VulkanLoaderResult result;
    std::string custom_error;
    if (selection.custom) {
        if (!valid_custom_selection(selection)) {
            custom_error = "driver_load_failed";
        } else if (!resolved.open_custom) {
            custom_error = "driver_abi_unsupported";
        } else {
            void *handle = resolved.open_custom(selection);
            if (handle == nullptr) {
                custom_error = "driver_load_failed";
            } else {
                void *symbol = resolved.symbol(handle, "vkGetInstanceProcAddr");
                if (symbol == nullptr) {
                    resolved.close(handle);
                    custom_error = "driver_load_failed";
                } else {
                    g_module = handle;
                    g_close = resolved.close;
                    install_entry_points(symbol);
                    result.ok = true;
                    result.info.loaded = true;
                    result.info.custom = true;
                    return result;
                }
            }
        }
    }

    const char *const names[] = {
#if defined(_WIN32)
        "vulkan-1.dll",
#elif defined(__APPLE__)
        "libvulkan.dylib", "libvulkan.1.dylib", "libMoltenVK.dylib",
#else
        "libvulkan.so.1", "libvulkan.so",
#endif
    };

    void *handle = nullptr;
    for (const char *name : names) {
        handle = resolved.open_system(name);
        if (handle != nullptr) break;
    }
    if (handle == nullptr) {
        result.error = "vulkan_loader_unavailable";
        result.info.fallback_error = custom_error;
        return result;
    }

    void *symbol = resolved.symbol(handle, "vkGetInstanceProcAddr");
    if (symbol == nullptr) {
        resolved.close(handle);
        result.error = "vulkan_loader_unavailable";
        result.info.fallback_error = custom_error;
        return result;
    }

    g_module = handle;
    g_close = resolved.close;
    install_entry_points(symbol);
    result.ok = true;
    result.info.loaded = true;
    result.info.custom = false;
    result.info.fell_back = selection.custom;
    result.info.fallback_error = custom_error;
    return result;
}

VulkanLoaderResult initialize_vulkan_loader(const VulkanDriverSelection &selection) {
    return initialize_vulkan_loader(selection, {});
}

bool read_driver_selection(const std::filesystem::path &path,
                           VulkanDriverSelection &selection, std::string &error) {
    selection = {};
    error.clear();
    std::ifstream input(path);
    if (!input) return true;

    std::vector<std::pair<std::string, std::string>> values;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto equals = line.find('=', first);
        if (equals == std::string::npos) {
            error = "driver_selection_invalid";
            return false;
        }
        std::string key = line.substr(first, equals - first);
        std::string value = line.substr(equals + 1);
        const auto key_end = key.find_last_not_of(" \t");
        key.erase(key_end == std::string::npos ? 0 : key_end + 1);
        const auto value_first = value.find_first_not_of(" \t");
        value.erase(0, value_first == std::string::npos ? value.size() : value_first);
        const auto value_end = value.find_last_not_of(" \t");
        value.erase(value_end == std::string::npos ? 0 : value_end + 1);
        values.emplace_back(std::move(key), std::move(value));
    }

    auto lookup = [&](const std::string &key) -> std::string {
        for (const auto &[candidate, value] : values) {
            if (candidate == key) return value;
        }
        return {};
    };
    const std::string custom = lookup("custom");
    if (custom == "false") {
        selection.custom = false;
        return true;
    }
    if (custom != "true") {
        error = "driver_selection_invalid";
        return false;
    }

    selection.custom = true;
    selection.custom_driver_dir = lookup("driver_dir");
    selection.custom_driver_name = lookup("soname");
    selection.hook_lib_dir = lookup("hook_lib_dir");
    selection.tmp_lib_dir = lookup("tmp_lib_dir");
    const bool absolute = std::filesystem::path(selection.custom_driver_dir).is_absolute() &&
                          std::filesystem::path(selection.hook_lib_dir).is_absolute() &&
                          (selection.tmp_lib_dir.empty() ||
                           std::filesystem::path(selection.tmp_lib_dir).is_absolute());
    if (selection.custom_driver_dir.empty() || selection.custom_driver_name.empty() ||
        selection.hook_lib_dir.empty() || !absolute) {
        error = "driver_selection_invalid";
        return false;
    }
    return true;
}

void shutdown_vulkan_loader() {
    if (g_module != nullptr) {
        if (g_close) g_close(g_module);
        g_module = nullptr;
    }
    g_close = {};
#if defined(__ANDROID__)
    volkFinalize();
#endif
}

} // namespace gungnir::platform

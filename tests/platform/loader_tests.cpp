// T10c contract tests: loader selection, system/custom paths and fallback.
// Openers are injected, so no Vulkan driver is loaded.

#include "platform/adreno/loader.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir::platform;

void check(bool condition, const std::string &contract) {
    if (!condition) throw std::runtime_error(contract);
}

void check_equal(const std::string &actual, const std::string &expected,
                 const std::string &contract) {
    check(actual == expected, contract + ": expected " + expected + ", got " + actual);
}

void *fake_symbol(void *handle, const char *name) {
    (void)handle;
    (void)name;
    return reinterpret_cast<void *>(&fake_symbol);
}

struct Recorder {
    std::vector<std::string> system_names;
    int custom_calls{0};
    int close_calls{0};
    VulkanDriverSelection last_selection;
};

LoaderHooks hooks(Recorder &recorder, bool custom_handle, bool system_handle) {
    LoaderHooks hooks;
    hooks.open_system = [&recorder, system_handle](const char *name) -> void * {
        recorder.system_names.emplace_back(name);
        return system_handle ? reinterpret_cast<void *>(0x1000) : nullptr;
    };
    hooks.symbol = [](void *handle, const char *name) -> void * {
        (void)handle;
        (void)name;
        return reinterpret_cast<void *>(&fake_symbol);
    };
    hooks.open_custom = [&recorder, custom_handle](const VulkanDriverSelection &selection) -> void * {
        ++recorder.custom_calls;
        recorder.last_selection = selection;
        return custom_handle ? reinterpret_cast<void *>(0x2000) : nullptr;
    };
    hooks.close = [&recorder](void *) { ++recorder.close_calls; };
    return hooks;
}

void system_path() {
    Recorder recorder;
    const VulkanLoaderResult result = initialize_vulkan_loader({}, hooks(recorder, true, true));
    check(result.ok, "system loader opens");
    check(!result.info.custom && result.info.loaded, "system driver is reported");
    check(recorder.custom_calls == 0, "system path never calls the custom opener");
    check(recorder.system_names.size() == 1, "first system name is used");
    check(result.info.fallback_error.empty(), "system path has no fallback");

    Recorder fallback;
    LoaderHooks fallback_hooks = hooks(fallback, true, false);
    fallback_hooks.open_system = [&fallback](const char *name) -> void * {
        fallback.system_names.emplace_back(name);
        return fallback.system_names.size() == 2 ? reinterpret_cast<void *>(0x1000) : nullptr;
    };
    const VulkanLoaderResult second = initialize_vulkan_loader({}, fallback_hooks);
    check(second.ok, "second system name is tried");
    check(fallback.system_names.size() == 2, "system names are tried in order");

    Recorder none;
    LoaderHooks none_hooks = hooks(none, true, false);
    none_hooks.open_system = [&none](const char *name) -> void * {
        none.system_names.emplace_back(name);
        return nullptr;
    };
    const VulkanLoaderResult failed = initialize_vulkan_loader({}, none_hooks);
    check(!failed.ok, "missing system loader fails");
    check_equal(failed.error, "vulkan_loader_unavailable", "missing system loader is named");
    shutdown_vulkan_loader();
}

void custom_path() {
    Recorder recorder;
    VulkanDriverSelection selection;
    selection.custom = true;
    selection.custom_driver_dir = "/data/user/0/com.gungnir.recomp/files/drivers/abc";
    selection.custom_driver_name = "vulkan.ad07XX.so";
    selection.hook_lib_dir = "/data/app/lib/arm64";
    selection.tmp_lib_dir = "/data/user/0/com.gungnir.recomp/cache";
    selection.file_redirect_dir = "/data/user/0/com.gungnir.recomp/files/vk";

    const VulkanLoaderResult result =
        initialize_vulkan_loader(selection, hooks(recorder, true, false));
    check(result.ok, "custom loader opens");
    check(result.info.custom && result.info.loaded, "custom driver is reported");
    check(recorder.custom_calls == 1, "custom opener is called once");
    check(recorder.system_names.empty(), "custom success does not open the system loader");
    check_equal(recorder.last_selection.custom_driver_name, "vulkan.ad07XX.so",
                "selection is forwarded");
    check_equal(recorder.last_selection.hook_lib_dir, "/data/app/lib/arm64",
                "hook dir is forwarded");

    Recorder fallback;
    const VulkanLoaderResult fell_back =
        initialize_vulkan_loader(selection, hooks(fallback, false, true));
    check(fell_back.ok, "custom failure falls back to system");
    check(!fell_back.info.custom && fell_back.info.fell_back, "fallback is reported");
    check_equal(fell_back.info.fallback_error, "driver_load_failed", "custom failure is named");

    Recorder missing;
    LoaderHooks no_custom = hooks(missing, false, true);
    no_custom.open_custom = {};
    const VulkanLoaderResult unsupported = initialize_vulkan_loader(selection, no_custom);
    check(unsupported.ok, "unsupported custom path still boots on system");
    check_equal(unsupported.info.fallback_error, "driver_abi_unsupported",
                "unsupported ABI is named");

    Recorder no_symbol;
    LoaderHooks broken = hooks(no_symbol, true, true);
    broken.symbol = [](void *handle, const char *) -> void * {
        return handle == reinterpret_cast<void *>(0x2000)
                   ? nullptr
                   : reinterpret_cast<void *>(&fake_symbol);
    };
    const VulkanLoaderResult broken_result = initialize_vulkan_loader(selection, broken);
    check(broken_result.ok, "missing entry point falls back to system");
    check_equal(broken_result.info.fallback_error, "driver_load_failed",
                "missing entry point is named");
    check(no_symbol.close_calls == 1, "failed custom handle is closed");

    Recorder invalid;
    VulkanDriverSelection empty;
    empty.custom = true;
    const VulkanLoaderResult invalid_result =
        initialize_vulkan_loader(empty, hooks(invalid, true, true));
    check(invalid_result.ok, "invalid selection falls back to system");
    check_equal(invalid_result.info.fallback_error, "driver_load_failed",
                "invalid selection is named");
    check(invalid.custom_calls == 0, "invalid selection never calls the custom opener");

    shutdown_vulkan_loader();
}

void total_failure_and_close() {
    Recorder recorder;
    VulkanDriverSelection selection;
    selection.custom = true;
    selection.custom_driver_dir = "/drivers/abc";
    selection.custom_driver_name = "vulkan.ad07XX.so";
    selection.hook_lib_dir = "/lib";
    LoaderHooks failing = hooks(recorder, false, false);
    failing.open_system = [&recorder](const char *name) -> void * {
        recorder.system_names.emplace_back(name);
        return nullptr;
    };
    const VulkanLoaderResult result = initialize_vulkan_loader(selection, failing);
    check(!result.ok, "both paths failing is an error");
    check_equal(result.error, "vulkan_loader_unavailable", "total failure is named");
    check_equal(result.info.fallback_error, "driver_load_failed", "custom failure is retained");

    Recorder closing;
    const VulkanLoaderResult opened =
        initialize_vulkan_loader({}, hooks(closing, true, true));
    check(opened.ok, "close test opens the system loader");
    check(closing.close_calls == 0, "close is not called while active");
    shutdown_vulkan_loader();
    check(closing.close_calls == 1, "shutdown closes the active module");
    shutdown_vulkan_loader();
    check(closing.close_calls == 1, "double shutdown is harmless");
}

void selection_file() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("gungnir_selection_" + std::to_string(stamp));
    std::error_code fs_error;
    std::filesystem::remove_all(directory, fs_error);
    std::filesystem::create_directories(directory);

    VulkanDriverSelection selection;
    std::string error;
    check(read_driver_selection(directory / "missing", selection, error) && !selection.custom &&
              error.empty(),
          "missing selection file means system");

    {
        std::ofstream file(directory / "system.txt");
        file << "custom=false\n";
    }
    check(read_driver_selection(directory / "system.txt", selection, error) && !selection.custom,
          "system selection parses");

    {
        std::ofstream file(directory / "custom.txt");
        file << "custom=true\n"
             << "driver_dir=/data/user/0/com.gungnir.recomp/files/drivers/abc\n"
             << "soname=vulkan.ad07XX.so\n"
             << "hook_lib_dir=/data/app/lib/arm64\n"
             << "tmp_lib_dir=/data/user/0/com.gungnir.recomp/cache\n";
    }
    check(read_driver_selection(directory / "custom.txt", selection, error),
          "custom selection parses: " + error);
    check(selection.custom && selection.custom_driver_name == "vulkan.ad07XX.so" &&
              selection.hook_lib_dir == "/data/app/lib/arm64",
          "custom selection fields are read");

    {
        std::ofstream file(directory / "malformed.txt");
        file << "custom=true\nnot-a-pair\n";
    }
    check(!read_driver_selection(directory / "malformed.txt", selection, error),
          "malformed selection is rejected");
    check_equal(error, "driver_selection_invalid", "malformed selection is named");

    {
        std::ofstream file(directory / "incomplete.txt");
        file << "custom=true\n"
             << "driver_dir=/drivers/abc\n"
             << "soname=vulkan.ad07XX.so\n";
    }
    check(!read_driver_selection(directory / "incomplete.txt", selection, error),
          "incomplete selection is rejected");
    check_equal(error, "driver_selection_invalid", "incomplete selection is named");

    {
        std::ofstream file(directory / "relative.txt");
        file << "custom=true\n"
             << "driver_dir=drivers/abc\n"
             << "soname=vulkan.ad07XX.so\n"
             << "hook_lib_dir=/data/app/lib/arm64\n";
    }
    check(!read_driver_selection(directory / "relative.txt", selection, error),
          "relative driver directory is rejected");

    std::filesystem::remove_all(directory, fs_error);
}

} // namespace

int main() {
    try {
        system_path();
        custom_path();
        total_failure_and_close();
        selection_file();
        std::cout << "loader_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

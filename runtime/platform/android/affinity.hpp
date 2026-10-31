#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gungnir::platform {

struct CpuTuning {
    int cpu{-1};
    int nice_value{0};
    bool affinity_ok{false};
    bool priority_ok{false};
    std::string affinity_error;
    std::string priority_error;
};

std::vector<int> parse_cpu_list(std::string_view text);

CpuTuning tune_current_thread(
    const std::filesystem::path &status_path,
    const std::function<bool(const std::vector<int> &)> &set_affinity,
    const std::function<bool(int)> &set_priority);

CpuTuning tune_current_thread();

} // namespace gungnir::platform

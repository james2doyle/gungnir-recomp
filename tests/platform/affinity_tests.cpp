// T9e contract tests: CPU-list parsing and best-effort thread tuning.
// Syscalls are injected, so the host test never changes its own affinity.

#include "platform/android/affinity.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace gungnir::platform;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

fs::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() / ("gungnir_affinity_" + std::to_string(stamp));
    std::error_code error;
    fs::remove_all(path, error);
    fs::create_directories(path);
    return path;
}

void write_status(const fs::path &path, const std::string &line) {
    std::ofstream file(path);
    file << "Name:\tgungnir\n" << line << "\n";
}

void parsing() {
    check(parse_cpu_list("0-3,6,8-9") == std::vector<int>({0, 1, 2, 3, 6, 8, 9}),
          "ranges and singletons parse");
    check(parse_cpu_list(" 0-1 , 4 ") == std::vector<int>({0, 1, 4}), "whitespace is ignored");
    check(parse_cpu_list("3,1,3,2") == std::vector<int>({1, 2, 3}), "duplicates are removed");
    check(parse_cpu_list("").empty(), "empty list is empty");
    check(parse_cpu_list("x").empty(), "non-numeric token is rejected");
    check(parse_cpu_list("3-1").empty(), "reversed range is rejected");
    check(parse_cpu_list("0-").empty(), "missing range end is rejected");
    check(parse_cpu_list("0-5000").empty(), "oversized range is rejected");
}

void tuning() {
    const fs::path directory = make_temp_dir();
    const fs::path status = directory / "status";
    write_status(status, "Cpus_allowed_list:\t0-3,6-7");

    std::vector<int> mask;
    int nice_value = 0;
    const CpuTuning applied = tune_current_thread(
        status,
        [&](const std::vector<int> &cpus) {
            mask = cpus;
            return true;
        },
        [&](int value) {
            nice_value = value;
            return true;
        });
    check(applied.affinity_ok && applied.cpu == 7, "highest allowed CPU is selected");
    check(mask == std::vector<int>({7}), "affinity mask contains the selected CPU");
    check(applied.priority_ok && nice_value == -10, "priority request is recorded");

    const CpuTuning denied = tune_current_thread(
        status, [](const std::vector<int> &) { return false; },
        [](int) { return false; });
    check(!denied.affinity_ok && denied.affinity_error == "affinity_denied",
          "denied affinity is named");
    check(!denied.priority_ok && denied.priority_error == "priority_denied",
          "denied priority is named");
    check(denied.cpu == 7, "denied affinity still reports the chosen CPU");

    const fs::path no_list = directory / "no-list";
    write_status(no_list, "Name:\tgungnir");
    const CpuTuning missing = tune_current_thread(
        no_list, [](const std::vector<int> &) { return true; },
        [](int) { return true; });
    check(!missing.affinity_ok && missing.affinity_error == "cpu_list_missing",
          "missing CPU list is named");

    const CpuTuning no_status = tune_current_thread(
        directory / "missing", [](const std::vector<int> &) { return true; },
        [](int) { return true; });
    check(!no_status.affinity_ok && no_status.affinity_error == "cpu_status_missing",
          "missing status file is named");

    std::error_code error;
    fs::remove_all(directory, error);
}

} // namespace

int main() {
    try {
        parsing();
        tuning();
        std::cout << "affinity_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

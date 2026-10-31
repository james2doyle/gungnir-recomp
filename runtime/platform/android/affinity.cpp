#include "platform/android/affinity.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sched.h>
#include <string>
#include <sys/resource.h>

namespace gungnir::platform {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool parse_number(std::string_view text, int &value) {
    if (text.empty()) return false;
    int parsed = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + (c - '0');
        if (parsed > 4096) return false;
    }
    value = parsed;
    return true;
}

bool set_thread_affinity(const std::vector<int> &cpus) {
    if (cpus.empty()) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
        CPU_SET(cpu, &set);
    }
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

bool set_thread_priority(int nice_value) {
    return setpriority(PRIO_PROCESS, 0, nice_value) == 0;
}

} // namespace

std::vector<int> parse_cpu_list(std::string_view text) {
    std::vector<int> cpus;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view token = trim(text.substr(0, comma));
        if (token.empty()) return {};

        const std::size_t dash = token.find('-');
        if (dash == std::string_view::npos) {
            int cpu = 0;
            if (!parse_number(token, cpu)) return {};
            cpus.push_back(cpu);
        } else {
            int first = 0;
            int last = 0;
            if (!parse_number(trim(token.substr(0, dash)), first) ||
                !parse_number(trim(token.substr(dash + 1)), last) || first > last)
                return {};
            for (int cpu = first; cpu <= last; ++cpu) cpus.push_back(cpu);
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return cpus;
}

CpuTuning tune_current_thread(
    const std::filesystem::path &status_path,
    const std::function<bool(const std::vector<int> &)> &set_affinity,
    const std::function<bool(int)> &set_priority) {
    CpuTuning tuning;
    std::ifstream status(status_path);
    if (!status) {
        tuning.affinity_error = "cpu_status_missing";
        tuning.priority_error = "priority_skipped";
        return tuning;
    }

    std::string line;
    bool found = false;
    std::vector<int> cpus;
    while (std::getline(status, line)) {
        constexpr std::string_view prefix = "Cpus_allowed_list:";
        if (line.rfind(prefix, 0) != 0) continue;
        found = true;
        cpus = parse_cpu_list(trim(std::string_view(line).substr(prefix.size())));
        break;
    }

    if (!found) {
        tuning.affinity_error = "cpu_list_missing";
    } else if (cpus.empty()) {
        tuning.affinity_error = "cpu_list_invalid";
    } else {
        tuning.cpu = cpus.back();
        if (set_affinity({tuning.cpu})) {
            tuning.affinity_ok = true;
        } else {
            tuning.affinity_error = "affinity_denied";
        }
    }

    tuning.nice_value = -10;
    if (set_priority(tuning.nice_value)) {
        tuning.priority_ok = true;
    } else {
        tuning.priority_error = "priority_denied";
    }
    return tuning;
}

CpuTuning tune_current_thread() {
    return tune_current_thread("/proc/self/status", set_thread_affinity, set_thread_priority);
}

} // namespace gungnir::platform

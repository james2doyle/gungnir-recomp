#pragma once

// Shared bounded session result and reporting (T13a).
//
// The Linux boot runner and the Android runtime use the same session; this
// header keeps the machine-readable result independent of the guest runtime
// so the stop-cause and log contracts can be tested on the host.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gungnir::session {

enum class StopCause {
    FirstFlip,
    MissingImport,
    DispatchLimit,
    WallClock,
    SchedulerDeadlock,
    HostStop,
    GuestStop,
};

[[nodiscard]] const char *stop_cause_name(StopCause cause) noexcept;
[[nodiscard]] StopCause classify_stop_cause(std::string_view reason) noexcept;

struct Result {
    bool ok{};
    std::string error;
    std::string module;
    std::uint32_t entry{};
    std::uint32_t gp{};
    std::uint32_t image_end{};
    std::size_t functions{};
    std::uint64_t dispatches{};
    std::uint64_t dispatch_work{};
    std::uint64_t vblanks{};
    std::uint64_t virtual_us{};
    bool reached_first_frame_buf{};
    std::uint64_t flips{};
    std::uint32_t first_frame_buf{};
    std::uint32_t display_width{};
    std::uint32_t display_format{};
    bool module_init_complete{};
    StopCause stop_cause{StopCause::GuestStop};
    std::string threads;
    std::uint64_t hle_bound{};
    std::uint64_t imports_total{};
    std::uint64_t imports_bound{};
    std::vector<std::string> imports_unbound;
    std::vector<std::string> imports_executed;
    bool stopped{};
    std::string stop_reason;
    std::uint32_t stop_pc{};
};

struct AudioStats {
    std::uint64_t buffers{};
    std::uint64_t frames{};
    int peak{};
};

// One line for logcat: session_stop cause=... dispatches=... ...
[[nodiscard]] std::string summary_line(const Result &result);

// The boot_runner JSON contract. Audio counters are optional so hosts without
// an audio sink still produce the same shape.
[[nodiscard]] std::string result_json(const Result &result, const AudioStats *audio = nullptr);

} // namespace gungnir::session

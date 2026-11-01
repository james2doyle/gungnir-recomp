#include "session/session_report.hpp"

#include <sstream>
#include <utility>

namespace gungnir::session {
namespace {

std::string hex32(std::uint32_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(value >> shift) & 0xFu]);
    return out;
}

std::string json_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
    }
    return out;
}

std::string join_quoted(const std::vector<std::string> &items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0u) out += ", ";
        out += "\"" + json_escape(items[i]) + "\"";
    }
    return out;
}

} // namespace

const char *stop_cause_name(StopCause cause) noexcept {
    switch (cause) {
    case StopCause::FirstFlip: return "first_flip";
    case StopCause::MissingImport: return "missing_import";
    case StopCause::DispatchLimit: return "dispatch_limit";
    case StopCause::WallClock: return "wall_clock";
    case StopCause::SchedulerDeadlock: return "scheduler_deadlock";
    case StopCause::HostStop: return "host_stop";
    case StopCause::GuestStop: return "guest_stop";
    }
    return "guest_stop";
}

StopCause classify_stop_cause(std::string_view reason) noexcept {
    if (reason.starts_with("first sceDisplaySetFrameBuf")) return StopCause::FirstFlip;
    if (reason.find("Missing HLE import") != std::string_view::npos) return StopCause::MissingImport;
    if (reason.starts_with("Dispatch limit")) return StopCause::DispatchLimit;
    if (reason.find("wall-clock") != std::string_view::npos) return StopCause::WallClock;
    if (reason.find("scheduler deadlock") != std::string_view::npos) return StopCause::SchedulerDeadlock;
    if (reason == "host stop requested") return StopCause::HostStop;
    return StopCause::GuestStop;
}

std::string summary_line(const Result &result) {
    std::ostringstream out;
    out << "session_stop cause=" << stop_cause_name(result.stop_cause)
        << " dispatches=" << result.dispatches
        << " flips=" << result.flips
        << " vblanks=" << result.vblanks
        << " imports_bound=" << result.imports_bound
        << " imports_total=" << result.imports_total
        << " imports_unbound=" << result.imports_unbound.size()
        << " imports_executed=" << result.imports_executed.size()
        << " missing_import=" << (result.stop_cause == StopCause::MissingImport ? 1 : 0)
        << " silent_nids=0"
        << " module_init_complete=" << (result.module_init_complete ? "true" : "false")
        << " stop_pc=" << hex32(result.stop_pc);
    return out.str();
}

std::string result_json(const Result &result, const AudioStats *audio) {
    const AudioStats empty{};
    const AudioStats &stats = audio != nullptr ? *audio : empty;
    std::ostringstream out;
    out << "{\n"
        << "  \"module\": \"" << json_escape(result.module) << "\",\n"
        << "  \"entry\": \"" << hex32(result.entry) << "\",\n"
        << "  \"gp\": \"" << hex32(result.gp) << "\",\n"
        << "  \"image_end\": \"" << hex32(result.image_end) << "\",\n"
        << "  \"functions\": " << result.functions << ",\n"
        << "  \"dispatches\": " << result.dispatches << ",\n"
        << "  \"dispatch_work\": " << result.dispatch_work << ",\n"
        << "  \"vblanks\": " << result.vblanks << ",\n"
        << "  \"virtual_us\": " << result.virtual_us << ",\n"
        << "  \"reached_first_frame_buf\": " << (result.reached_first_frame_buf ? "true" : "false") << ",\n"
        << "  \"flips\": " << result.flips << ",\n"
        << "  \"first_frame_buf\": \"" << hex32(result.first_frame_buf) << "\",\n"
        << "  \"display_width\": " << result.display_width << ",\n"
        << "  \"display_format\": " << result.display_format << ",\n"
        << "  \"module_init_complete\": " << (result.module_init_complete ? "true" : "false") << ",\n"
        << "  \"stop_cause\": \"" << stop_cause_name(result.stop_cause) << "\",\n"
        << "  \"threads\": \"" << json_escape(result.threads) << "\",\n"
        << "  \"audio_buffers\": " << stats.buffers << ",\n"
        << "  \"audio_frames\": " << stats.frames << ",\n"
        << "  \"audio_peak\": " << stats.peak << ",\n"
        << "  \"hle_bound\": " << result.hle_bound << ",\n"
        << "  \"imports_total\": " << result.imports_total << ",\n"
        << "  \"imports_bound\": " << result.imports_bound << ",\n"
        << "  \"imports_unbound\": [" << join_quoted(result.imports_unbound) << "],\n"
        << "  \"imports_executed\": [" << join_quoted(result.imports_executed) << "],\n"
        << "  \"stopped\": " << (result.stopped ? "true" : "false") << ",\n"
        << "  \"stop_reason\": \"" << json_escape(result.stop_reason) << "\",\n"
        << "  \"stop_pc\": \"" << hex32(result.stop_pc) << "\"\n"
        << "}\n";
    return out.str();
}

} // namespace gungnir::session

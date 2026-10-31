#pragma once

#include <cstdint>
#include <string>

namespace gungnir::platform {

struct AudioSmokeResult {
    bool ok{};
    bool vag_ok{};
    bool clean{};
    std::uint32_t duration_ms{};
    std::uint64_t frames_pulled{};
    std::uint64_t underrun_frames{};
    std::int32_t xruns{};
    std::string error;
};

AudioSmokeResult run_audio_smoke(std::uint32_t duration_ms);

} // namespace gungnir::platform

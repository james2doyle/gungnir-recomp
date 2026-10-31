#include "platform/android/audio_smoke.hpp"

#include "audio/audio_mixer.hpp"
#include "audio/sas_core.hpp"
#include "platform/android/aaudio_out.hpp"
#include "psprecomp/guest_memory.hpp"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace gungnir::platform {
namespace {

constexpr std::uint32_t kFrames = 256u;
constexpr double kTwoPi = 6.28318530717958647692;

void write_vag(psprecomp::GuestMemory &memory, std::uint32_t address) {
    memory.store8(address + 0u, 0x02u);
    memory.store8(address + 1u, 0x00u);
    for (std::uint32_t i = 2u; i < 16u; ++i) memory.store8(address + i, 0x11u);
    memory.store8(address + 16u, 0x00u);
    memory.store8(address + 17u, 0x07u);
    for (std::uint32_t i = 18u; i < 32u; ++i) memory.store8(address + i, 0x00u);
}

} // namespace

AudioSmokeResult run_audio_smoke(std::uint32_t duration_ms) {
    AudioSmokeResult result;
    result.duration_ms = std::clamp<std::uint32_t>(duration_ms, 100u, 120'000u);
    gungnir::audio::MixerSink mixer(44'100u);
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t kVagAddress = 0x08810000u;
    write_vag(memory, kVagAddress);
    gungnir::audio::SasCore sas;
    sas.init(kFrames, 32u, 0u);
    sas.set_voice(0u, kVagAddress, 32u, false);
    sas.set_volume(0u, 0x1000, 0x1000);
    sas.key_on(0u);
    std::vector<std::int16_t> vag(kFrames * 2u);
    sas.render(memory, vag.data(), kFrames);
    result.vag_ok = vag[0] == 1023 && vag[1] == 1023;

    gungnir::audio::AudioBuffer buffer{};
    buffer.samples = vag.data();
    buffer.frames = kFrames;
    buffer.channels = 2u;
    buffer.left_volume = 0x8000u;
    buffer.right_volume = 0x8000u;
    if (!mixer.submit(buffer)) {
        result.error = "audio_smoke_mixer_overflow";
        return result;
    }

    std::vector<std::int16_t> tone(kFrames * 2u);
    double phase = 0.0;
    const auto fill_tone = [&]() {
        for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
            const auto value = static_cast<std::int16_t>(8000.0 * std::sin(phase));
            tone[frame * 2u] = value;
            tone[frame * 2u + 1u] = value;
            phase += kTwoPi * 440.0 / 44'100.0;
            if (phase > kTwoPi) phase -= kTwoPi;
        }
    };
    const auto submit_tone = [&]() {
        buffer.samples = tone.data();
        buffer.frames = kFrames;
        buffer.channels = 2u;
        buffer.left_volume = 0x8000u;
        buffer.right_volume = 0x8000u;
        return mixer.submit(buffer);
    };

    for (std::uint32_t block = 0; block < 8u; ++block) {
        fill_tone();
        if (!submit_tone()) {
            result.error = "audio_smoke_mixer_overflow";
            return result;
        }
    }

    std::string error;
    auto device = gungnir::audio::AAudioOut::open(mixer, error);
    if (!device) {
        result.error = error;
        __android_log_print(ANDROID_LOG_ERROR, "gungnir", "audio_smoke_failed: %s", error.c_str());
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(result.duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mixer.available_frames() + kFrames <= mixer.capacity_frames()) {
            fill_tone();
            if (!submit_tone()) {
                result.error = "audio_smoke_mixer_overflow";
                return result;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    const auto stats = mixer.stats();
    result.frames_pulled = stats.frames_pulled;
    result.underrun_frames = stats.underrun_frames;
    result.xruns = device->xruns();
    result.clean = result.underrun_frames == 0u && result.xruns == 0;
    result.ok = stats.frames_pulled > 0u && result.vag_ok;
    __android_log_print(ANDROID_LOG_INFO, "gungnir",
        "audio_smoke ok=%d vag=%d clean=%d duration_ms=%u pulled=%llu underrun=%llu xruns=%d",
        result.ok ? 1 : 0, result.vag_ok ? 1 : 0, result.clean ? 1 : 0, result.duration_ms,
        static_cast<unsigned long long>(result.frames_pulled),
        static_cast<unsigned long long>(result.underrun_frames), result.xruns);
    return result;
}

} // namespace gungnir::platform

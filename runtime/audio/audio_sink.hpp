#pragma once

#include <cstdint>
#include <vector>

namespace gungnir::audio {

// One guest buffer handed to the host the moment the channel starts playing it.
// `samples` is interleaved 16-bit PCM in guest order, valid only for the call.
struct AudioBuffer {
    const std::int16_t *samples{};
    std::uint32_t frames{};
    std::uint32_t channels{2}; // 1 = mono, 2 = stereo
    std::uint32_t left_volume{};
    std::uint32_t right_volume{};
    std::uint32_t channel{};
    std::uint64_t start_us{}; // virtual PSP time the buffer starts playing
};

// Host playback seam. The desktop/Android device backend lands with the audio
// work; the boot runner installs an explicit counting sink so a run can prove
// what the guest produced instead of silently discarding it.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Returns false when the backend cannot accept the buffer. The caller
    // stops with a named error rather than pretending it played.
    virtual bool submit(const AudioBuffer &buffer) = 0;
};

void set_audio_sink(AudioSink *sink);
[[nodiscard]] AudioSink *audio_sink();

} // namespace gungnir::audio

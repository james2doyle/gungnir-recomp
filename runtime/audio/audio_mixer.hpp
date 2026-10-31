#pragma once

#include "audio/audio_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace gungnir::audio {

struct MixerStats {
    std::uint64_t frames_pulled{};
    std::uint64_t underrun_frames{};
    std::uint64_t submitted_buffers{};
    std::uint64_t overruns{};
    std::uint64_t rejected_buffers{};
};

// Interleaved s16 stereo ring fed by sceAudio buffers and drained by an
// AAudio/SDL callback. Volumes are 0..0x8000 per channel; a full ring is a
// rejected submission (loud), never a silent drop.
class MixerSink final : public AudioSink {
public:
    explicit MixerSink(std::size_t capacity_frames = 44'100u);

    bool submit(const AudioBuffer &buffer) override;

    // Serves exactly output.size()/2 frames; missing input is silence and is
    // counted in underrun_frames.
    std::size_t pull(std::span<std::int16_t> output);

    void reset();

    [[nodiscard]] MixerStats stats() const;
    [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacity_frames_; }

private:
    std::size_t capacity_frames_{};
    std::vector<std::int16_t> ring_;
    std::size_t read_{};
    std::size_t available_{};
    MixerStats stats_{};
    mutable std::mutex mutex_;
};

} // namespace gungnir::audio

#pragma once

#include "audio/audio_sink.hpp"

#include <array>
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
    std::uint64_t resyncs{};
    std::uint32_t peak{};
};

// Mixes every sceAudio channel onto one 44.1 kHz timeline. Each channel keeps
// its own cursor: a submitted buffer is added at the channel's next free
// position, so channels overlap instead of playing one after another. A
// channel that falls behind the device restarts just ahead of it (a counted
// resync); a timeline that outgrows the ring is dropped and restarted (a
// counted overrun). Neither is a rejected submission, because a full ring is
// a pacing condition the guest cannot observe, not a bad buffer.
class MixerSink final : public AudioSink {
public:
    explicit MixerSink(std::size_t capacity_frames = 44'100u, std::size_t lead_frames = 2'048u);

    bool submit(const AudioBuffer &buffer) override;

    // Serves exactly output.size()/2 frames from the mixed timeline; missing
    // input is silence and is counted in underrun_frames.
    std::size_t pull(std::span<std::int16_t> output);

    void reset();

    [[nodiscard]] MixerStats stats() const;
    [[nodiscard]] std::size_t available_frames() const;
    [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacity_frames_; }

private:
    struct ChannelState {
        std::uint64_t cursor{};
        bool started{};
    };

    void reset_locked();
    void add_locked(std::uint64_t position, std::int32_t left, std::int32_t right) noexcept;

    std::size_t capacity_frames_{};
    std::size_t lead_frames_{};
    std::vector<std::int32_t> ring_;
    std::uint64_t read_{};
    std::uint64_t write_head_{};
    std::array<ChannelState, 9> channels_{};
    MixerStats stats_{};
    mutable std::mutex mutex_;
};

} // namespace gungnir::audio

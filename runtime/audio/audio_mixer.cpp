#include "audio/audio_mixer.hpp"

#include <algorithm>
#include <limits>

namespace gungnir::audio {
namespace {

std::int16_t scale(std::int32_t sample, std::uint32_t volume) {
    const std::uint32_t gain = std::min<std::uint32_t>(volume, 0x8000u);
    const std::int64_t value = static_cast<std::int64_t>(sample) * gain / 0x8000;
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        value, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
}

} // namespace

MixerSink::MixerSink(std::size_t capacity_frames)
    : capacity_frames_(std::max<std::size_t>(capacity_frames, 1u)),
      ring_(capacity_frames_ * 2u) {}

bool MixerSink::submit(const AudioBuffer &buffer) {
    if (buffer.samples == nullptr || buffer.frames == 0u || (buffer.channels != 1u && buffer.channels != 2u)) {
        std::lock_guard lock(mutex_);
        ++stats_.rejected_buffers;
        return false;
    }
    const std::size_t frames = buffer.frames;
    std::lock_guard lock(mutex_);
    if (frames > capacity_frames_ - available_) {
        ++stats_.overruns;
        return false;
    }
    std::size_t write = (read_ + available_) % capacity_frames_;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t source = frame * buffer.channels;
        const std::int32_t left = buffer.samples[source];
        const std::int32_t right = buffer.channels == 2u ? buffer.samples[source + 1u] : left;
        ring_[write * 2u] = scale(left, buffer.left_volume);
        ring_[write * 2u + 1u] = scale(right, buffer.right_volume);
        const std::uint32_t magnitude = static_cast<std::uint32_t>(
            std::max(std::abs(ring_[write * 2u]), std::abs(ring_[write * 2u + 1u])));
        stats_.peak = std::max(stats_.peak, magnitude);
        write = (write + 1u) % capacity_frames_;
    }
    available_ += frames;
    ++stats_.submitted_buffers;
    return true;
}

std::size_t MixerSink::pull(std::span<std::int16_t> output) {
    const std::size_t frames = output.size() / 2u;
    std::lock_guard lock(mutex_);
    const std::size_t served = std::min(frames, available_);
    for (std::size_t frame = 0; frame < served; ++frame) {
        output[frame * 2u] = ring_[read_ * 2u];
        output[frame * 2u + 1u] = ring_[read_ * 2u + 1u];
        read_ = (read_ + 1u) % capacity_frames_;
    }
    for (std::size_t frame = served; frame < frames; ++frame) {
        output[frame * 2u] = 0;
        output[frame * 2u + 1u] = 0;
    }
    available_ -= served;
    stats_.frames_pulled += frames;
    stats_.underrun_frames += frames - served;
    if (output.size() % 2u != 0u) output.back() = 0;
    return frames;
}

void MixerSink::reset() {
    std::lock_guard lock(mutex_);
    read_ = 0u;
    available_ = 0u;
    stats_ = {};
}

MixerStats MixerSink::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t MixerSink::available_frames() const {
    std::lock_guard lock(mutex_);
    return available_;
}

} // namespace gungnir::audio

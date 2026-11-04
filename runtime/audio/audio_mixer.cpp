#include "audio/audio_mixer.hpp"

#include <algorithm>
#include <limits>

namespace gungnir::audio {
namespace {

constexpr std::uint32_t kChannelCount = 9u;
constexpr std::uint64_t kResyncFloor = 64u;

std::int32_t scale(std::int32_t sample, std::uint32_t volume) {
    const std::uint32_t gain = std::min<std::uint32_t>(volume, 0x8000u);
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(gain) / 0x8000);
}

std::int16_t clamp16(std::int32_t value) {
    return static_cast<std::int16_t>(
        std::clamp(value, -32768, 32767));
}

} // namespace

MixerSink::MixerSink(std::size_t capacity_frames, std::size_t lead_frames)
    : capacity_frames_(std::max<std::size_t>(capacity_frames, 1u)),
      lead_frames_(std::min(lead_frames, capacity_frames_ / 2u)),
      ring_(capacity_frames_ * 2u, 0) {}

void MixerSink::add_locked(std::uint64_t position, std::int32_t left, std::int32_t right) noexcept {
    const std::size_t frame = static_cast<std::size_t>(position % capacity_frames_);
    ring_[frame * 2u] += left;
    ring_[frame * 2u + 1u] += right;
}

void MixerSink::reset_locked() {
    std::fill(ring_.begin(), ring_.end(), 0);
    read_ = 0u;
    write_head_ = 0u;
    channels_ = {};
}

bool MixerSink::submit(const AudioBuffer &buffer) {
    if (buffer.samples == nullptr || buffer.frames == 0u || (buffer.channels != 1u && buffer.channels != 2u)) {
        std::lock_guard lock(mutex_);
        ++stats_.rejected_buffers;
        return false;
    }
    if (lead_frames_ == 0u && capacity_frames_ < 2u) {
        std::lock_guard lock(mutex_);
        ++stats_.rejected_buffers;
        return false;
    }

    std::lock_guard lock(mutex_);
    if (muted_) {
        // Fast-forward: the guest produces audio faster than the device plays
        // it. Drop it, counted, instead of overrunning the ring.
        ++stats_.muted_buffers;
        return true;
    }
    if (write_head_ > read_ && write_head_ - read_ >= capacity_frames_) {
        ++stats_.overruns;
        reset_locked();
    }

    ChannelState &channel = channels_[std::min<std::uint32_t>(buffer.channel, kChannelCount - 1u)];
    if (!channel.started) {
        channel.cursor = read_ + lead_frames_;
        channel.started = true;
    } else if (channel.cursor + kResyncFloor < read_) {
        channel.cursor = read_ + lead_frames_;
        ++stats_.resyncs;
    }

    const std::uint64_t end = channel.cursor + buffer.frames;
    if (end - read_ > capacity_frames_) {
        ++stats_.overruns;
        reset_locked();
        channel.cursor = read_ + lead_frames_;
        channel.started = true;
    }

    std::uint64_t position = channel.cursor;
    for (std::uint32_t frame = 0u; frame < buffer.frames; ++frame) {
        const std::size_t source = static_cast<std::size_t>(frame) * buffer.channels;
        const std::int32_t left = scale(buffer.samples[source], buffer.left_volume);
        const std::int32_t right = buffer.channels == 2u
            ? scale(buffer.samples[source + 1u], buffer.right_volume)
            : scale(buffer.samples[source], buffer.right_volume);
        add_locked(position, left, right);
        ++position;
    }
    channel.cursor = position;
    write_head_ = std::max(write_head_, position);
    ++stats_.submitted_buffers;
    return true;
}

std::size_t MixerSink::pull(std::span<std::int16_t> output) {
    const std::size_t frames = output.size() / 2u;
    std::lock_guard lock(mutex_);
    for (std::size_t frame = 0u; frame < frames; ++frame) {
        std::int16_t left = 0;
        std::int16_t right = 0;
        if (read_ < write_head_) {
            const std::size_t index = static_cast<std::size_t>(read_ % capacity_frames_) * 2u;
            left = clamp16(ring_[index]);
            right = clamp16(ring_[index + 1u]);
            ring_[index] = 0;
            ring_[index + 1u] = 0;
        } else {
            ++stats_.underrun_frames;
        }
        output[frame * 2u] = left;
        output[frame * 2u + 1u] = right;
        const std::uint32_t magnitude = static_cast<std::uint32_t>(
            std::max(std::abs(static_cast<int>(left)), std::abs(static_cast<int>(right))));
        stats_.peak = std::max(stats_.peak, magnitude);
        ++read_;
    }
    stats_.frames_pulled += frames;
    if (output.size() % 2u != 0u) output.back() = 0;
    return frames;
}

void MixerSink::reset() {
    std::lock_guard lock(mutex_);
    reset_locked();
    stats_ = {};
}

void MixerSink::set_muted(bool muted) {
    std::lock_guard lock(mutex_);
    if (muted_ == muted) return;
    muted_ = muted;
    if (muted_) reset_locked();
}

bool MixerSink::muted() const {
    std::lock_guard lock(mutex_);
    return muted_;
}

MixerStats MixerSink::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t MixerSink::available_frames() const {
    std::lock_guard lock(mutex_);
    return write_head_ > read_ ? static_cast<std::size_t>(write_head_ - read_) : 0u;
}

} // namespace gungnir::audio

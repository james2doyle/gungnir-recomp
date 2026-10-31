// sceAtrac3plus: whole-file RIFF ATRAC3/ATRAC3+ playback over the vendored
// decoder. Streaming/second-buffer calls remain counted gaps and are not
// registered, so a title that needs them stops with a named NID instead of
// receiving fake PCM.
#include "hle/hle_atrac.hpp"

#include "audio/atrac_file.hpp"
#include "hle/kernel.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kCodecAt3Plus = 0x00001000u;
constexpr std::uint32_t kCodecAt3 = 0x00001001u;
constexpr std::uint32_t kBadCodecType = 0x80630004u;
constexpr std::uint32_t kBadAtracId = 0x80630005u;
constexpr std::uint32_t kUnknownFormat = 0x80630006u;
constexpr std::uint32_t kWrongCodecType = 0x80630007u;
constexpr std::uint32_t kNoFreeId = 0x80630003u;
constexpr std::uint32_t kAllDataLoaded = 0x80630009u;
constexpr std::uint32_t kSizeTooSmall = 0x80630011u;
constexpr std::uint32_t kAddDataTooBig = 0x80630018u;
constexpr std::uint32_t kBufferEmpty = 0x80630023u;
constexpr std::uint32_t kBadAlignment = 0x80630014u;
constexpr std::uint32_t kAllDataDecoded = 0x80630024u;
constexpr std::uint32_t kRemainOnMemory = 0xFFFFFFFFu;
constexpr std::size_t kMaxTracks = 6u;

struct Slot {
    bool used{};
    std::uint32_t codec{};
    std::unique_ptr<audio::AtracTrack> track;
    std::uint32_t position{};
    std::uint32_t end_sample{};
    std::uint32_t loop_start{};
    std::uint32_t loop_end{};
    bool has_loop{};
    int loops_left{};
    bool streaming{};
    std::uint32_t stream_buffer{};
    std::uint32_t stream_buffer_size{};
    std::uint32_t stream_offset{};
};

std::array<Slot, kMaxTracks> g_slots{};

void finish(AllegrexContext &ctx, std::uint32_t result) {
    kernel().finish(ctx, result);
}

Slot *slot(std::uint32_t id) {
    if (id == 0u || id > kMaxTracks) return nullptr;
    Slot &candidate = g_slots[id - 1u];
    return candidate.used ? &candidate : nullptr;
}

Slot *allocate(std::uint32_t codec) {
    for (std::size_t index = 0; index < g_slots.size(); ++index) {
        Slot &candidate = g_slots[index];
        if (candidate.used) continue;
        candidate = Slot{};
        candidate.used = true;
        candidate.codec = codec;
        return &candidate;
    }
    return nullptr;
}

std::uint32_t id_of(const Slot *target) {
    return static_cast<std::uint32_t>(target - g_slots.data()) + 1u;
}

void reset_slot(Slot &target) {
    target = Slot{};
}

void set_loop_bounds(Slot &target) {
    const audio::AtracTrackInfo &info = target.track->info();
    const std::uint32_t raw_samples = info.frames() * info.samples_per_frame();
    const std::uint32_t total = info.total_samples != 0u
        ? info.total_samples
        : raw_samples - std::min(raw_samples, info.delay);
    target.end_sample = total != 0u ? total - 1u : 0u;
    if (!info.loops.empty() && info.loops.front().end >= info.loops.front().start) {
        target.has_loop = true;
        target.loop_start = info.loops.front().start - std::min(info.loops.front().start, info.delay);
        target.loop_end = std::min(
            info.loops.front().end - std::min(info.loops.front().end, info.delay), target.end_sample);
    }
}

bool read_file(Runtime &rt, std::uint32_t address, std::uint32_t size, std::vector<std::uint8_t> &file) {
    if (address == 0u || size == 0u || !rt.memory().contains(address, size)) return false;
    const std::uint8_t *source = rt.memory().raw_pointer(address, size);
    if (source == nullptr) return false;
    file.assign(source, source + size);
    return true;
}

std::uint32_t load_track(Runtime &rt, Slot &target, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> file;
    if (!read_file(rt, address, size, file)) return kSizeTooSmall;
    std::string error;
    auto track = audio::AtracTrack::open(file, error);
    if (!track) return kUnknownFormat;
    if (target.codec != 0u) {
        const std::uint32_t expected = track->info().codec == audio::AtracCodec::Atrac3Plus
            ? kCodecAt3Plus
            : kCodecAt3;
        if (expected != target.codec) return kWrongCodecType;
    }
    target.track = std::move(track);
    target.codec = target.track->info().codec == audio::AtracCodec::Atrac3Plus ? kCodecAt3Plus : kCodecAt3;
    target.position = 0u;
    target.loops_left = 0;
    target.has_loop = false;
    target.streaming = false;
    target.stream_buffer = 0u;
    target.stream_buffer_size = 0u;
    target.stream_offset = 0u;
    set_loop_bounds(target);
    return 0u;
}

bool write32(Runtime &rt, std::uint32_t address, std::uint32_t value) {
    if (address == 0u) return true;
    if (!rt.memory().contains(address, 4u)) return false;
    rt.memory().store32(address, value);
    return true;
}

void get_atrac_id(AllegrexContext &ctx) {
    const std::uint32_t codec = arg(ctx, 0);
    if (codec != kCodecAt3Plus && codec != kCodecAt3) {
        finish(ctx, kBadCodecType);
        return;
    }
    Slot *target = allocate(codec);
    finish(ctx, target != nullptr ? id_of(target) : kNoFreeId);
}

void set_data_and_get_id(AllegrexContext &ctx) {
    const std::uint32_t buffer = arg(ctx, 0);
    const std::uint32_t size = arg(ctx, 1);
    Slot *target = allocate(0u);
    if (target == nullptr) {
        finish(ctx, kNoFreeId);
        return;
    }
    const std::uint32_t result = load_track(kernel().runtime(), *target, buffer, size);
    if (result != 0u) {
        reset_slot(*target);
        finish(ctx, result);
        return;
    }
    finish(ctx, id_of(target));
}

void set_data(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    const std::uint32_t result = load_track(kernel().runtime(), *target, arg(ctx, 1), arg(ctx, 2));
    finish(ctx, result);
}

void mark_streaming(Slot &target, std::uint32_t buffer, std::uint32_t buffer_size, std::uint32_t read_size) {
    target.streaming = true;
    target.stream_buffer = buffer;
    target.stream_buffer_size = buffer_size;
    target.stream_offset = read_size;
}

void set_halfway_buffer(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    const std::uint32_t read_size = arg(ctx, 2);
    if (read_size > arg(ctx, 3)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    const std::uint32_t result = load_track(kernel().runtime(), *target, arg(ctx, 1), read_size);
    if (result == 0u) mark_streaming(*target, arg(ctx, 1), arg(ctx, 3), read_size);
    finish(ctx, result);
}

void set_halfway_buffer_and_get_id(AllegrexContext &ctx) {
    const std::uint32_t read_size = arg(ctx, 1);
    if (read_size > arg(ctx, 2)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    Slot *target = allocate(0u);
    if (target == nullptr) {
        finish(ctx, kNoFreeId);
        return;
    }
    const std::uint32_t result = load_track(kernel().runtime(), *target, arg(ctx, 0), read_size);
    if (result != 0u) {
        reset_slot(*target);
        finish(ctx, result);
        return;
    }
    mark_streaming(*target, arg(ctx, 0), arg(ctx, 2), read_size);
    finish(ctx, id_of(target));
}

void release_atrac_id(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    reset_slot(*target);
    finish(ctx, 0u);
}

void get_sound_sample(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    auto &rt = kernel().runtime();
    write32(rt, arg(ctx, 1), target->end_sample);
    write32(rt, arg(ctx, 2), target->has_loop ? target->loop_start : 0xFFFFFFFFu);
    write32(rt, arg(ctx, 3), target->has_loop ? target->loop_end : 0xFFFFFFFFu);
    finish(ctx, 0u);
}

void get_channel(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    if (!write32(kernel().runtime(), arg(ctx, 1), static_cast<std::uint32_t>(target->track->info().channels))) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

void get_max_sample(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    if (!write32(kernel().runtime(), arg(ctx, 1), target->track->info().samples_per_frame())) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

void get_remain_frame(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    const auto &info = target->track->info();
    const std::uint32_t frame = info.samples_per_frame();
    const std::uint32_t frame_index = (target->position + info.delay) / frame;
    std::uint32_t remain = 0u;
    if (frame_index < info.available_frames()) remain = info.available_frames() - frame_index;
    if (!write32(kernel().runtime(), arg(ctx, 1), remain)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

void get_next_sample(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    const auto &info = target->track->info();
    std::uint32_t samples = 0u;
    if (target->position <= target->end_sample) {
        const std::uint32_t raw = target->position + info.delay;
        samples = info.samples_per_frame() - raw % info.samples_per_frame();
    }
    if (!write32(kernel().runtime(), arg(ctx, 1), samples)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

void get_next_decode_position(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    if (!write32(kernel().runtime(), arg(ctx, 1), target->position)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

void get_bitrate(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    const auto &info = target->track->info();
    const std::uint32_t bitrate =
        static_cast<std::uint32_t>(info.block_align) * 8u * info.sample_rate / info.samples_per_frame();
    if (!write32(kernel().runtime(), arg(ctx, 1), bitrate)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

void get_loop_status(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    auto &rt = kernel().runtime();
    write32(rt, arg(ctx, 1), static_cast<std::uint32_t>(target->loops_left));
    write32(rt, arg(ctx, 2), target->has_loop ? 1u : 0u);
    finish(ctx, 0u);
}

void set_loop_num(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    target->loops_left = static_cast<int>(arg(ctx, 1));
    finish(ctx, 0u);
}

void reset_play_position(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    target->position = std::min(arg(ctx, 1), target->end_sample + 1u);
    finish(ctx, 0u);
}

void decode_data(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    auto &rt = kernel().runtime();
    const std::uint32_t output = arg(ctx, 1);
    const auto write = [&](std::uint32_t samples, bool end) {
        write32(rt, arg(ctx, 2), samples);
        write32(rt, arg(ctx, 3), end ? 1u : 0u);
        write32(rt, arg(ctx, 4), kRemainOnMemory);
    };
    if (target->position > target->end_sample) {
        write(0u, true);
        finish(ctx, kAllDataDecoded);
        return;
    }
    const auto &info = target->track->info();
    const std::uint32_t frame_samples = info.samples_per_frame();
    if (info.channels <= 0) {
        finish(ctx, kUnknownFormat);
        return;
    }
    const std::uint32_t raw = target->position + info.delay;
    const std::uint32_t frame_index = raw / frame_samples;
    if (frame_index >= target->track->frames()) {
        write(0u, true);
        finish(ctx, kAllDataDecoded);
        return;
    }
    if (frame_index >= info.available_frames()) {
        write(0u, false);
        finish(ctx, kBufferEmpty);
        return;
    }
    const bool looping = target->has_loop && target->loops_left != 0 && target->position <= target->loop_end;
    const std::uint32_t limit = looping ? target->loop_end : target->end_sample;
    const std::uint32_t count = std::min(frame_samples - raw % frame_samples, limit - target->position + 1u);
    std::vector<std::int16_t> pcm;
    std::string error;
    if (!target->track->decode_frame(frame_index, pcm, error)) {
        finish(ctx, kUnknownFormat);
        return;
    }
    if (output != 0u) {
        if ((output & 1u) != 0u) {
            finish(ctx, kBadAlignment);
            return;
        }
        const std::uint32_t bytes = count * static_cast<std::uint32_t>(info.channels) * 2u;
        if (!rt.memory().contains(output, bytes)) {
            finish(ctx, kSizeTooSmall);
            return;
        }
        std::uint8_t *destination = rt.memory().raw_pointer(output, bytes);
        if (destination == nullptr) {
            finish(ctx, kSizeTooSmall);
            return;
        }
        const std::size_t offset = static_cast<std::size_t>(raw % frame_samples) * static_cast<std::size_t>(info.channels);
        const std::size_t samples = static_cast<std::size_t>(count) * static_cast<std::size_t>(info.channels);
        for (std::size_t i = 0; i < samples; ++i) {
            const auto value = static_cast<std::uint16_t>(pcm[offset + i]);
            destination[i * 2u] = static_cast<std::uint8_t>(value);
            destination[i * 2u + 1u] = static_cast<std::uint8_t>(value >> 8u);
        }
    }
    target->position += count;
    if (looping && target->position > target->loop_end) {
        target->position = target->loop_start;
        if (target->loops_left > 0) --target->loops_left;
    }
    write(count, target->position > target->end_sample);
    finish(ctx, 0u);
}

void get_stream_data_info(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    auto &rt = kernel().runtime();
    std::uint32_t write_pointer = 0u;
    std::uint32_t writable = 0u;
    std::uint32_t read_offset = 0u;
    if (target->streaming) {
        write_pointer = target->stream_buffer;
        writable = target->stream_buffer_size;
        read_offset = target->stream_offset;
    }
    write32(rt, arg(ctx, 1), write_pointer);
    write32(rt, arg(ctx, 2), writable);
    write32(rt, arg(ctx, 3), read_offset);
    finish(ctx, 0u);
}

void add_stream_data(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr || target->track == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    const std::uint32_t bytes = arg(ctx, 1);
    if (!target->streaming) {
        finish(ctx, bytes == 0u ? 0u : kAllDataLoaded);
        return;
    }
    if (bytes == 0u) {
        finish(ctx, 0u);
        return;
    }
    if (bytes > target->stream_buffer_size) {
        finish(ctx, kAddDataTooBig);
        return;
    }
    auto &rt = kernel().runtime();
    if (!rt.memory().contains(target->stream_buffer, bytes)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    const std::uint8_t *source = rt.memory().raw_pointer(target->stream_buffer, bytes);
    if (source == nullptr) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    std::string error;
    if (!target->track->append(std::span<const std::uint8_t>(source, bytes), error)) {
        finish(ctx, kAddDataTooBig);
        return;
    }
    target->stream_offset += bytes;
    finish(ctx, 0u);
}

void get_internal_error(AllegrexContext &ctx) {
    Slot *target = slot(arg(ctx, 0));
    if (target == nullptr) {
        finish(ctx, kBadAtracId);
        return;
    }
    if (!write32(kernel().runtime(), arg(ctx, 1), 0u)) {
        finish(ctx, kSizeTooSmall);
        return;
    }
    finish(ctx, 0u);
}

} // namespace

void reset_atrac_state() {
    for (Slot &target : g_slots) reset_slot(target);
}

void register_atrac(HleRegistrar &hle) {
    hle.add("sceAtrac3plus", "sceAtracGetAtracID", [](Runtime &, AllegrexContext &ctx) { get_atrac_id(ctx); });
    hle.add("sceAtrac3plus", "sceAtracSetDataAndGetID",
        [](Runtime &, AllegrexContext &ctx) { set_data_and_get_id(ctx); });
    hle.add("sceAtrac3plus", "sceAtracSetData", [](Runtime &, AllegrexContext &ctx) { set_data(ctx); });
    hle.add("sceAtrac3plus", "sceAtracSetHalfwayBufferAndGetID",
        [](Runtime &, AllegrexContext &ctx) { set_halfway_buffer_and_get_id(ctx); });
    hle.add("sceAtrac3plus", "sceAtracSetHalfwayBuffer",
        [](Runtime &, AllegrexContext &ctx) { set_halfway_buffer(ctx); });
    hle.add("sceAtrac3plus", "sceAtracReleaseAtracID",
        [](Runtime &, AllegrexContext &ctx) { release_atrac_id(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetSoundSample",
        [](Runtime &, AllegrexContext &ctx) { get_sound_sample(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetChannel", [](Runtime &, AllegrexContext &ctx) { get_channel(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetMaxSample",
        [](Runtime &, AllegrexContext &ctx) { get_max_sample(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetRemainFrame",
        [](Runtime &, AllegrexContext &ctx) { get_remain_frame(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetNextSample",
        [](Runtime &, AllegrexContext &ctx) { get_next_sample(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetNextDecodePosition",
        [](Runtime &, AllegrexContext &ctx) { get_next_decode_position(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetBitrate", [](Runtime &, AllegrexContext &ctx) { get_bitrate(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetLoopStatus",
        [](Runtime &, AllegrexContext &ctx) { get_loop_status(ctx); });
    hle.add("sceAtrac3plus", "sceAtracSetLoopNum", [](Runtime &, AllegrexContext &ctx) { set_loop_num(ctx); });
    hle.add("sceAtrac3plus", "sceAtracResetPlayPosition",
        [](Runtime &, AllegrexContext &ctx) { reset_play_position(ctx); });
    hle.add("sceAtrac3plus", "sceAtracDecodeData", [](Runtime &, AllegrexContext &ctx) { decode_data(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetInternalErrorInfo",
        [](Runtime &, AllegrexContext &ctx) { get_internal_error(ctx); });
    hle.add("sceAtrac3plus", "sceAtracGetStreamDataInfo",
        [](Runtime &, AllegrexContext &ctx) { get_stream_data_info(ctx); });
    hle.add("sceAtrac3plus", "sceAtracAddStreamData",
        [](Runtime &, AllegrexContext &ctx) { add_stream_data(ctx); });
}

} // namespace gungnir::hle

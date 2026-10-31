#include "audio/atrac_file.hpp"

#include "atrac3p_api.h"

#include <algorithm>
#include <cstring>

namespace gungnir::audio {
namespace {

constexpr std::uint8_t kAtrac3PlusGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
    0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};

std::uint16_t read16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (static_cast<std::uint16_t>(data[offset + 1u]) << 8u));
}

std::uint32_t read32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(data[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(data[offset + 3u]) << 24u);
}

bool tag_is(std::span<const std::uint8_t> data, std::size_t offset, const char *tag) {
    return std::memcmp(data.data() + offset, tag, 4u) == 0;
}

} // namespace

std::optional<AtracTrackInfo> parse_atrac_riff(std::span<const std::uint8_t> data) {
    if (data.size() < 12u || !tag_is(data, 0u, "RIFF") || !tag_is(data, 8u, "WAVE")) return std::nullopt;
    AtracTrackInfo info;
    bool have_format = false;
    bool have_data = false;
    for (std::size_t offset = 12u; offset + 8u <= data.size();) {
        const std::uint32_t size = read32(data, offset + 4u);
        const std::size_t body = offset + 8u;
        if (body > data.size()) return std::nullopt;
        if (tag_is(data, offset, "fmt ") && size >= 16u && body + size <= data.size()) {
            const std::uint16_t tag = read16(data, body);
            info.channels = read16(data, body + 2u);
            info.sample_rate = read32(data, body + 4u);
            info.block_align = read16(data, body + 12u);
            if (tag == 0x0270u) {
                info.codec = AtracCodec::Atrac3;
                if (size > 18u) {
                    const std::uint16_t extra = read16(data, body + 16u);
                    const std::size_t begin = body + 18u;
                    const std::size_t end = std::min<std::size_t>(begin + extra, body + size);
                    info.codec_extra.assign(data.begin() + static_cast<std::ptrdiff_t>(begin),
                        data.begin() + static_cast<std::ptrdiff_t>(end));
                }
            } else if (tag == 0xFFFEu && size >= 52u &&
                std::memcmp(data.data() + body + 24u, kAtrac3PlusGuid, 16u) == 0) {
                info.codec = AtracCodec::Atrac3Plus;
                info.codec_params = read16(data, body + 42u);
            } else {
                return std::nullopt;
            }
            if (info.channels <= 0 || info.block_align == 0u) return std::nullopt;
            have_format = true;
        } else if (tag_is(data, offset, "fact") && size >= 8u && body + size <= data.size()) {
            info.total_samples = read32(data, body);
            info.delay = read32(data, body + 4u);
        } else if (tag_is(data, offset, "smpl") && size >= 36u && body + size <= data.size()) {
            const std::uint32_t count = read32(data, body + 28u);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t entry = body + 36u + static_cast<std::size_t>(i) * 24u;
                if (entry + 24u > body + size) break;
                info.loops.push_back({read32(data, entry + 8u), read32(data, entry + 12u)});
            }
        } else if (tag_is(data, offset, "data") && body <= data.size()) {
            info.data_offset = static_cast<std::uint32_t>(body);
            info.data_size = static_cast<std::uint32_t>(std::min<std::size_t>(size, data.size() - body));
            have_data = true;
            break;
        }
        const std::size_t next = body + size + (size & 1u);
        if (next <= offset) break;
        offset = next;
    }
    if (!have_format || !have_data || info.data_size < info.block_align) return std::nullopt;
    return info;
}

struct AtracTrack::Impl {
    std::vector<std::uint8_t> file;
    AtracTrackInfo info;
    Atrac3pHandle *plus{};
    Atrac3Handle *at3{};
};

AtracTrack::AtracTrack(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AtracTrack::~AtracTrack() {
    if (!impl_) return;
    if (impl_->plus != nullptr) atrac3p_destroy(impl_->plus);
    if (impl_->at3 != nullptr) atrac3_destroy(impl_->at3);
}

std::unique_ptr<AtracTrack> AtracTrack::open(std::span<const std::uint8_t> file, std::string &error) {
    error.clear();
    auto parsed = parse_atrac_riff(file);
    if (!parsed) {
        error = "atrac_unknown_format";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->file.assign(file.begin(), file.end());
    impl->info = std::move(*parsed);
    if (impl->info.codec == AtracCodec::Atrac3Plus) {
        if (atrac3p_create(impl->info.channels, impl->info.block_align, &impl->plus) != 0) {
            error = "atrac3p_init_failed";
            return nullptr;
        }
    } else {
        if (atrac3_create(impl->info.channels, impl->info.block_align, impl->info.codec_extra.data(),
                &impl->at3) != 0) {
            error = "atrac3_init_failed";
            return nullptr;
        }
    }
    return std::unique_ptr<AtracTrack>(new AtracTrack(std::move(impl)));
}

const AtracTrackInfo &AtracTrack::info() const noexcept {
    return impl_->info;
}

std::uint32_t AtracTrack::frames() const noexcept {
    return impl_->info.frames();
}

bool AtracTrack::decode_frame(std::uint32_t frame, std::vector<std::int16_t> &pcm, std::string &error) {
    error.clear();
    if (!impl_ || frame >= impl_->info.frames()) {
        error = "atrac_frame_out_of_range";
        return false;
    }
    const std::uint32_t samples_per_frame = impl_->info.samples_per_frame();
    const std::size_t channels = static_cast<std::size_t>(impl_->info.channels);
    pcm.assign(channels * samples_per_frame, 0);
    const std::uint8_t *data = impl_->file.data() + impl_->info.data_offset +
        static_cast<std::size_t>(frame) * impl_->info.block_align;
    int samples = 0;
    int consumed = 0;
    if (impl_->info.codec == AtracCodec::Atrac3Plus) {
        consumed = atrac3p_decode(impl_->plus, data, impl_->info.block_align, pcm.data(), &samples);
    } else {
        consumed = atrac3_decode(impl_->at3, data, impl_->info.block_align, pcm.data(), &samples);
    }
    if (consumed < 0 || samples != static_cast<int>(samples_per_frame)) {
        error = "atrac_decode_failed";
        return false;
    }
    return true;
}

} // namespace gungnir::audio

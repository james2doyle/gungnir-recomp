#include "audio/atrac_file.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    if (condition) return;
    std::cerr << "atrac_track_tests: FAIL: " << what << "\n";
    ++failures;
}

void put16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void put_tag(std::vector<std::uint8_t> &out, const char *tag) {
    out.insert(out.end(), tag, tag + 4);
}

std::vector<std::uint8_t> make_atrac3plus_riff(std::uint32_t frames, std::uint32_t total_samples,
    std::uint32_t delay) {
    constexpr std::uint16_t kBlockAlign = 1536u;
    constexpr std::uint16_t kCodecParams = 0x00BFu;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(frames) * kBlockAlign, 0u);
    for (std::uint32_t frame = 0; frame < frames; ++frame) data[frame * kBlockAlign] = 0x60u;

    std::vector<std::uint8_t> out;
    put_tag(out, "RIFF");
    put32(out, 0u);
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put32(out, 52u);
    put16(out, 0xFFFEu);
    put16(out, 2u);
    put32(out, 44'100u);
    put32(out, static_cast<std::uint32_t>(kBlockAlign) * 44'100u / 2048u);
    put16(out, kBlockAlign);
    put16(out, 0u);
    put16(out, 34u);
    put16(out, 2048u);
    put32(out, 3u);
    static constexpr std::uint8_t kGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
        0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
    out.insert(out.end(), kGuid, kGuid + 16);
    const std::uint8_t extra[12] = {1u, 0u, static_cast<std::uint8_t>(kCodecParams >> 8u),
        static_cast<std::uint8_t>(kCodecParams), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    out.insert(out.end(), extra, extra + 12);
    put_tag(out, "fact");
    put32(out, 8u);
    put32(out, total_samples);
    put32(out, delay);
    put_tag(out, "data");
    put32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t riff_size = static_cast<std::uint32_t>(out.size() - 8u);
    out[4] = static_cast<std::uint8_t>(riff_size);
    out[5] = static_cast<std::uint8_t>(riff_size >> 8u);
    out[6] = static_cast<std::uint8_t>(riff_size >> 16u);
    out[7] = static_cast<std::uint8_t>(riff_size >> 24u);
    return out;
}

void contracts() {
    const auto file = make_atrac3plus_riff(1u, 2048u, 0u);
    const auto info = gungnir::audio::parse_atrac_riff(file);
    check(info.has_value(), "synthetic RIFF parses");
    if (info) {
        check(info->codec == gungnir::audio::AtracCodec::Atrac3Plus, "codec is ATRAC3+");
        check(info->channels == 2, "channels are read");
        check(info->block_align == 1536u, "block align is read");
        check(info->samples_per_frame() == 2048u, "frame sample count is 2048");
        check(info->frames() == 1u, "one frame is present");
        check(info->total_samples == 2048u && info->delay == 0u, "fact fields are read");
    }

    std::string error;
    auto track = gungnir::audio::AtracTrack::open(file, error);
    check(track != nullptr, "decoder opens the synthetic stream");
    if (track) {
        std::vector<std::int16_t> pcm;
        check(track->decode_frame(0u, pcm, error), "terminator frame decodes");
        check(pcm.size() == 2u * 2048u, "decoded frame has the full channel count");
        bool silent = true;
        for (const auto sample : pcm) silent = silent && sample == 0;
        check(silent, "terminator frame decodes to silence");
        check(!track->decode_frame(1u, pcm, error), "out-of-range frame is rejected");
    }

    const auto stream_file = make_atrac3plus_riff(2u, 4096u, 0u);
    const std::size_t first_frame_end = stream_file.size() - 1536u;
    const auto first_chunk = std::span<const std::uint8_t>(stream_file.data(), first_frame_end);
    auto stream_track = gungnir::audio::AtracTrack::open(first_chunk, error);
    check(stream_track != nullptr, "partial stream opens");
    if (stream_track) {
        check(stream_track->info().frames() == 2u, "declared stream length is kept");
        check(stream_track->info().available_frames() == 1u, "only the first frame is available");
        std::vector<std::int16_t> pcm;
        check(stream_track->decode_frame(0u, pcm, error), "first stream frame decodes");
        check(!stream_track->decode_frame(1u, pcm, error), "second stream frame is not available yet");
        const auto full_span = std::span<const std::uint8_t>(stream_file.data(), stream_file.size());
        check(stream_track->decode_frame(1u, full_span, pcm, error),
            "live span decodes past the snapshot without an append");
        const auto second = std::span<const std::uint8_t>(stream_file.data() + first_frame_end, 1536u);
        check(stream_track->append(second, error), "second frame appends");
        check(stream_track->info().available_frames() == 2u, "both frames are available");
        check(stream_track->decode_frame(1u, pcm, error), "second stream frame decodes");
        check(!stream_track->append(second, error), "append past the declared data is rejected");
    }

    const auto truncated = std::vector<std::uint8_t>(file.begin(), file.begin() + 20);
    check(!gungnir::audio::parse_atrac_riff(truncated).has_value(), "truncated RIFF is rejected");
    std::vector<std::uint8_t> garbage(64u, 0xABu);
    check(!gungnir::audio::parse_atrac_riff(garbage).has_value(), "garbage is rejected");
    check(gungnir::audio::AtracTrack::open(garbage, error) == nullptr, "garbage does not open a decoder");
}

} // namespace

int main() {
    contracts();
    if (failures != 0) {
        std::cerr << "atrac_track_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "atrac_track_tests: OK\n";
    return 0;
}

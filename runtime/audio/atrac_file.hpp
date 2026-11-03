#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gungnir::audio {

enum class AtracCodec { Atrac3, Atrac3Plus };

struct AtracLoop {
    std::uint32_t start{};
    std::uint32_t end{};
};

struct AtracTrackInfo {
    AtracCodec codec{AtracCodec::Atrac3Plus};
    int channels{};
    std::uint32_t sample_rate{};
    std::uint16_t block_align{};
    std::uint16_t codec_params{};
    std::uint32_t total_samples{};
    std::uint32_t delay{};
    std::uint32_t data_offset{};
    std::uint32_t data_size{};
    std::uint32_t data_available{};
    std::vector<AtracLoop> loops;
    std::vector<std::uint8_t> codec_extra;

    [[nodiscard]] std::uint32_t samples_per_frame() const {
        return codec == AtracCodec::Atrac3Plus ? 2048u : 1024u;
    }
    [[nodiscard]] std::uint32_t frames() const {
        return block_align != 0u ? data_size / block_align : 0u;
    }
    [[nodiscard]] std::uint32_t available_frames() const {
        return block_align != 0u ? std::min(data_available, data_size) / block_align : 0u;
    }
};

[[nodiscard]] std::optional<AtracTrackInfo> parse_atrac_riff(std::span<const std::uint8_t> file);

class AtracTrack {
public:
    static std::unique_ptr<AtracTrack> open(std::span<const std::uint8_t> file, std::string &error);
    ~AtracTrack();

    AtracTrack(const AtracTrack &) = delete;
    AtracTrack &operator=(const AtracTrack &) = delete;

    [[nodiscard]] const AtracTrackInfo &info() const noexcept;
    [[nodiscard]] std::uint32_t frames() const noexcept;
    bool decode_frame(std::uint32_t frame, std::vector<std::int16_t> &pcm, std::string &error);
    // Same decode but byte-sourced from the caller's span instead of the
    // snapshot (live guest RAM the game keeps filling asynchronously, PPSSPP
    // ignoreDataBuf parity). Bounds-checked against the span; the snapshot
    // and its available count are untouched.
    bool decode_frame(std::uint32_t frame, std::span<const std::uint8_t> file, std::vector<std::int16_t> &pcm,
                      std::string &error);
    bool append(std::span<const std::uint8_t> bytes, std::string &error);
    // Source bytes buffered so far (RIFF + appended stream data). Snapshot
    // re-opens the track from these bytes and restores the play position.
    [[nodiscard]] std::span<const std::uint8_t> file_bytes() const noexcept;

private:
    struct Impl;
    explicit AtracTrack(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::audio

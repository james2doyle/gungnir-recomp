#pragma once

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
    std::vector<AtracLoop> loops;
    std::vector<std::uint8_t> codec_extra;

    [[nodiscard]] std::uint32_t samples_per_frame() const {
        return codec == AtracCodec::Atrac3Plus ? 2048u : 1024u;
    }
    [[nodiscard]] std::uint32_t frames() const {
        return block_align != 0u ? data_size / block_align : 0u;
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

private:
    struct Impl;
    explicit AtracTrack(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::audio

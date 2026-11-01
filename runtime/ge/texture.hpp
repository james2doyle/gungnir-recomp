#pragma once

// Raw GE texture decoding and sampling (T7d). TPL container parsing stays in
// T8; this module works on the bytes the GE points at, including the CLUT.
// Colours are packed RGBA8 with red in the low byte, matching the PSP 8888
// layout. The module has no renderer or Vulkan dependency.

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace gungnir::ge {

enum class TextureFormat : std::uint8_t {
    Rgb565 = 0,
    Rgba5551 = 1,
    Rgba4444 = 2,
    Rgba8888 = 3,
    Clut4 = 4,
    Clut8 = 5,
    Clut16 = 6,
    Clut32 = 7,
};

const char *texture_format_name(TextureFormat format);
bool is_clut_format(TextureFormat format);
int texture_bits_per_texel(TextureFormat format);

constexpr std::uint32_t rgba8(std::uint32_t red, std::uint32_t green, std::uint32_t blue,
                              std::uint32_t alpha = 255u) {
    return (red & 255u) | ((green & 255u) << 8u) | ((blue & 255u) << 16u) | ((alpha & 255u) << 24u);
}

struct TextureState {
    TextureFormat format{TextureFormat::Rgba8888};
    bool swizzled{};
    int width{1};
    int height{1};
    int buffer_width{1};
    bool clamp_u{};
    bool clamp_v{};
    bool linear{};
    std::uint32_t clut_format{};

    bool operator==(const TextureState &) const = default;
};

class Texture {
public:
    Texture(TextureState state, std::span<const std::uint8_t> data, std::span<const std::uint8_t> clut = {});

    [[nodiscard]] const TextureState &state() const { return state_; }
    [[nodiscard]] std::uint32_t texel(int x, int y) const;
    [[nodiscard]] std::uint32_t palette_color(std::uint32_t index) const;
    [[nodiscard]] std::uint32_t sample(float u, float v) const;
    [[nodiscard]] std::uint64_t content_key() const;

    void decode(std::vector<std::uint32_t> &out) const;

private:
    [[nodiscard]] std::uint32_t packed_texel(int x, int y) const;

    TextureState state_;
    std::span<const std::uint8_t> data_;
    std::span<const std::uint8_t> clut_;
};

struct TextureImage {
    int width{};
    int height{};
    std::vector<std::uint32_t> pixels;
};

// Guest bytes backing one texture decode. Kept alive by in-flight draws so a
// frame queued for presentation never reads freed source data.
struct CpuTexture {
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> clut;
    std::optional<Texture> texture;
};

class TextureUploader {
public:
    virtual ~TextureUploader() = default;
    virtual void upload(std::uint64_t key, const TextureImage &image) = 0;
    virtual void invalidate(std::uint64_t key) = 0;
};

class TextureCache {
public:
    [[nodiscard]] const TextureImage *find(std::uint64_t key) const;
    const TextureImage &get(std::uint64_t key, const Texture &texture, TextureUploader *uploader = nullptr);
    [[nodiscard]] std::shared_ptr<const TextureImage> get_shared(std::uint64_t key, const Texture &texture,
                                                                 TextureUploader *uploader = nullptr);
    void invalidate(std::uint64_t key);
    void invalidate(std::uint64_t key, TextureUploader *uploader);
    void clear();
    [[nodiscard]] std::size_t size() const { return images_.size(); }
    [[nodiscard]] std::size_t byte_size() const { return bytes_; }
    void set_budget(std::size_t bytes) { budget_ = bytes; }

private:
    struct Entry {
        std::shared_ptr<const TextureImage> image;
        std::size_t bytes{};
        std::list<std::uint64_t>::iterator order{};
    };

    void evict_over_budget(std::uint64_t keep);

    std::unordered_map<std::uint64_t, Entry> images_;
    std::list<std::uint64_t> order_;
    std::size_t bytes_{};
    std::size_t budget_{64u * 1024u * 1024u};
};

// Guest-address ranges that currently back cached textures. A draw that renders
// into an overlapping range (the "render to area containing texture" feedback
// path) invalidates those cache entries before the next sample.
class TextureRangeIndex {
public:
    void track(std::uint32_t address, std::uint32_t size, std::uint64_t key);
    void forget(std::uint64_t key);
    std::size_t invalidate_write(std::uint32_t address, std::uint32_t size, TextureCache &cache,
                                 TextureUploader *uploader = nullptr);
    std::size_t invalidate_write(std::uint32_t address, std::uint32_t size,
                                 const std::function<void(std::uint64_t)> &invalidate);
    [[nodiscard]] std::vector<std::uint64_t> overlapping(std::uint32_t address, std::uint32_t size) const;
    [[nodiscard]] std::size_t size() const { return ranges_.size(); }
    void clear() { ranges_.clear(); }

private:
    struct Range {
        std::uint32_t address{};
        std::uint32_t size{};
        std::uint64_t key{};
    };
    std::vector<Range> ranges_;
};

} // namespace gungnir::ge

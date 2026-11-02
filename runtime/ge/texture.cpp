#include "ge/texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gungnir::ge {
namespace {

std::uint16_t read_u16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(data[1]) << 8u;
}

std::uint32_t read_u32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8u |
           static_cast<std::uint32_t>(data[2]) << 16u | static_cast<std::uint32_t>(data[3]) << 24u;
}

std::uint32_t expand5(std::uint32_t value) {
    return (value << 3u) | (value >> 2u);
}

std::uint32_t expand6(std::uint32_t value) {
    return (value << 2u) | (value >> 4u);
}

std::uint32_t decode_565(std::uint32_t packed) {
    return rgba8(expand5(packed & 31u), expand6((packed >> 5u) & 63u), expand5((packed >> 11u) & 31u));
}

std::uint32_t decode_5551(std::uint32_t packed) {
    return rgba8(expand5(packed & 31u), expand5((packed >> 5u) & 31u), expand5((packed >> 10u) & 31u),
                 (packed >> 15u) & 1u ? 255u : 0u);
}

std::uint32_t decode_4444(std::uint32_t packed) {
    return rgba8((packed & 15u) * 17u, ((packed >> 4u) & 15u) * 17u, ((packed >> 8u) & 15u) * 17u,
                 ((packed >> 12u) & 15u) * 17u);
}

int wrap_coordinate(int value, int size, bool clamp) {
    if (size <= 1) return 0;
    if (clamp) return std::clamp(value, 0, size - 1);
    value %= size;
    return value < 0 ? value + size : value;
}

std::uint32_t lerp_color(std::uint32_t a, std::uint32_t b, float t) {
    const auto channel = [t](std::uint32_t from, std::uint32_t to, int shift) {
        const float value = static_cast<float>((from >> shift) & 255u) +
                            (static_cast<float>((to >> shift) & 255u) - static_cast<float>((from >> shift) & 255u)) * t;
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
    };
    return rgba8(channel(a, b, 0), channel(a, b, 8), channel(a, b, 16), channel(a, b, 24));
}

void mix_byte(std::uint64_t &hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
}

void mix_u32(std::uint64_t &hash, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) mix_byte(hash, static_cast<std::uint8_t>(value >> shift));
}

void mix_span(std::uint64_t &hash, std::span<const std::uint8_t> bytes) {
    mix_u32(hash, static_cast<std::uint32_t>(bytes.size()));
    for (const std::uint8_t byte : bytes) mix_byte(hash, byte);
}

std::uint32_t palette_color(std::uint32_t index, std::uint32_t format, std::span<const std::uint8_t> clut) {
    if (clut.empty()) return 0u;
    const std::uint32_t shift = (format >> 2u) & 31u;
    const std::uint32_t mask = (format >> 8u) & 0xFFu;
    const std::uint32_t palette_index = ((index >> shift) & mask) | (((format >> 16u) & 31u) << 4u);
    switch (format & 3u) {
    case 0: {
        const std::size_t offset = (palette_index * 2u) & 1023u;
        return offset + 2u <= clut.size() ? decode_565(read_u16(clut.data() + offset)) : 0u;
    }
    case 1: {
        const std::size_t offset = (palette_index * 2u) & 1023u;
        return offset + 2u <= clut.size() ? decode_5551(read_u16(clut.data() + offset)) : 0u;
    }
    case 2: {
        const std::size_t offset = (palette_index * 2u) & 1023u;
        return offset + 2u <= clut.size() ? decode_4444(read_u16(clut.data() + offset)) : 0u;
    }
    default: {
        const std::size_t offset = (palette_index * 4u) & 1023u;
        return offset + 4u <= clut.size() ? read_u32(clut.data() + offset) : 0u;
    }
    }
}

} // namespace

const char *texture_format_name(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgb565:
        return "5650";
    case TextureFormat::Rgba5551:
        return "5551";
    case TextureFormat::Rgba4444:
        return "4444";
    case TextureFormat::Rgba8888:
        return "8888";
    case TextureFormat::Clut4:
        return "clut4";
    case TextureFormat::Clut8:
        return "clut8";
    case TextureFormat::Clut16:
        return "clut16";
    case TextureFormat::Clut32:
        return "clut32";
    }
    return "unknown";
}

bool is_clut_format(TextureFormat format) {
    return format >= TextureFormat::Clut4 && format <= TextureFormat::Clut32;
}

int texture_bits_per_texel(TextureFormat format) {
    static constexpr int bits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    return bits[static_cast<unsigned>(format) & 7u];
}

Texture::Texture(TextureState state, std::span<const std::uint8_t> data, std::span<const std::uint8_t> clut)
    : state_(state), data_(data), clut_(clut) {}

std::uint32_t Texture::palette_color(std::uint32_t index) const {
    return gungnir::ge::palette_color(index, state_.clut_format, clut_);
}

std::uint32_t Texture::packed_texel(int x, int y) const {
    const int bits = texture_bits_per_texel(state_.format);
    const int row_bytes = state_.buffer_width * bits / 8;
    const int byte_x = x * bits / 8;
    std::size_t offset = 0;
    if (state_.swizzled) {
        const int blocks_per_row = std::max(row_bytes / 16, 1);
        offset = static_cast<std::size_t>((y / 8) * blocks_per_row + byte_x / 16) * 128u +
                 static_cast<std::size_t>(y % 8) * 16u + static_cast<std::size_t>(byte_x % 16);
    } else {
        offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(row_bytes) + static_cast<std::size_t>(byte_x);
    }
    const std::uint8_t *data = data_.data() + offset;
    switch (state_.format) {
    case TextureFormat::Rgb565:
        return decode_565(read_u16(data));
    case TextureFormat::Rgba5551:
        return decode_5551(read_u16(data));
    case TextureFormat::Rgba4444:
        return decode_4444(read_u16(data));
    case TextureFormat::Rgba8888:
        return read_u32(data);
    case TextureFormat::Clut4:
        return palette_color((x & 1) != 0 ? static_cast<std::uint32_t>(*data >> 4u) : static_cast<std::uint32_t>(*data & 15u));
    case TextureFormat::Clut8:
        return palette_color(*data);
    case TextureFormat::Clut16:
        return palette_color(read_u16(data));
    case TextureFormat::Clut32:
        return palette_color(read_u32(data));
    }
    return 0u;
}

std::uint32_t Texture::texel(int x, int y) const {
    return packed_texel(x, y);
}

std::uint32_t Texture::sample(float u, float v) const {
    if (state_.linear) {
        const float fx = u * static_cast<float>(state_.width) - 0.5f;
        const float fy = v * static_cast<float>(state_.height) - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        const auto fetch = [this](int x, int y) {
            return texel(wrap_coordinate(x, state_.width, state_.clamp_u),
                         wrap_coordinate(y, state_.height, state_.clamp_v));
        };
        const std::uint32_t top = lerp_color(fetch(x0, y0), fetch(x0 + 1, y0), tx);
        const std::uint32_t bottom = lerp_color(fetch(x0, y0 + 1), fetch(x0 + 1, y0 + 1), tx);
        return lerp_color(top, bottom, ty);
    }

    const int x = static_cast<int>(std::floor(u * static_cast<float>(state_.width)));
    const int y = static_cast<int>(std::floor(v * static_cast<float>(state_.height)));
    return texel(wrap_coordinate(x, state_.width, state_.clamp_u),
                 wrap_coordinate(y, state_.height, state_.clamp_v));
}

void Texture::decode(std::vector<std::uint32_t> &out) const {
    out.resize(static_cast<std::size_t>(state_.width) * static_cast<std::size_t>(state_.height));
    for (int y = 0; y < state_.height; ++y)
        for (int x = 0; x < state_.width; ++x)
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(state_.width) + static_cast<std::size_t>(x)] =
                texel(x, y);
}

std::uint64_t Texture::content_key() const {
    std::uint64_t hash = 14695981039346656037ull;
    mix_u32(hash, static_cast<std::uint32_t>(state_.format));
    mix_byte(hash, state_.swizzled ? 1u : 0u);
    mix_u32(hash, static_cast<std::uint32_t>(state_.width));
    mix_u32(hash, static_cast<std::uint32_t>(state_.height));
    mix_u32(hash, static_cast<std::uint32_t>(state_.buffer_width));
    mix_byte(hash, state_.clamp_u ? 1u : 0u);
    mix_byte(hash, state_.clamp_v ? 1u : 0u);
    mix_byte(hash, state_.linear ? 1u : 0u);
    mix_u32(hash, state_.clut_format);
    mix_span(hash, data_);
    mix_span(hash, clut_);
    return hash;
}

const TextureImage *TextureCache::find(std::uint64_t key) const {
    const auto found = images_.find(key);
    return found == images_.end() ? nullptr : found->second.image.get();
}

std::shared_ptr<const TextureImage> TextureCache::get_shared(std::uint64_t key, const Texture &texture,
                                                             TextureUploader *uploader) {
    const auto found = images_.find(key);
    if (found != images_.end()) {
        order_.splice(order_.end(), order_, found->second.order);
        return found->second.image;
    }

    auto image = std::make_shared<TextureImage>();
    image->width = texture.state().width;
    image->height = texture.state().height;
    texture.decode(image->pixels);

    Entry entry;
    entry.image = image;
    entry.bytes = image->pixels.size() * sizeof(std::uint32_t);
    order_.push_back(key);
    entry.order = std::prev(order_.end());
    bytes_ += entry.bytes;
    images_.emplace(key, std::move(entry));
    evict_over_budget(key);
    if (uploader != nullptr && images_.find(key) != images_.end()) uploader->upload(key, *image);
    return image;
}

const TextureImage &TextureCache::get(std::uint64_t key, const Texture &texture, TextureUploader *uploader) {
    return *get_shared(key, texture, uploader);
}

void TextureCache::evict_over_budget(std::uint64_t keep) {
    while (bytes_ > budget_ && images_.size() > 1u && order_.front() != keep) {
        const std::uint64_t key = order_.front();
        order_.pop_front();
        const auto found = images_.find(key);
        if (found == images_.end()) continue;
        bytes_ -= found->second.bytes;
        images_.erase(found);
    }
}

void TextureCache::invalidate(std::uint64_t key) {
    invalidate(key, nullptr);
}

void TextureCache::invalidate(std::uint64_t key, TextureUploader *uploader) {
    const auto found = images_.find(key);
    if (found == images_.end()) return;
    bytes_ -= found->second.bytes;
    order_.erase(found->second.order);
    images_.erase(found);
    if (uploader != nullptr) uploader->invalidate(key);
}

void TextureCache::clear() {
    images_.clear();
    order_.clear();
    bytes_ = 0u;
}

void TextureRangeIndex::track(std::uint32_t address, std::uint32_t size, std::uint64_t key) {
    forget(key);
    ranges_.push_back({address, size, key});
}

void TextureRangeIndex::forget(std::uint64_t key) {
    std::erase_if(ranges_, [key](const Range &range) { return range.key == key; });
}

std::vector<std::uint64_t> TextureRangeIndex::overlapping(std::uint32_t address, std::uint32_t size) const {
    std::vector<std::uint64_t> keys;
    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
    for (const Range &range : ranges_) {
        const std::uint64_t range_end = static_cast<std::uint64_t>(range.address) + range.size;
        if (static_cast<std::uint64_t>(address) < range_end && static_cast<std::uint64_t>(range.address) < end)
            keys.push_back(range.key);
    }
    return keys;
}

std::size_t TextureRangeIndex::invalidate_write(std::uint32_t address, std::uint32_t size, TextureCache &cache,
                                                TextureUploader *uploader) {
    const std::vector<std::uint64_t> keys = overlapping(address, size);
    for (const std::uint64_t key : keys) {
        cache.invalidate(key, uploader);
        forget(key);
    }
    return keys.size();
}

std::size_t TextureRangeIndex::invalidate_write(std::uint32_t address, std::uint32_t size,
                                                const std::function<void(std::uint64_t)> &invalidate) {
    const std::vector<std::uint64_t> keys = overlapping(address, size);
    for (const std::uint64_t key : keys) {
        invalidate(key);
        forget(key);
    }
    return keys.size();
}

std::uint16_t pack_framebuffer(std::uint32_t color, TextureFormat format) {
    const std::uint32_t r = (color >> 0u) & 255u;
    const std::uint32_t g = (color >> 8u) & 255u;
    const std::uint32_t b = (color >> 16u) & 255u;
    const std::uint32_t a = (color >> 24u) & 255u;
    switch (format) {
    case TextureFormat::Rgb565:
        return static_cast<std::uint16_t>(((r >> 3u) & 31u) | (((g >> 2u) & 63u) << 5u) |
                                          (((b >> 3u) & 31u) << 11u));
    case TextureFormat::Rgba5551:
        return static_cast<std::uint16_t>(((r >> 3u) & 31u) | (((g >> 3u) & 31u) << 5u) |
                                          (((b >> 3u) & 31u) << 10u) | (((a >> 7u) & 1u) << 15u));
    case TextureFormat::Rgba4444:
        return static_cast<std::uint16_t>(((r >> 4u) & 15u) | (((g >> 4u) & 15u) << 4u) |
                                          (((b >> 4u) & 15u) << 8u) | (((a >> 4u) & 15u) << 12u));
    default:
        return 0u;
    }
}

std::uint32_t unpack_framebuffer(std::uint16_t value, TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgb565:
        return rgba8(static_cast<std::uint32_t>((value & 31u) * 255u / 31u),
                     static_cast<std::uint32_t>(((value >> 5u) & 63u) * 255u / 63u),
                     static_cast<std::uint32_t>(((value >> 11u) & 31u) * 255u / 31u));
    case TextureFormat::Rgba5551:
        return rgba8(static_cast<std::uint32_t>((value & 31u) * 255u / 31u),
                     static_cast<std::uint32_t>(((value >> 5u) & 31u) * 255u / 31u),
                     static_cast<std::uint32_t>(((value >> 10u) & 31u) * 255u / 31u),
                     ((value >> 15u) & 1u) != 0u ? 255u : 0u);
    case TextureFormat::Rgba4444:
        return rgba8(static_cast<std::uint32_t>((value & 15u) * 255u / 15u),
                     static_cast<std::uint32_t>(((value >> 4u) & 15u) * 255u / 15u),
                     static_cast<std::uint32_t>(((value >> 8u) & 15u) * 255u / 15u),
                     static_cast<std::uint32_t>(((value >> 12u) & 15u) * 255u / 15u));
    default:
        return 0u;
    }
}

} // namespace gungnir::ge

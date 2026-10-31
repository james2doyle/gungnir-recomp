// T7d contract tests: raw GE texture formats, swizzle, CLUT, sampling and cache keys.
//
// Every texture block and palette below is hand-authored for this fixture.

#include "ge/texture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir::ge;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

std::uint32_t channel(std::uint32_t color, int shift) {
    return (color >> shift) & 255u;
}

bool near_channel(std::uint32_t color, int shift, std::uint32_t expected) {
    return std::abs(static_cast<int>(channel(color, shift)) - static_cast<int>(expected)) <= 1;
}

void store_u16(std::vector<std::uint8_t> &data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    data[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void store_u32(std::vector<std::uint8_t> &data, std::size_t offset, std::uint32_t value) {
    store_u16(data, offset, static_cast<std::uint16_t>(value & 0xFFFFu));
    store_u16(data, offset + 2u, static_cast<std::uint16_t>(value >> 16u));
}

TextureState texture_state(TextureFormat format, int width, int height, int buffer_width) {
    TextureState state;
    state.format = format;
    state.width = width;
    state.height = height;
    state.buffer_width = buffer_width;
    return state;
}

void plain_formats() {
    std::vector<std::uint8_t> rgba(2u * 2u * 4u);
    store_u32(rgba, 0u, rgba8(255u, 0u, 0u));
    store_u32(rgba, 4u, rgba8(0u, 255u, 0u));
    store_u32(rgba, 8u, rgba8(0u, 0u, 255u));
    store_u32(rgba, 12u, rgba8(255u, 255u, 255u));
    const Texture direct(texture_state(TextureFormat::Rgba8888, 2, 2, 2), rgba);
    std::vector<std::uint32_t> pixels;
    direct.decode(pixels);
    check(pixels.size() == 4u && pixels[0] == rgba8(255u, 0u, 0u) && pixels[1] == rgba8(0u, 255u, 0u) &&
              pixels[2] == rgba8(0u, 0u, 255u) && pixels[3] == rgba8(255u, 255u, 255u),
          "8888 texels decode in row-major order");

    std::vector<std::uint8_t> rgb565(4u * 2u);
    store_u16(rgb565, 0u, 0x001Fu);
    store_u16(rgb565, 2u, 0x07E0u);
    store_u16(rgb565, 4u, 0xF800u);
    store_u16(rgb565, 6u, 0xFFFFu);
    const Texture rgb565_texture(texture_state(TextureFormat::Rgb565, 4, 1, 4), rgb565);
    rgb565_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 0u, 0u) && pixels[1] == rgba8(0u, 255u, 0u) &&
              pixels[2] == rgba8(0u, 0u, 255u) && pixels[3] == rgba8(255u, 255u, 255u),
          "5650 expands to 8-bit channels");

    std::vector<std::uint8_t> rgba5551(3u * 2u);
    store_u16(rgba5551, 0u, 0xFFFFu);
    store_u16(rgba5551, 2u, 0x0000u);
    store_u16(rgba5551, 4u, 0x801Fu);
    const Texture rgba5551_texture(texture_state(TextureFormat::Rgba5551, 3, 1, 3), rgba5551);
    rgba5551_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 255u, 255u, 255u) && pixels[1] == rgba8(0u, 0u, 0u, 0u) &&
              pixels[2] == rgba8(255u, 0u, 0u, 255u),
          "5551 keeps its one-bit alpha");

    std::vector<std::uint8_t> rgba4444(2u * 2u);
    store_u16(rgba4444, 0u, 0xFFFFu);
    store_u16(rgba4444, 2u, 0x00F0u);
    const Texture rgba4444_texture(texture_state(TextureFormat::Rgba4444, 2, 1, 2), rgba4444);
    rgba4444_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 255u, 255u, 255u) && pixels[1] == rgba8(0u, 255u, 0u, 0u),
          "4444 expands each nibble by 17");
}

void clut_lookup() {
    std::vector<std::uint8_t> palette(16u * 2u);
    store_u16(palette, 0u, 0xF800u);
    store_u16(palette, 2u, 0x001Fu);
    store_u16(palette, 4u, 0x07E0u);
    store_u16(palette, 6u, 0xFFFFu);
    const std::uint32_t clut_format = 0xFFu << 8u;

    std::vector<std::uint8_t> clut4{0x21u};
    TextureState state = texture_state(TextureFormat::Clut4, 2, 1, 2);
    state.clut_format = clut_format;
    const Texture clut4_texture(state, clut4, palette);
    std::vector<std::uint32_t> pixels;
    clut4_texture.decode(pixels);
    check(pixels.size() == 2u && pixels[0] == rgba8(255u, 0u, 0u) && pixels[1] == rgba8(0u, 255u, 0u),
          "CLUT4 reads low and high nibbles");

    std::vector<std::uint8_t> clut8{1u, 2u};
    state = texture_state(TextureFormat::Clut8, 2, 1, 2);
    state.clut_format = clut_format;
    const Texture clut8_texture(state, clut8, palette);
    clut8_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 0u, 0u) && pixels[1] == rgba8(0u, 255u, 0u), "CLUT8 reads one byte per texel");

    std::vector<std::uint8_t> clut16(2u * 2u);
    store_u16(clut16, 0u, 1u);
    store_u16(clut16, 2u, 2u);
    state = texture_state(TextureFormat::Clut16, 2, 1, 2);
    state.clut_format = clut_format;
    const Texture clut16_texture(state, clut16, palette);
    clut16_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 0u, 0u) && pixels[1] == rgba8(0u, 255u, 0u), "CLUT16 reads 16-bit indices");

    std::vector<std::uint8_t> clut32(2u * 4u);
    store_u32(clut32, 0u, 1u);
    store_u32(clut32, 4u, 2u);
    state = texture_state(TextureFormat::Clut32, 2, 1, 2);
    state.clut_format = clut_format;
    const Texture clut32_texture(state, clut32, palette);
    clut32_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 0u, 0u) && pixels[1] == rgba8(0u, 255u, 0u), "CLUT32 reads 32-bit indices");

    check(clut4_texture.palette_color(1u) == rgba8(255u, 0u, 0u), "palette lookup follows the CLUT format");
}

void swizzle_blocks() {
    std::vector<std::uint8_t> data(16u * 8u * 4u, 0u);
    const auto put = [&data](std::size_t offset, std::uint32_t color) { store_u32(data, offset, color); };
    put(0u, rgba8(255u, 0u, 0u));
    put(4u, rgba8(0u, 255u, 0u));
    put(128u, rgba8(0u, 0u, 255u));
    put(16u, rgba8(255u, 255u, 255u));
    put(508u, rgba8(255u, 255u, 0u));

    TextureState state = texture_state(TextureFormat::Rgba8888, 16, 8, 16);
    state.swizzled = true;
    const Texture texture(state, data);
    std::vector<std::uint32_t> pixels;
    texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 0u, 0u), "swizzle (0,0) is the first texel");
    check(pixels[1] == rgba8(0u, 255u, 0u), "swizzle (1,0) advances four bytes");
    check(pixels[4] == rgba8(0u, 0u, 255u), "swizzle (4,0) starts the next 16-byte block");
    check(pixels[16] == rgba8(255u, 255u, 255u), "swizzle (0,1) starts the next eight-row lane");
    check(pixels[15u + 7u * 16u] == rgba8(255u, 255u, 0u), "swizzle (15,7) lands in the last block");

    std::vector<std::uint8_t> nibbles(128u, 0u);
    nibbles[0] = 0x21u;
    nibbles[1] = 0x43u;
    nibbles[2] = 0x05u;
    nibbles[16] = 0x07u;
    nibbles[119] = 0x08u;
    std::vector<std::uint8_t> palette(16u * 2u);
    for (std::uint32_t i = 0; i < 8u; ++i) {
        const std::uint16_t colors[8] = {0xF800u, 0x001Fu, 0x07E0u, 0xFFFFu, 0xFFE0u, 0x07FFu, 0xF81Fu, 0x8410u};
        store_u16(palette, i * 2u, colors[i]);
    }
    TextureState nibble_state = texture_state(TextureFormat::Clut4, 16, 8, 16);
    nibble_state.swizzled = true;
    nibble_state.clut_format = 0xFFu << 8u;
    const Texture nibble_texture(nibble_state, nibbles, palette);
    nibble_texture.decode(pixels);
    check(pixels[0] == rgba8(255u, 0u, 0u), "CLUT4 swizzle low nibble at (0,0)");
    check(pixels[1] == rgba8(0u, 255u, 0u), "CLUT4 swizzle high nibble at (1,0)");
    check(pixels[2] == rgba8(255u, 255u, 255u), "CLUT4 swizzle low nibble at (2,0)");
    check(pixels[16] == rgba8(132u, 130u, 132u), "CLUT4 swizzle at (0,1)");
    check(pixels[15u + 7u * 16u] == rgba8(0u, 0u, 255u), "CLUT4 swizzle at (15,7)");
}

void buffer_width_and_sampling() {
    std::vector<std::uint8_t> data(7u * 2u * 4u, 0u);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 5; ++x)
            store_u32(data, static_cast<std::size_t>(y * 7 + x) * 4u,
                      rgba8(static_cast<std::uint32_t>(x * 40), static_cast<std::uint32_t>(y * 200), 0u));
    const Texture texture(texture_state(TextureFormat::Rgba8888, 5, 2, 7), data);
    std::vector<std::uint32_t> pixels;
    texture.decode(pixels);
    check(pixels[4] == rgba8(160u, 0u, 0u), "non-power-of-two bufw decodes the last column");
    check(pixels[9] == rgba8(160u, 200u, 0u), "bufw stride separates rows");

    std::vector<std::uint8_t> small(2u * 2u * 4u);
    store_u32(small, 0u, rgba8(255u, 0u, 0u));
    store_u32(small, 4u, rgba8(0u, 255u, 0u));
    store_u32(small, 8u, rgba8(0u, 0u, 255u));
    store_u32(small, 12u, rgba8(255u, 255u, 255u));
    TextureState nearest = texture_state(TextureFormat::Rgba8888, 2, 2, 2);
    nearest.clamp_u = nearest.clamp_v = true;
    const Texture nearest_texture(nearest, small);
    check(nearest_texture.sample(0.0f, 0.0f) == rgba8(255u, 0u, 0u), "nearest samples the top-left texel");
    check(nearest_texture.sample(1.0f, 1.0f) == rgba8(255u, 255u, 255u), "clamp pins the far edge");
    check(nearest_texture.sample(-0.2f, 0.0f) == rgba8(255u, 0u, 0u), "clamp pins the near edge");

    TextureState wrap = texture_state(TextureFormat::Rgba8888, 2, 2, 2);
    const Texture wrap_texture(wrap, small);
    check(wrap_texture.sample(1.0f, 0.0f) == rgba8(255u, 0u, 0u), "wrap turns u=1 back to zero");
    check(wrap_texture.sample(-0.1f, 0.0f) == rgba8(0u, 255u, 0u), "wrap turns u<0 onto the last texel");

    TextureState linear = texture_state(TextureFormat::Rgba8888, 2, 2, 2);
    linear.linear = true;
    const Texture linear_texture(linear, small);
    const std::uint32_t middle = linear_texture.sample(0.5f, 0.5f);
    check(near_channel(middle, 0, 128u) && near_channel(middle, 8, 128u) && near_channel(middle, 16, 128u) &&
              near_channel(middle, 24, 255u),
          "bilinear averages the four texel corners");
}

class CountingUploader : public TextureUploader {
public:
    void upload(std::uint64_t key, const TextureImage &image) override {
        ++uploads;
        last_key = key;
        last_pixels = image.pixels.size();
    }
    void invalidate(std::uint64_t key) override {
        ++invalidations;
        last_invalidated = key;
    }

    int uploads{};
    int invalidations{};
    std::uint64_t last_key{};
    std::size_t last_pixels{};
    std::uint64_t last_invalidated{};
};

void cache_keys() {
    std::vector<std::uint8_t> data(4u * 4u, 0u);
    store_u32(data, 0u, rgba8(255u, 0u, 0u));
    std::vector<std::uint8_t> palette(4u * 2u, 0u);
    store_u16(palette, 0u, 0x001Fu);
    store_u16(palette, 2u, 0xF800u);
    TextureState state = texture_state(TextureFormat::Clut4, 2, 2, 2);
    state.swizzled = false;
    state.clut_format = 0xFFu << 8u;
    const Texture texture(state, data, palette);
    const std::uint64_t key = texture.content_key();
    check(key == Texture(state, data, palette).content_key(), "content key is deterministic");

    CountingUploader uploader;
    TextureCache cache;
    const TextureImage &first = cache.get(key, texture, &uploader);
    check(first.width == 2 && first.height == 2 && uploader.uploads == 1, "first get decodes and uploads");
    const TextureImage &again = cache.get(key, texture, &uploader);
    check(&again == &first && uploader.uploads == 1, "cache hit does not upload again");

    palette[0] ^= 0x1Fu;
    const Texture with_new_palette(state, data, palette);
    const std::uint64_t palette_key = with_new_palette.content_key();
    check(palette_key != key, "CLUT bytes change the content key");
    const TextureImage &changed = cache.get(palette_key, with_new_palette, &uploader);
    check(&changed != &first && uploader.uploads == 2, "a changed CLUT decodes a new image");

    data[0] ^= 0xFFu;
    const Texture with_new_texels(state, data, palette);
    const std::uint64_t texel_key = with_new_texels.content_key();
    check(texel_key != palette_key, "texel bytes change the content key");

    TextureState linear = state;
    linear.linear = true;
    check(Texture(linear, data, palette).content_key() != texel_key, "filter state changes the content key");

    const std::size_t before = cache.size();
    cache.invalidate(palette_key, &uploader);
    check(cache.size() == before - 1u && cache.find(palette_key) == nullptr, "invalidate removes the cached image");
    check(uploader.invalidations == 1 && uploader.last_invalidated == palette_key,
          "invalidate notifies the backend upload seam");
}

} // namespace

int main() {
    try {
        plain_formats();
        clut_lookup();
        swizzle_blocks();
        buffer_width_and_sampling();
        cache_keys();
        std::cout << "ge_texture_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

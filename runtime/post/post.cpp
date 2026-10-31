#include "post/post.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace gungnir::post {
namespace {

std::uint32_t pack(std::uint32_t red, std::uint32_t green, std::uint32_t blue, std::uint32_t alpha) {
    return (red & 255u) | ((green & 255u) << 8u) | ((blue & 255u) << 16u) | ((alpha & 255u) << 24u);
}

std::uint32_t channel(std::uint32_t color, int shift) {
    return (color >> shift) & 255u;
}

std::uint32_t lerp_channel(std::uint32_t a, std::uint32_t b, float t) {
    const float value = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
}

std::uint32_t sample_bilinear(const Image &image, float x, float y) {
    const float clamped_x = std::clamp(x, 0.0f, static_cast<float>(image.width - 1));
    const float clamped_y = std::clamp(y, 0.0f, static_cast<float>(image.height - 1));
    const int x0 = static_cast<int>(std::floor(clamped_x));
    const int y0 = static_cast<int>(std::floor(clamped_y));
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float tx = clamped_x - static_cast<float>(x0);
    const float ty = clamped_y - static_cast<float>(y0);
    const auto at = [&image](int px, int py) {
        return image.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width) +
                            static_cast<std::size_t>(px)];
    };
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const std::uint32_t top = lerp_channel(channel(at(x0, y0), shift), channel(at(x1, y0), shift), tx);
        const std::uint32_t bottom = lerp_channel(channel(at(x0, y1), shift), channel(at(x1, y1), shift), tx);
        out |= lerp_channel(top, bottom, ty) << shift;
    }
    return out;
}

Image bilinear_scale(const Image &source, float scale) {
    Image out;
    out.width = std::max(1, static_cast<int>(std::lround(static_cast<float>(source.width) * scale)));
    out.height = std::max(1, static_cast<int>(std::lround(static_cast<float>(source.height) * scale)));
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(source.width) /
                                 static_cast<float>(out.width) -
                             0.5f;
            const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(source.height) /
                                 static_cast<float>(out.height) -
                             0.5f;
            out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                       static_cast<std::size_t>(x)] = sample_bilinear(source, sx, sy);
        }
    }
    return out;
}

std::uint32_t darken(std::uint32_t color, float strength) {
    const float factor = std::clamp(1.0f - strength, 0.0f, 1.0f);
    return pack(static_cast<std::uint32_t>(std::lround(channel(color, 0) * factor)),
                static_cast<std::uint32_t>(std::lround(channel(color, 8) * factor)),
                static_cast<std::uint32_t>(std::lround(channel(color, 16) * factor)), channel(color, 24));
}

bool get_int(json_object *object, const char *key, int &value) {
    json_object *field = nullptr;
    if (!json_object_object_get_ex(object, key, &field)) return false;
    value = json_object_get_int(field);
    return true;
}

bool get_double(json_object *object, const char *key, float &value) {
    json_object *field = nullptr;
    if (!json_object_object_get_ex(object, key, &field)) return false;
    value = static_cast<float>(json_object_get_double(field));
    return true;
}

LoadResult parse_error(const char *error) {
    LoadResult result;
    result.error = error;
    return result;
}

} // namespace

LoadResult load_preset(const std::string &json) {
    json_tokener *tokener = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tokener, json.c_str(), static_cast<int>(json.size()));
    const json_tokener_error parse_result = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (root == nullptr || parse_result != json_tokener_success) {
        if (root != nullptr) json_object_put(root);
        return parse_error("invalid_json");
    }
    if (!json_object_is_type(root, json_type_object)) {
        json_object_put(root);
        return parse_error("invalid_preset");
    }

    LoadResult result;
    json_object *name = nullptr;
    if (json_object_object_get_ex(root, "name", &name) && json_object_is_type(name, json_type_string))
        result.preset.name = json_object_get_string(name);
    json_object *passes = nullptr;
    if (!json_object_object_get_ex(root, "passes", &passes) || !json_object_is_type(passes, json_type_array)) {
        json_object_put(root);
        return parse_error("missing_passes");
    }
    const std::size_t count = json_object_array_length(passes);
    for (std::size_t i = 0; i < count; ++i) {
        json_object *entry = json_object_array_get_idx(passes, static_cast<int>(i));
        if (entry == nullptr || !json_object_is_type(entry, json_type_object)) {
            json_object_put(root);
            return parse_error("invalid_pass");
        }
        json_object *type = nullptr;
        if (!json_object_object_get_ex(entry, "type", &type) || !json_object_is_type(type, json_type_string)) {
            json_object_put(root);
            return parse_error("missing_pass_type");
        }
        const std::string type_name = json_object_get_string(type);
        Pass pass;
        if (type_name == "integer_scale") {
            pass.type = PassType::IntegerScale;
            if (!get_int(entry, "factor", pass.factor) || pass.factor < 1 || pass.factor > 8) {
                json_object_put(root);
                return parse_error("invalid_factor");
            }
        } else if (type_name == "bilinear") {
            pass.type = PassType::Bilinear;
            if (!get_double(entry, "scale", pass.scale) || pass.scale < 1.0f || pass.scale > 8.0f) {
                json_object_put(root);
                return parse_error("invalid_scale");
            }
        } else if (type_name == "fsr_lite") {
            pass.type = PassType::FsrLite;
            if (!get_double(entry, "scale", pass.scale) || pass.scale < 1.0f || pass.scale > 8.0f) {
                json_object_put(root);
                return parse_error("invalid_scale");
            }
            pass.sharpness = 0.5f;
            get_double(entry, "sharpness", pass.sharpness);
            pass.sharpness = std::clamp(pass.sharpness, 0.0f, 1.0f);
        } else if (type_name == "scanlines" || type_name == "lcd_grid") {
            pass.type = type_name == "scanlines" ? PassType::Scanlines : PassType::LcdGrid;
            if (!get_double(entry, "strength", pass.strength) || pass.strength < 0.0f || pass.strength > 1.0f) {
                json_object_put(root);
                return parse_error("invalid_strength");
            }
            pass.period = 2;
            get_int(entry, "period", pass.period);
            if (pass.period < 1 || pass.period > 16) {
                json_object_put(root);
                return parse_error("invalid_period");
            }
        } else {
            json_object_put(root);
            return parse_error("unknown_pass");
        }
        result.preset.passes.push_back(pass);
    }
    json_object_put(root);
    result.ok = true;
    return result;
}

LoadResult load_preset_file(const std::string &path) {
    std::ifstream file(path);
    if (!file) return parse_error("file_not_found");
    std::ostringstream contents;
    contents << file.rdbuf();
    return load_preset(contents.str());
}

Preset default_preset() {
    return Preset{};
}

Image apply(const Image &source, const Preset &preset) {
    Image out = source;
    for (const Pass &pass : preset.passes) {
        if (out.width <= 0 || out.height <= 0) break;
        switch (pass.type) {
        case PassType::IntegerScale: {
            if (pass.factor <= 1) break;
            Image scaled;
            scaled.width = out.width * pass.factor;
            scaled.height = out.height * pass.factor;
            scaled.pixels.resize(static_cast<std::size_t>(scaled.width) * static_cast<std::size_t>(scaled.height));
            for (int y = 0; y < out.height; ++y) {
                for (int x = 0; x < out.width; ++x) {
                    const std::uint32_t pixel =
                        out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                                   static_cast<std::size_t>(x)];
                    for (int dy = 0; dy < pass.factor; ++dy)
                        for (int dx = 0; dx < pass.factor; ++dx)
                            scaled.pixels[static_cast<std::size_t>(y * pass.factor + dy) *
                                              static_cast<std::size_t>(scaled.width) +
                                          static_cast<std::size_t>(x * pass.factor + dx)] = pixel;
                }
            }
            out = std::move(scaled);
            break;
        }
        case PassType::Bilinear:
            out = bilinear_scale(out, pass.scale);
            break;
        case PassType::FsrLite: {
            const Image base = bilinear_scale(out, pass.scale);
            out = base;
            for (int y = 0; y < out.height; ++y) {
                for (int x = 0; x < out.width; ++x) {
                    const auto at = [&base](int px, int py) {
                        return base.pixels[static_cast<std::size_t>(std::clamp(py, 0, base.height - 1)) *
                                                static_cast<std::size_t>(base.width) +
                                            static_cast<std::size_t>(std::clamp(px, 0, base.width - 1))];
                    };
                    const std::uint32_t center = at(x, y);
                    const std::uint32_t average =
                        pack((channel(at(x - 1, y), 0) + channel(at(x + 1, y), 0) + channel(at(x, y - 1), 0) +
                              channel(at(x, y + 1), 0)) /
                                 4,
                             (channel(at(x - 1, y), 8) + channel(at(x + 1, y), 8) + channel(at(x, y - 1), 8) +
                              channel(at(x, y + 1), 8)) /
                                 4,
                             (channel(at(x - 1, y), 16) + channel(at(x + 1, y), 16) + channel(at(x, y - 1), 16) +
                              channel(at(x, y + 1), 16)) /
                                 4,
                             channel(center, 24));
                    std::uint32_t sharpened = 0;
                    for (int shift = 0; shift < 24; shift += 8) {
                        const int value = static_cast<int>(std::lround(
                            static_cast<float>(channel(center, shift)) +
                            pass.sharpness *
                                (static_cast<float>(channel(center, shift)) - static_cast<float>(channel(average, shift)))));
                        sharpened |= static_cast<std::uint32_t>(std::clamp(value, 0, 255)) << shift;
                    }
                    out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                               static_cast<std::size_t>(x)] = pack(channel(sharpened, 0), channel(sharpened, 8),
                                                                   channel(sharpened, 16), channel(center, 24));
                }
            }
            break;
        }
        case PassType::Scanlines:
            for (int y = 0; y < out.height; ++y)
                if (y % pass.period == 0)
                    for (int x = 0; x < out.width; ++x) {
                        const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                                                  static_cast<std::size_t>(x);
                        out.pixels[index] = darken(out.pixels[index], pass.strength);
                    }
            break;
        case PassType::LcdGrid:
            for (int y = 0; y < out.height; ++y)
                for (int x = 0; x < out.width; ++x) {
                    if (x % pass.period != 0 && y % pass.period != 0) continue;
                    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                                              static_cast<std::size_t>(x);
                    out.pixels[index] = darken(out.pixels[index], pass.strength);
                }
            break;
        }
    }
    return out;
}

std::uint64_t hash(const Image &image) {
    std::uint64_t value = 14695981039346656037ull;
    const auto mix = [&value](std::uint8_t byte) {
        value ^= byte;
        value *= 1099511628211ull;
    };
    for (int shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(static_cast<std::uint32_t>(image.width) >> shift));
    for (int shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(static_cast<std::uint32_t>(image.height) >> shift));
    for (const std::uint32_t pixel : image.pixels)
        for (int shift = 0; shift < 32; shift += 8) mix(static_cast<std::uint8_t>(pixel >> shift));
    return value;
}

} // namespace gungnir::post

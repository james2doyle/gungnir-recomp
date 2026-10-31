#include "tex/dump.hpp"

#include "tex/png.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace gungnir::tex {
namespace {

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

std::string hex16(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::string trim(const std::string &text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool is_key(const std::string &text) {
    if (text.size() != 16) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::vector<std::uint8_t> pixels_to_bytes(const std::vector<std::uint32_t> &pixels) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(pixels.size() * 4u);
    for (const std::uint32_t pixel : pixels) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(pixel >> shift));
    }
    return bytes;
}

bool same_texture(const TplTexture &a, const TplTexture &b) {
    return a.state.format == b.state.format && a.state.swizzled == b.state.swizzled &&
           a.state.width == b.state.width && a.state.height == b.state.height &&
           a.state.buffer_width == b.state.buffer_width &&
           a.state.clut_format == b.state.clut_format &&
           a.palette_entries == b.palette_entries && a.palette_format == b.palette_format &&
           a.data == b.data && a.palette == b.palette;
}

bool update_manifest(const std::filesystem::path &path, const std::vector<DumpEntry> &entries,
                     std::string &error) {
    std::string existing;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "manifest_read_failed";
        return false;
    }
    if (exists) {
        std::ifstream input(path);
        if (!input) {
            error = "manifest_read_failed";
            return false;
        }
        existing.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    std::set<std::string> known;
    std::istringstream lines(existing);
    std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = trim(line.substr(0, equals));
        if (is_key(key)) {
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            known.insert(key);
        }
    }

    std::string append;
    if (!exists || existing.empty())
        append += "[options]\nversion = 1\nhash = gungnir64\n\n[hashes]\n";
    else if (existing.find("[hashes]") == std::string::npos)
        append += "\n[hashes]\n";

    for (const DumpEntry &entry : entries) {
        if (known.count(entry.key) != 0) continue;
        append += "# " + std::to_string(entry.width) + "x" + std::to_string(entry.height) + " " +
                  entry.format + "\n";
        append += entry.key + " = " + entry.filename + "\n";
    }
    if (append.empty()) return true;

    std::ofstream output(path, std::ios::app);
    if (!output) {
        error = "manifest_write_failed";
        return false;
    }
    output << append;
    if (!output) {
        error = "manifest_write_failed";
        return false;
    }
    return true;
}

} // namespace

std::string texture_key(const TplTexture &texture) {
    std::uint64_t hash = 14695981039346656037ull;
    constexpr char version[] = "gungnir-tex-v1";
    mix_span(hash, std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t *>(version), sizeof(version) - 1u));
    mix_u32(hash, static_cast<std::uint32_t>(texture.state.format));
    mix_byte(hash, texture.state.swizzled ? 1u : 0u);
    mix_u32(hash, static_cast<std::uint32_t>(texture.state.width));
    mix_u32(hash, static_cast<std::uint32_t>(texture.state.height));
    mix_u32(hash, static_cast<std::uint32_t>(texture.state.buffer_width));
    mix_u32(hash, texture.state.clut_format);
    mix_u32(hash, texture.palette_entries);
    mix_u32(hash, texture.palette_format);
    mix_span(hash, texture.data);
    mix_span(hash, texture.palette);
    return hex16(hash);
}

DumpResult dump_textures(std::span<const TplTexture> textures, const DumpOptions &options) {
    DumpResult result;
    if (textures.empty()) {
        result.error = "no_textures";
        return result;
    }
    if (options.root.empty()) {
        result.error = "invalid_output_directory";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.root / "new", ec);
    if (ec) {
        result.error = "output_directory_failed";
        return result;
    }

    std::vector<const TplTexture *> unique;
    std::map<std::string, std::size_t> by_key;
    for (const TplTexture &texture : textures) {
        const std::string key = texture_key(texture);
        const auto found = by_key.find(key);
        if (found != by_key.end()) {
            if (!same_texture(*unique[found->second], texture)) {
                result.error = "key_collision";
                return result;
            }
            continue;
        }
        by_key.emplace(key, unique.size());
        unique.push_back(&texture);
    }

    for (const TplTexture *texture : unique) {
        if (texture->state.width <= 0 || texture->state.height <= 0) {
            result.error = "invalid_texture";
            return result;
        }
        const std::string key = texture_key(*texture);
        DumpEntry entry;
        entry.key = key;
        entry.filename = "new/" + key + ".png";
        entry.width = texture->state.width;
        entry.height = texture->state.height;
        entry.format = ge::texture_format_name(texture->state.format);

        const std::filesystem::path path = options.root / entry.filename;
        const std::vector<std::uint8_t> bytes = pixels_to_bytes(decode_texture(*texture));
        if (std::filesystem::exists(path, ec) && !ec) {
            const PngReadResult existing = read_png(path);
            if (!existing.ok) {
                result.error = "existing_png_invalid";
                return result;
            }
            if (existing.image.width != entry.width || existing.image.height != entry.height ||
                existing.image.rgba != bytes) {
                result.error = "key_collision";
                return result;
            }
            ++result.skipped;
        } else {
            const PngWriteResult written = write_png(path, entry.width, entry.height, bytes);
            if (!written.ok) {
                result.error = written.error;
                return result;
            }
            ++result.written;
        }
        result.entries.push_back(std::move(entry));
    }

    std::string manifest_error;
    if (!update_manifest(options.root / "textures.ini", result.entries, manifest_error)) {
        result.error = manifest_error;
        return result;
    }
    result.ok = true;
    return result;
}

DumpResult dump_tpl(const Tpl &tpl, const DumpOptions &options) {
    if (!tpl.ok) {
        DumpResult result;
        result.error = "invalid_tpl";
        return result;
    }
    return dump_textures(tpl.textures, options);
}

} // namespace gungnir::tex

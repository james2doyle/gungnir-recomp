#include "tex/replace.hpp"

#include "tex/png.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <system_error>

namespace gungnir::tex {
namespace {

std::string trim(const std::string &text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool is_key(const std::string &text) {
    if (text.size() != 16) return false;
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string normalize_key(const std::string &text) {
    std::string key = trim(text);
    if (!is_key(key)) return {};
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return key;
}

std::filesystem::path locate(const std::filesystem::path &root, const std::string &key,
                             std::error_code &ec) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::filesystem::path directory = root / "replace";
    const std::array<std::string, 4> names = {key + ".png", lower + ".png", key + ".PNG",
                                              lower + ".PNG"};
    for (const std::string &name : names) {
        ec.clear();
        const std::filesystem::path candidate = directory / name;
        if (std::filesystem::exists(candidate, ec)) return candidate;
        if (ec) return {};
    }
    return {};
}

} // namespace

ge::TextureState ReplacementImage::state_for(const ge::TextureState &original) const {
    ge::TextureState state = original;
    state.format = ge::TextureFormat::Rgba8888;
    state.swizzled = false;
    state.width = width;
    state.height = height;
    state.buffer_width = width;
    state.clut_format = 0;
    return state;
}

TextureReplacements::TextureReplacements(std::filesystem::path root) : root_(std::move(root)) {}

void TextureReplacements::set_root(std::filesystem::path root) {
    root_ = std::move(root);
    cache_.clear();
}

ReplacementLookup TextureReplacements::find(const std::string &key) {
    ReplacementLookup result;
    if (root_.empty()) {
        result.error = "invalid_root";
        return result;
    }
    std::error_code root_error;
    if (!std::filesystem::is_directory(root_, root_error) || root_error) {
        result.error = "invalid_root";
        return result;
    }
    const std::string normalized = normalize_key(key);
    if (normalized.empty()) {
        result.error = "invalid_key";
        return result;
    }
    const auto cached = cache_.find(normalized);
    if (cached != cache_.end()) {
        result.image = &cached->second;
        result.ok = true;
        return result;
    }

    std::error_code ec;
    const std::filesystem::path file = locate(root_, normalized, ec);
    if (ec) {
        result.error = "file_access_failed";
        return result;
    }
    if (file.empty()) {
        result.error = "not_found";
        return result;
    }
    const PngReadResult png = read_png(file);
    if (!png.ok) {
        result.error = png.error;
        return result;
    }
    if (png.image.width <= 0 || png.image.height <= 0 ||
        png.image.rgba.size() != static_cast<std::size_t>(png.image.width) *
                                     static_cast<std::size_t>(png.image.height) * 4u) {
        result.error = "invalid_replacement";
        return result;
    }

    ReplacementImage image;
    image.width = png.image.width;
    image.height = png.image.height;
    image.rgba = png.image.rgba;
    const auto inserted = cache_.emplace(normalized, std::move(image));
    result.image = &inserted.first->second;
    result.ok = true;
    return result;
}

ReplacementScan TextureReplacements::scan(std::span<const std::string> known_keys) {
    ReplacementScan result;
    if (root_.empty()) {
        result.errors.push_back("invalid_root");
        return result;
    }
    std::error_code root_error;
    if (!std::filesystem::is_directory(root_, root_error) || root_error) {
        result.errors.push_back("invalid_root");
        return result;
    }

    std::set<std::string> known;
    for (const std::string &key : known_keys) {
        const std::string normalized = normalize_key(key);
        if (!normalized.empty()) known.insert(normalized);
    }

    const std::filesystem::path directory = root_ / "replace";
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || ec) {
        if (ec) result.errors.push_back("replace: " + ec.message());
        return result;
    }
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code file_error;
        if (!it->is_regular_file(file_error) || file_error) continue;
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".png") continue;
        ++result.present;
        const std::string name = it->path().filename().string();
        const std::string key = normalize_key(it->path().stem().string());
        if (key.empty() || known.count(key) == 0) {
            result.unmatched.push_back(name);
            continue;
        }
        const ReplacementLookup lookup = find(key);
        if (lookup.ok) {
            ++result.loaded;
        } else {
            result.errors.push_back(name + ": " + lookup.error);
        }
    }
    if (ec) result.errors.push_back("replace: " + ec.message());
    std::sort(result.unmatched.begin(), result.unmatched.end());
    std::sort(result.errors.begin(), result.errors.end());
    return result;
}

void TextureReplacements::clear() {
    cache_.clear();
}

} // namespace gungnir::tex

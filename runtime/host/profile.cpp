#include "host/profile.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <limits>

namespace gungnir::host {
namespace {

struct KeySpec {
    std::string_view name;
    ProfileValue::Kind kind;
};

// The recognised schema. Adding a per-title patch is an explicit, reviewed
// change here; an unrecognised key is a named error, never an ignored line.
constexpr std::array<KeySpec, 2> kSchema{{
    {"cam_projection", ProfileValue::Kind::kU32},
    {"cam_setup", ProfileValue::Kind::kU32},
}};
constexpr std::string_view kFloatKey = "widescreen_fov_scale";

const KeySpec *find_spec(std::string_view key) {
    for (const KeySpec &spec : kSchema) {
        if (spec.name == key) return &spec;
    }
    return nullptr;
}

bool is_float_key(std::string_view key) { return key == kFloatKey; }

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool parse_u32(std::string_view text, std::uint32_t &out) {
    if (text.empty()) return false;
    int base = 10;
    std::string_view digits = text;
    if (digits.size() > 2 && digits[0] == '0' &&
        (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    if (digits.empty()) return false;
    for (char c : digits) {
        const bool ok = base == 16
            ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F'))
            : (c >= '0' && c <= '9');
        if (!ok) return false;
    }
    const char *begin = digits.data();
    const char *end = begin + digits.size();
    std::uint64_t wide = 0;
    const auto result = std::from_chars(begin, end, wide, base);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    if (wide > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(wide);
    return true;
}

bool parse_float(std::string_view text, float &out) {
    if (text.empty()) return false;
    // std::from_chars(float) is not available on all toolchains; strtof with a
    // full-consumption check is equivalent here and refuses trailing junk.
    std::string owned(text);
    char *end = nullptr;
    errno = 0;
    const float value = std::strtof(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size() || errno == ERANGE) return false;
    out = value;
    return true;
}

}  // namespace

std::string ProfileValue::render() const {
    if (kind_ == Kind::kU32) {
        char buffer[16];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), u32_, 10);
        return std::string(buffer, result.ptr);
    }
    return std::to_string(float_);
}

void HostProfile::insert(std::string key, ProfileValue value) {
    entries_.emplace(std::move(key), value);
}

const ProfileValue *HostProfile::lookup(std::string_view key) const noexcept {
    const auto it = entries_.find(std::string(key));
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<std::string> HostProfile::applied_lines() const {
    std::vector<std::string> lines;
    lines.reserve(entries_.size());
    for (const auto &[key, value] : entries_) {
        lines.push_back("profile " + key + "=" + value.render());
    }
    // Stable, reviewable boot log independent of hash iteration order.
    std::sort(lines.begin(), lines.end());
    return lines;
}

ProfileResult HostProfile::parse(std::string_view text) {
    ProfileResult result;
    std::size_t line_number = 0;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t newline = text.find('\n', offset);
        const std::string_view raw = text.substr(
            offset, newline == std::string_view::npos ? std::string_view::npos
                                                      : newline - offset);
        ++line_number;
        offset = newline == std::string_view::npos ? text.size() + 1 : newline + 1;

        const std::size_t comment = raw.find('#');
        std::string_view line =
            trim(comment == std::string_view::npos ? raw : raw.substr(0, comment));
        if (line.empty()) continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            result.error = ProfileError::kMalformedValue;
            result.message = "line " + std::to_string(line_number) +
                ": expected 'key = value'";
            return result;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) {
            result.error = ProfileError::kMalformedValue;
            result.message = "line " + std::to_string(line_number) + ": empty " +
                std::string(key.empty() ? "key" : "value in '" + std::string(key) + "'");
            return result;
        }
        const std::string owned_key(key);
        if (result.profile.entries_.find(owned_key) != result.profile.entries_.end()) {
            result.error = ProfileError::kDuplicateKey;
            result.message = "duplicate key '" + owned_key + "'";
            return result;
        }

        if (const KeySpec *spec = find_spec(key)) {
            std::uint32_t parsed = 0;
            if (!parse_u32(value, parsed)) {
                // A float-looking value in an address slot is a type mismatch,
                // not a coercion.
                const bool looks_float = value.find('.') != std::string_view::npos;
                result.error = looks_float ? ProfileError::kTypeMismatch
                                           : ProfileError::kMalformedValue;
                result.message = "'" + owned_key + "' expects a u32 address, got '" +
                    std::string(value) + "'";
                return result;
            }
            result.profile.insert(owned_key, ProfileValue(parsed));
        } else if (is_float_key(key)) {
            float parsed = 0.0f;
            if (!parse_float(value, parsed)) {
                result.error = ProfileError::kMalformedValue;
                result.message = "'" + owned_key + "' expects a float, got '" +
                    std::string(value) + "'";
                return result;
            }
            result.profile.insert(owned_key, ProfileValue(parsed));
        } else {
            result.error = ProfileError::kUnknownKey;
            result.message = "unknown profile key '" + owned_key + "'";
            return result;
        }
    }
    return result;
}

}  // namespace gungnir::host

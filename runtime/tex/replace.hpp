#pragma once

// User PNG replacement lookup (T8d). Replacements are keyed by the same
// content key as dump mode and may be any resolution; the returned image is
// sampled through the GE texture path with the live clamp/filter state.

#include "ge/texture.hpp"
#include "tex/tpl.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace gungnir::tex {

struct ReplacementImage {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] ge::TextureState state_for(const ge::TextureState &original) const;
};

struct ReplacementLookup {
    const ReplacementImage *image{};
    std::string error;
    bool ok{false};
};

struct ReplacementScan {
    std::size_t present{};
    std::size_t loaded{};
    std::vector<std::string> unmatched;
    std::vector<std::string> errors;
};

class TextureReplacements {
public:
    TextureReplacements() = default;
    explicit TextureReplacements(std::filesystem::path root);

    void set_root(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path &root() const { return root_; }

    // The returned image is owned by this object and stays valid until clear(),
    // set_root() or destruction. A missing file is "not_found"; a corrupt PNG
    // is "png_read_failed" and never falls back silently.
    ReplacementLookup find(const std::string &key);

    // Reports every PNG under replace/: loaded known keys, unknown keys by
    // filename, and per-file errors. Directory order is sorted for determinism.
    ReplacementScan scan(std::span<const std::string> known_keys);

    void clear();

private:
    std::filesystem::path root_;
    std::map<std::string, ReplacementImage> cache_;
};

} // namespace gungnir::tex

#pragma once

// Hash-keyed texture dump (T8c). Content keys are stable across runs and
// independent of the live GE sampler state; user PNGs are never written here.

#include "tex/tpl.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gungnir::tex {

struct DumpOptions {
    std::filesystem::path root;
};

struct DumpEntry {
    std::string key;
    std::string filename;
    int width{};
    int height{};
    std::string format;
};

struct DumpResult {
    std::vector<DumpEntry> entries;
    std::size_t written{};
    std::size_t skipped{};
    std::string error;
    bool ok{false};
};

std::string texture_key(const TplTexture &texture);
DumpResult dump_textures(std::span<const TplTexture> textures, const DumpOptions &options);
DumpResult dump_tpl(const Tpl &tpl, const DumpOptions &options);

} // namespace gungnir::tex

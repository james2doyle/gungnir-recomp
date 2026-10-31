#include "input/touch_layout_store.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace gungnir::input {

TouchLayoutResult load_touch_layout(const std::filesystem::path &path, TouchOverlay &overlay) {
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (error) {
        return {TouchLayoutStatus::Corrupt, "touch_layout_unreadable"};
    }
    if (!present) {
        return {TouchLayoutStatus::Missing, "touch_layout_missing"};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {TouchLayoutStatus::Corrupt, "touch_layout_unreadable"};
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return {TouchLayoutStatus::Corrupt, "touch_layout_unreadable"};
    }
    switch (overlay.load_json(text)) {
    case TouchOverlay::LoadStatus::Ok:
        return {TouchLayoutStatus::Ok, {}};
    case TouchOverlay::LoadStatus::Older:
        return {TouchLayoutStatus::Older, "touch_layout_older"};
    case TouchOverlay::LoadStatus::Corrupt:
        break;
    }
    return {TouchLayoutStatus::Corrupt, "touch_layout_corrupt"};
}

TouchLayoutResult save_touch_layout(const std::filesystem::path &path, const TouchOverlay &overlay) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return {TouchLayoutStatus::WriteFailed, "touch_layout_open_failed"};
        }
        const std::string text = overlay.to_json();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return {TouchLayoutStatus::WriteFailed, "touch_layout_write_failed"};
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return {TouchLayoutStatus::WriteFailed, "touch_layout_rename_failed"};
    }
    return {TouchLayoutStatus::Ok, {}};
}

} // namespace gungnir::input

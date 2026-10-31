#include "platform/android/assets.hpp"

#include <cctype>

namespace gungnir::platform {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

} // namespace

bool version_stamp_matches(std::string_view stored, std::string_view expected) {
    return trim(stored) == trim(expected);
}

} // namespace gungnir::platform

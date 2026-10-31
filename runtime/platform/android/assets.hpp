#pragma once

#include <string_view>

namespace gungnir::platform {

bool version_stamp_matches(std::string_view stored, std::string_view expected);

} // namespace gungnir::platform

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gungnir::platform {

struct DriverLimits {
    std::uint64_t max_archive_bytes{512ull << 20};
    std::uint64_t max_entry_bytes{256ull << 20};
    std::uint64_t max_total_bytes{1ull << 30};
    std::uint32_t max_entries{4096};
    std::uint32_t max_name_length{255};
};

struct DriverPackage {
    std::string id;
    std::string display_name;
    std::string vendor{"unknown"};
    std::string driver_version{"unknown"};
    int min_api{0};
    std::string soname;
    std::filesystem::path staged_dir;
};

struct DriverInspectResult {
    bool ok{false};
    std::string error;
    DriverPackage package;
};

struct DriverScanError {
    std::filesystem::path path;
    std::string error;
};

struct DriverScanResult {
    std::vector<DriverPackage> packages;
    std::vector<DriverScanError> errors;
};

DriverInspectResult inspect_driver_zip(const std::filesystem::path &zip_path,
                                       const DriverLimits &limits = {});

DriverInspectResult install_driver_zip(const std::filesystem::path &zip_path,
                                       const std::filesystem::path &drivers_root,
                                       const DriverLimits &limits = {});

DriverScanResult scan_drop_folder(const std::filesystem::path &drop_dir,
                                  const DriverLimits &limits = {});

bool remove_driver(const std::filesystem::path &drivers_root,
                   const std::string &id, std::string &error);

} // namespace gungnir::platform

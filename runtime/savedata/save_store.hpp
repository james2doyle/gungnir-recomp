#pragma once

// Virtual memory-stick savedata: the PSP/SAVEDATA layout around the vendored
// MIT PARAM.SFO reader/writer. No KIRK encryption is implemented here.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gungnir::savedata {

struct SaveFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct SaveDirectory {
    std::string name;
    std::vector<SaveFile> files;
};

struct SfoFields {
    std::string title;
    std::string savedata_title;
    std::string detail;
    std::uint8_t parental_level{};
};

class SaveStore {
public:
    explicit SaveStore(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }

    [[nodiscard]] static bool valid_name(std::string_view name);

    [[nodiscard]] std::optional<SaveDirectory> read(std::string_view directory) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(
        std::string_view directory, std::string_view file) const;
    [[nodiscard]] std::vector<std::string> list(std::string_view prefix = {}) const;

    bool write(const SaveDirectory &save, std::string &error) const;
    bool write_file(std::string_view directory, std::string_view file,
        std::span<const std::uint8_t> bytes, std::string &error) const;

private:
    std::filesystem::path root_;
};

[[nodiscard]] std::vector<std::uint8_t> build_param_sfo(
    const SfoFields &fields, std::string_view directory, std::string_view data_file);

[[nodiscard]] std::optional<SfoFields> parse_sfo_fields(std::span<const std::uint8_t> bytes);

} // namespace gungnir::savedata

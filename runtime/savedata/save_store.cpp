#include "savedata/save_store.hpp"

#include "save_data/param_sfo.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace gungnir::savedata {
namespace {

constexpr std::size_t kMaxNameLength = 63u;
constexpr std::size_t kFileListCount = 99u;
constexpr std::size_t kFileListEntrySize = 32u;

bool write_atomic(const std::filesystem::path &path, std::span<const std::uint8_t> bytes, std::string &error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "savedata_write_open_failed: " + temporary.string();
            return false;
        }
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "savedata_write_failed: " + temporary.string();
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "savedata_rename_failed: " + rename_error.message();
        return false;
    }
    return true;
}

} // namespace

SaveStore::SaveStore(std::filesystem::path root) : root_(std::move(root)) {}

bool SaveStore::valid_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength) return false;
    if (name == "." || name == "..") return false;
    for (const char value : name) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20u || byte == 0x7Fu) return false;
        if (value == '/' || value == '\\') return false;
    }
    return true;
}

std::optional<SaveDirectory> SaveStore::read(std::string_view directory) const {
    if (!valid_name(directory)) return std::nullopt;
    const std::filesystem::path path = root_ / "PSP" / "SAVEDATA" / std::string(directory);
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) return std::nullopt;

    SaveDirectory save;
    save.name = std::string(directory);
    for (const auto &entry : std::filesystem::directory_iterator(path, error)) {
        if (error) return std::nullopt;
        if (entry.is_symlink(error)) return std::nullopt;
        if (error) return std::nullopt;
        const bool regular = entry.is_regular_file(error);
        if (error) return std::nullopt;
        if (!regular) continue;
        const std::string name = entry.path().filename().string();
        if (!valid_name(name)) return std::nullopt;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) return std::nullopt;
        SaveFile file;
        file.name = name;
        file.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) return std::nullopt;
        save.files.push_back(std::move(file));
    }
    std::sort(save.files.begin(), save.files.end(),
        [](const SaveFile &left, const SaveFile &right) { return left.name < right.name; });
    return save;
}

std::optional<std::vector<std::uint8_t>> SaveStore::read_file(
    std::string_view directory, std::string_view file) const {
    if (!valid_name(directory) || !valid_name(file)) return std::nullopt;
    const std::filesystem::path path = root_ / "PSP" / "SAVEDATA" / std::string(directory) / std::string(file);
    std::error_code error;
    if (std::filesystem::is_symlink(path, error)) return std::nullopt;
    if (error) return std::nullopt;
    if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

std::vector<std::string> SaveStore::list(std::string_view prefix) const {
    std::vector<std::string> names;
    std::error_code error;
    const std::filesystem::path path = root_ / "PSP" / "SAVEDATA";
    if (!std::filesystem::is_directory(path, error)) return names;
    for (const auto &entry : std::filesystem::directory_iterator(path, error)) {
        if (error) return names;
        const bool directory = entry.is_directory(error);
        if (error) return names;
        if (!directory) continue;
        const std::string name = entry.path().filename().string();
        if (!valid_name(name) || !name.starts_with(prefix)) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool SaveStore::write(const SaveDirectory &save, std::string &error) const {
    error.clear();
    if (!valid_name(save.name)) {
        error = "savedata_invalid_directory: " + save.name;
        return false;
    }
    for (const SaveFile &file : save.files) {
        if (!valid_name(file.name)) {
            error = "savedata_invalid_file: " + file.name;
            return false;
        }
    }
    for (std::size_t i = 0; i < save.files.size(); ++i)
        for (std::size_t j = i + 1; j < save.files.size(); ++j)
            if (save.files[i].name == save.files[j].name) {
                error = "savedata_duplicate_file: " + save.files[i].name;
                return false;
            }

    const std::filesystem::path path = root_ / "PSP" / "SAVEDATA" / save.name;
    std::error_code directory_error;
    std::filesystem::create_directories(path, directory_error);
    if (directory_error) {
        error = "savedata_mkdir_failed: " + directory_error.message();
        return false;
    }
    for (const SaveFile &file : save.files) {
        if (!write_atomic(path / file.name, file.bytes, error)) return false;
    }
    return true;
}

bool SaveStore::write_file(std::string_view directory, std::string_view file,
    std::span<const std::uint8_t> bytes, std::string &error) const {
    error.clear();
    if (!valid_name(directory)) {
        error = "savedata_invalid_directory: " + std::string(directory);
        return false;
    }
    if (!valid_name(file)) {
        error = "savedata_invalid_file: " + std::string(file);
        return false;
    }
    const std::filesystem::path path = root_ / "PSP" / "SAVEDATA" / std::string(directory);
    std::error_code directory_error;
    std::filesystem::create_directories(path, directory_error);
    if (directory_error) {
        error = "savedata_mkdir_failed: " + directory_error.message();
        return false;
    }
    return write_atomic(path / std::string(file), bytes, error);
}

std::vector<std::uint8_t> build_param_sfo(
    const SfoFields &fields, std::string_view directory, std::string_view data_file) {
    mhp3rd::savedata::ParamSfo sfo;
    sfo.set_string("TITLE", fields.title, 128u);
    sfo.set_string("SAVEDATA_TITLE", fields.savedata_title, 128u);
    sfo.set_string("SAVEDATA_DETAIL", fields.detail, 1024u);
    sfo.set_integer("PARENTAL_LEVEL", fields.parental_level);
    sfo.set_string("CATEGORY", "MS", 4u);
    sfo.set_string("SAVEDATA_DIRECTORY", std::string(directory), 64u);

    std::vector<std::uint8_t> file_list(kFileListCount * kFileListEntrySize, 0u);
    if (!data_file.empty()) {
        const std::size_t length = std::min<std::size_t>(data_file.size(), 12u);
        std::memcpy(file_list.data(), data_file.data(), length);
    }
    sfo.set_binary("SAVEDATA_FILE_LIST", std::move(file_list),
        static_cast<std::uint32_t>(kFileListCount * kFileListEntrySize));
    sfo.set_binary("SAVEDATA_PARAMS", std::vector<std::uint8_t>(128u, 0u), 128u);
    return sfo.serialize();
}

std::optional<SfoFields> parse_sfo_fields(std::span<const std::uint8_t> bytes) {
    const auto sfo = mhp3rd::savedata::ParamSfo::parse(bytes);
    if (!sfo) return std::nullopt;
    SfoFields fields;
    fields.title = sfo->string("TITLE").value_or("");
    fields.savedata_title = sfo->string("SAVEDATA_TITLE").value_or("");
    fields.detail = sfo->string("SAVEDATA_DETAIL").value_or("");
    fields.parental_level = static_cast<std::uint8_t>(sfo->integer("PARENTAL_LEVEL").value_or(0u));
    return fields;
}

} // namespace gungnir::savedata

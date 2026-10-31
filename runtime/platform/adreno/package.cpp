#include "platform/adreno/package.hpp"

#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#include <miniz.h>
#include <nlohmann/json.hpp>

namespace gungnir::platform {
namespace {

using json = nlohmann::json;

constexpr const char *kMetaName = "meta.json";
constexpr const char *kPlainDriverName = "libvulkan.so";

DriverInspectResult failure(std::string error) {
    DriverInspectResult result;
    result.error = std::move(error);
    return result;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_valid_id(std::string_view id) {
    if (id.size() != 16) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool path_is_safe(std::string_view name, const DriverLimits &limits, std::string &error) {
    if (name.empty() || name.size() > limits.max_name_length) {
        error = "driver_entry_name";
        return false;
    }
    if (name.front() == '/' || name.front() == '\\' ||
        name.find('\\') != std::string_view::npos) {
        error = "driver_zip_slip";
        return false;
    }
    for (char c : name) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || c == '\0') {
            error = "driver_entry_name";
            return false;
        }
    }
    const bool directory = name.back() == '/';
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::string_view part =
            name.substr(start, slash == std::string_view::npos ? name.size() - start
                                                               : slash - start);
        if (part.empty()) {
            const bool trailing_directory = directory && slash == std::string_view::npos;
            if (!trailing_directory) {
                error = "driver_entry_name";
                return false;
            }
        } else if (part == "..") {
            error = "driver_zip_slip";
            return false;
        } else if (part == ".") {
            error = "driver_entry_name";
            return false;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool valid_soname(std::string_view soname, const DriverLimits &limits) {
    return !soname.empty() && soname.size() <= limits.max_name_length &&
           soname.find('/') == std::string_view::npos &&
           soname.find('\\') == std::string_view::npos && soname != "." && soname != ".." &&
           ends_with(soname, ".so");
}

struct OpenArchive {
    std::vector<std::uint8_t> bytes;
    mz_zip_archive zip{};
    bool open{false};

    ~OpenArchive() {
        if (open) mz_zip_reader_end(&zip);
    }
};

bool open_archive(const std::filesystem::path &path, const DriverLimits &limits,
                  OpenArchive &archive, std::string &error) {
    std::error_code fs_error;
    if (path.empty() || !std::filesystem::is_regular_file(path, fs_error)) {
        error = "driver_zip_missing";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, fs_error);
    if (fs_error) {
        error = "driver_zip_missing";
        return false;
    }
    if (size > limits.max_archive_bytes ||
        size > std::numeric_limits<std::size_t>::max()) {
        error = "driver_entry_oversize";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "driver_zip_missing";
        return false;
    }
    archive.bytes.resize(static_cast<std::size_t>(size));
    if (size != 0) {
        input.read(reinterpret_cast<char *>(archive.bytes.data()),
                   static_cast<std::streamsize>(archive.bytes.size()));
        if (!input) {
            error = "driver_zip_missing";
            return false;
        }
    }

    mz_zip_zero_struct(&archive.zip);
    if (!mz_zip_reader_init_mem(&archive.zip, archive.bytes.data(), archive.bytes.size(), 0)) {
        error = "driver_zip_unsupported";
        return false;
    }
    archive.open = true;
    return true;
}

bool entry_name(OpenArchive &archive, mz_uint index, std::string &name) {
    const mz_uint needed = mz_zip_reader_get_filename(&archive.zip, index, nullptr, 0);
    if (needed == 0) return false;
    std::vector<char> buffer(needed, '\0');
    const mz_uint written =
        mz_zip_reader_get_filename(&archive.zip, index, buffer.data(), needed);
    if (written == 0) return false;
    name.assign(buffer.data(), written - 1);
    return true;
}

bool extract_entry(OpenArchive &archive, mz_uint index, std::uint64_t size,
                   const DriverLimits &limits, std::vector<std::uint8_t> &out,
                   std::string &error) {
    if (size > limits.max_entry_bytes ||
        size > std::numeric_limits<std::size_t>::max()) {
        error = "driver_entry_oversize";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (size == 0) return true;
    if (!mz_zip_reader_extract_to_mem(&archive.zip, index, out.data(), out.size(), 0)) {
        error = "driver_zip_unsupported";
        return false;
    }
    return true;
}

std::string validate_elf(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(Elf64_Ehdr)) return "driver_elf_truncated";

    Elf64_Ehdr header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB) {
        return "driver_elf_class";
    }
    if (header.e_type != ET_DYN) return "driver_elf_type";
    if (header.e_machine != EM_AARCH64) return "driver_elf_machine";
    if (header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shnum == 0 ||
        header.e_shoff == 0) {
        return "driver_elf_truncated";
    }
    if (header.e_shoff > bytes.size() ||
        header.e_shnum > (bytes.size() - header.e_shoff) / sizeof(Elf64_Shdr) ||
        header.e_shstrndx >= header.e_shnum) {
        return "driver_elf_truncated";
    }

    auto section_at = [&](std::size_t index, Elf64_Shdr &section) {
        std::memcpy(&section, bytes.data() + header.e_shoff + index * sizeof(Elf64_Shdr),
                    sizeof(section));
    };

    Elf64_Shdr strings{};
    section_at(header.e_shstrndx, strings);
    if (strings.sh_offset > bytes.size() ||
        strings.sh_size > bytes.size() - strings.sh_offset) {
        return "driver_elf_truncated";
    }
    const char *string_table =
        reinterpret_cast<const char *>(bytes.data() + strings.sh_offset);

    for (std::size_t index = 0; index < header.e_shnum; ++index) {
        Elf64_Shdr section{};
        section_at(index, section);
        if (section.sh_type != SHT_DYNSYM) continue;
        if (section.sh_name >= strings.sh_size) return "driver_elf_truncated";
        const char *name = string_table + section.sh_name;
        const std::size_t remaining = strings.sh_size - section.sh_name;
        if (std::memchr(name, '\0', remaining) == nullptr) {
            return "driver_elf_truncated";
        }
        if (std::strcmp(name, ".dynsym") != 0) continue;
        if (section.sh_size < sizeof(Elf64_Sym)) return "driver_elf_dynsym";
        if (section.sh_offset > bytes.size() ||
            section.sh_size > bytes.size() - section.sh_offset) {
            return "driver_elf_dynsym";
        }
        return {};
    }
    return "driver_elf_dynsym";
}

struct FileEntry {
    mz_uint index{0};
    std::string name;
    std::uint64_t size{0};
};

struct Prepared {
    DriverPackage package;
    std::vector<FileEntry> files;
};

bool prepare(OpenArchive &archive, const std::filesystem::path &source,
             const DriverLimits &limits, Prepared &prepared, std::string &error) {
    const mz_uint count = mz_zip_reader_get_num_files(&archive.zip);
    if (count == 0) {
        error = "driver_library_missing";
        return false;
    }
    if (count > limits.max_entries) {
        error = "driver_entry_oversize";
        return false;
    }

    std::set<std::string> names;
    std::vector<FileEntry> root_libraries;
    std::uint64_t total = 0;
    bool meta_found = false;
    FileEntry meta;

    for (mz_uint index = 0; index < count; ++index) {
        std::string name;
        if (!entry_name(archive, index, name)) {
            error = "driver_zip_unsupported";
            return false;
        }
        if (!path_is_safe(name, limits, error)) return false;
        if (!names.insert(name).second) {
            error = "driver_entry_duplicate";
            return false;
        }

        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive.zip, index, &stat)) {
            error = "driver_zip_unsupported";
            return false;
        }
        if (stat.m_is_encrypted) {
            error = "driver_entry_encrypted";
            return false;
        }
        if (!stat.m_is_supported) {
            error = "driver_zip_unsupported";
            return false;
        }
        if (stat.m_is_directory) continue;

        if (stat.m_uncomp_size > limits.max_entry_bytes) {
            error = "driver_entry_oversize";
            return false;
        }
        if (total > limits.max_total_bytes ||
            stat.m_uncomp_size > limits.max_total_bytes - total) {
            error = "driver_entry_oversize";
            return false;
        }
        total += stat.m_uncomp_size;

        FileEntry entry{index, name, stat.m_uncomp_size};
        prepared.files.push_back(entry);
        if (name.find('/') == std::string::npos && ends_with(name, ".so")) {
            root_libraries.push_back(entry);
        }
        if (name == kMetaName) {
            meta_found = true;
            meta = entry;
        }
    }

    const std::string digest =
        psprecomp::sha256_bytes(std::span<const std::uint8_t>(archive.bytes));
    prepared.package.id = digest.substr(0, 16);
    prepared.package.staged_dir.clear();

    if (meta_found) {
        std::vector<std::uint8_t> meta_bytes;
        if (!extract_entry(archive, meta.index, meta.size, limits, meta_bytes, error)) {
            return false;
        }
        const json document =
            json::parse(meta_bytes.begin(), meta_bytes.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            error = "driver_meta_invalid";
            return false;
        }
        try {
            const auto schema = document.find("schemaVersion");
            if (schema == document.end() || !schema->is_number_integer() ||
                schema->get<long long>() != 1) {
                error = "driver_meta_schema";
                return false;
            }
            const auto display_name = document.find("name");
            if (display_name == document.end() || !display_name->is_string()) {
                error = "driver_meta_name";
                return false;
            }
            prepared.package.display_name = display_name->get<std::string>();
            if (prepared.package.display_name.empty() ||
                prepared.package.display_name.size() > limits.max_name_length) {
                error = "driver_meta_name";
                return false;
            }

            const auto library = document.find("libraryName");
            if (library == document.end() || !library->is_string()) {
                error = "driver_library_missing";
                return false;
            }
            const std::string soname = library->get<std::string>();
            if (!valid_soname(soname, limits)) {
                error = "driver_library_missing";
                return false;
            }
            const auto named = std::find_if(
                root_libraries.begin(), root_libraries.end(),
                [&](const FileEntry &entry) { return entry.name == soname; });
            if (named == root_libraries.end()) {
                error = "driver_library_missing";
                return false;
            }
            prepared.package.soname = soname;

            const auto min_api = document.find("minApi");
            if (min_api != document.end()) {
                if (!min_api->is_number_integer()) {
                    error = "driver_meta_invalid";
                    return false;
                }
                const long long value = min_api->get<long long>();
                if (value < 0 || value > 1000) {
                    error = "driver_meta_invalid";
                    return false;
                }
                prepared.package.min_api = static_cast<int>(value);
            }

            for (const char *field : {"vendor", "driverVersion"}) {
                const auto found = document.find(field);
                if (found == document.end()) continue;
                if (!found->is_string()) {
                    error = "driver_meta_invalid";
                    return false;
                }
                std::string value = found->get<std::string>();
                if (value.empty()) value = "unknown";
                if (std::strcmp(field, "vendor") == 0) {
                    prepared.package.vendor = std::move(value);
                } else {
                    prepared.package.driver_version = std::move(value);
                }
            }
        } catch (const json::exception &) {
            error = "driver_meta_invalid";
            return false;
        }
    } else {
        if (root_libraries.empty()) {
            error = "driver_library_missing";
            return false;
        }
        if (root_libraries.size() != 1) {
            error = "driver_library_ambiguous";
            return false;
        }
        if (root_libraries.front().name != kPlainDriverName) {
            error = "driver_library_missing";
            return false;
        }
        prepared.package.soname = kPlainDriverName;
        prepared.package.display_name = source.stem().string();
        if (prepared.package.display_name.empty()) prepared.package.display_name = "unknown";
    }

    const auto driver = std::find_if(
        root_libraries.begin(), root_libraries.end(),
        [&](const FileEntry &entry) { return entry.name == prepared.package.soname; });
    if (driver == root_libraries.end()) {
        error = "driver_library_missing";
        return false;
    }
    std::vector<std::uint8_t> elf;
    if (!extract_entry(archive, driver->index, driver->size, limits, elf, error)) {
        return false;
    }
    error = validate_elf(std::span<const std::uint8_t>(elf));
    return error.empty();
}

} // namespace

DriverInspectResult inspect_driver_zip(const std::filesystem::path &zip_path,
                                       const DriverLimits &limits) {
    OpenArchive archive;
    std::string error;
    if (!open_archive(zip_path, limits, archive, error)) return failure(std::move(error));

    Prepared prepared;
    if (!prepare(archive, zip_path, limits, prepared, error)) {
        return failure(std::move(error));
    }
    DriverInspectResult result;
    result.ok = true;
    result.package = std::move(prepared.package);
    return result;
}

DriverInspectResult install_driver_zip(const std::filesystem::path &zip_path,
                                       const std::filesystem::path &drivers_root,
                                       const DriverLimits &limits) {
    OpenArchive archive;
    std::string error;
    if (!open_archive(zip_path, limits, archive, error)) return failure(std::move(error));

    Prepared prepared;
    if (!prepare(archive, zip_path, limits, prepared, error)) {
        return failure(std::move(error));
    }

    std::error_code fs_error;
    std::filesystem::create_directories(drivers_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(drivers_root, fs_error)) {
        return failure("driver_storage_unavailable");
    }

    const std::filesystem::path target = drivers_root / prepared.package.id;
    if (std::filesystem::exists(target, fs_error)) {
        if (fs_error || !std::filesystem::is_directory(target, fs_error)) {
            return failure("driver_storage_unavailable");
        }
        prepared.package.staged_dir = target;
        DriverInspectResult result;
        result.ok = true;
        result.package = std::move(prepared.package);
        return result;
    }

    const std::filesystem::path staging =
        drivers_root / (".staging-" + prepared.package.id);
    std::filesystem::remove_all(staging, fs_error);
    fs_error.clear();
    std::filesystem::create_directories(staging, fs_error);
    if (fs_error) return failure("driver_storage_unavailable");

    for (const FileEntry &entry : prepared.files) {
        std::vector<std::uint8_t> bytes;
        if (!extract_entry(archive, entry.index, entry.size, limits, bytes, error)) {
            std::filesystem::remove_all(staging, fs_error);
            return failure(std::move(error));
        }
        const std::filesystem::path destination = staging / entry.name;
        std::filesystem::create_directories(destination.parent_path(), fs_error);
        if (fs_error) {
            std::filesystem::remove_all(staging, fs_error);
            return failure("driver_storage_unavailable");
        }
        std::ofstream output(destination, std::ios::binary);
        if (!output) {
            std::filesystem::remove_all(staging, fs_error);
            return failure("driver_storage_unavailable");
        }
        if (!bytes.empty()) {
            output.write(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        if (!output) {
            std::filesystem::remove_all(staging, fs_error);
            return failure("driver_storage_unavailable");
        }
    }

    std::filesystem::rename(staging, target, fs_error);
    if (fs_error) {
        if (std::filesystem::exists(target, fs_error)) {
            std::filesystem::remove_all(staging, fs_error);
        } else {
            std::filesystem::remove_all(staging, fs_error);
            return failure("driver_storage_unavailable");
        }
    }

    prepared.package.staged_dir = target;
    DriverInspectResult result;
    result.ok = true;
    result.package = std::move(prepared.package);
    return result;
}

DriverScanResult scan_drop_folder(const std::filesystem::path &drop_dir,
                                  const DriverLimits &limits) {
    DriverScanResult result;
    std::error_code fs_error;
    if (!std::filesystem::is_directory(drop_dir, fs_error)) return result;

    std::vector<std::filesystem::path> candidates;
    std::filesystem::directory_iterator iterator(
        drop_dir, std::filesystem::directory_options::skip_permission_denied, fs_error);
    const std::filesystem::directory_iterator end;
    for (; !fs_error && iterator != end; iterator.increment(fs_error)) {
        std::error_code entry_error;
        if (!iterator->is_regular_file(entry_error) || entry_error) continue;
        const std::string extension = iterator->path().extension().string();
        std::string lowered = extension;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == ".zip") candidates.push_back(iterator->path());
    }
    if (fs_error) {
        result.errors.push_back({drop_dir, "driver_storage_unavailable"});
        return result;
    }

    std::sort(candidates.begin(), candidates.end());
    for (const auto &candidate : candidates) {
        const DriverInspectResult inspected = inspect_driver_zip(candidate, limits);
        if (inspected.ok) {
            result.packages.push_back(inspected.package);
        } else {
            result.errors.push_back({candidate, inspected.error});
        }
    }
    return result;
}

bool remove_driver(const std::filesystem::path &drivers_root,
                   const std::string &id, std::string &error) {
    if (!is_valid_id(id)) {
        error = "driver_id_invalid";
        return false;
    }
    std::error_code fs_error;
    const std::filesystem::path target = drivers_root / id;
    if (!std::filesystem::exists(target, fs_error)) {
        if (fs_error) error = "driver_storage_unavailable";
        return !fs_error;
    }
    std::filesystem::remove_all(target, fs_error);
    if (fs_error) {
        error = "driver_storage_unavailable";
        return false;
    }
    return true;
}

} // namespace gungnir::platform

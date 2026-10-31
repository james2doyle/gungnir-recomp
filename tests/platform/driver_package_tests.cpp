// T10b contract tests: untrusted driver zip inspection and atomic install.
// Every archive and ELF below is synthesized here; no driver binary is used.

#include "platform/adreno/package.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
using namespace gungnir::platform;

void check(bool condition, const std::string &contract) {
    if (!condition) throw std::runtime_error(contract);
}

void check_equal(const std::string &actual, const std::string &expected,
                 const std::string &contract) {
    check(actual == expected, contract + ": expected " + expected + ", got " + actual);
}

fs::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path =
        fs::temp_directory_path() / ("gungnir_driver_package_" + std::to_string(stamp));
    std::error_code error;
    fs::remove_all(path, error);
    fs::create_directories(path);
    return path;
}

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "fixture file opens: " + path.string());
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    check(static_cast<bool>(input), "fixture file reads: " + path.string());
    return bytes;
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    check(static_cast<bool>(output), "fixture file opens for writing: " + path.string());
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    check(static_cast<bool>(output), "fixture file writes: " + path.string());
}

struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool deflate{true};
};

void write_zip(const fs::path &path, const std::vector<ZipEntry> &entries) {
    mz_zip_archive archive{};
    mz_zip_zero_struct(&archive);
    check(mz_zip_writer_init_file(&archive, path.string().c_str(), 0) != MZ_FALSE,
          "zip writer initializes");
    for (const ZipEntry &entry : entries) {
        const mz_uint level = entry.deflate ? MZ_BEST_COMPRESSION : MZ_NO_COMPRESSION;
        const mz_bool added = mz_zip_writer_add_mem(
            &archive, entry.name.c_str(), entry.bytes.data(), entry.bytes.size(), level);
        check(added != MZ_FALSE, "zip entry is added: " + entry.name);
    }
    check(mz_zip_writer_finalize_archive(&archive) != MZ_FALSE, "zip finalizes");
    check(mz_zip_writer_end(&archive) != MZ_FALSE, "zip writer ends");
}

std::vector<std::uint8_t> make_elf(std::uint8_t elf_class = ELFCLASS64,
                                   std::uint16_t machine = EM_AARCH64,
                                   std::uint16_t type = ET_DYN,
                                   std::uint32_t dynsym_size = sizeof(Elf64_Sym)) {
    constexpr std::size_t kDynsymOffset = 0x100;
    constexpr std::size_t kStringsOffset = 0x200;
    constexpr std::size_t kSectionsOffset = 0x300;
    std::vector<std::uint8_t> bytes(0x400, 0);

    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = elf_class;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = type;
    header.e_machine = machine;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(Elf64_Ehdr);
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = 3;
    header.e_shstrndx = 2;
    header.e_shoff = kSectionsOffset;
    std::memcpy(bytes.data(), &header, sizeof(header));

    const std::string strings("\0.dynsym\0.shstrtab\0", 20);
    std::memcpy(bytes.data() + kStringsOffset, strings.data(), strings.size());

    Elf64_Shdr sections[3]{};
    sections[1].sh_name = 1;
    sections[1].sh_type = SHT_DYNSYM;
    sections[1].sh_offset = kDynsymOffset;
    sections[1].sh_size = dynsym_size;
    sections[1].sh_entsize = sizeof(Elf64_Sym);
    sections[1].sh_addralign = 8;
    sections[2].sh_name = 9;
    sections[2].sh_type = SHT_STRTAB;
    sections[2].sh_offset = kStringsOffset;
    sections[2].sh_size = strings.size();
    sections[2].sh_addralign = 1;
    std::memcpy(bytes.data() + kSectionsOffset, sections, sizeof(sections));
    return bytes;
}

std::string meta_json(const std::string &library = "vulkan.ad07XX.so",
                      int schema = 1, const std::string &name = "Synthetic Turnip") {
    nlohmann::json meta;
    meta["schemaVersion"] = schema;
    meta["name"] = name;
    meta["description"] = "synthetic test package";
    meta["author"] = "gungnir tests";
    meta["packageVersion"] = "1";
    meta["vendor"] = "Mesa";
    meta["driverVersion"] = "Vulkan 1.3.999";
    meta["minApi"] = 28;
    meta["libraryName"] = library;
    return meta.dump();
}

std::vector<std::uint8_t> bytes_of(const std::string &text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void patch_headers(const fs::path &path, bool encrypted, bool unsupported) {
    std::vector<std::uint8_t> bytes = read_file(path);
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); ++offset) {
        std::uint32_t signature = 0;
        std::memcpy(&signature, bytes.data() + offset, sizeof(signature));
        if (signature == 0x04034b50) {
            if (offset + 10 > bytes.size()) continue;
            if (encrypted) bytes[offset + 6] |= 0x01;
            if (unsupported) bytes[offset + 8] = 99;
        } else if (signature == 0x02014b50) {
            if (offset + 12 > bytes.size()) continue;
            if (encrypted) bytes[offset + 8] |= 0x01;
            if (unsupported) bytes[offset + 10] = 99;
        }
    }
    write_file(path, bytes);
}

void patch_name(const fs::path &path, const std::string &from, const std::string &to) {
    check(from.size() == to.size(), "patched names keep their length");
    std::vector<std::uint8_t> bytes = read_file(path);
    std::size_t replaced = 0;
    for (std::size_t offset = 0; offset + from.size() <= bytes.size(); ++offset) {
        if (std::memcmp(bytes.data() + offset, from.data(), from.size()) == 0) {
            std::memcpy(bytes.data() + offset, to.data(), to.size());
            ++replaced;
        }
    }
    check(replaced >= 2, "patched names appear in local and central headers");
    write_file(path, bytes);
}

void expect_error(const fs::path &path, const std::string &expected,
                  const DriverLimits &limits = {}) {
    const DriverInspectResult result = inspect_driver_zip(path, limits);
    check(!result.ok, "expected rejection: " + expected);
    check_equal(result.error, expected, "named rejection");
}

DriverPackage expect_ok(const fs::path &path, const DriverLimits &limits = {}) {
    const DriverInspectResult result = inspect_driver_zip(path, limits);
    check(result.ok, "expected acceptance, got " + result.error);
    return result.package;
}

void valid_meta_package() {
    const fs::path directory = make_temp_dir();
    const fs::path zip = directory / "turnip.zip";
    write_zip(zip, {
        {"meta.json", bytes_of(meta_json()), true},
        {"vulkan.ad07XX.so", make_elf(), false},
        {"driver.cfg", bytes_of("config"), true},
    });

    const DriverPackage package = expect_ok(zip);
    check(package.id.size() == 16, "id is 16 hex characters");
    check(std::all_of(package.id.begin(), package.id.end(), [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          }),
          "id is lowercase hex");
    check_equal(package.display_name, "Synthetic Turnip", "display name");
    check_equal(package.vendor, "Mesa", "vendor");
    check_equal(package.driver_version, "Vulkan 1.3.999", "driver version");
    check(package.min_api == 28, "minApi is recorded");
    check_equal(package.soname, "vulkan.ad07XX.so", "soname");
    check(package.staged_dir.empty(), "inspect does not stage");

    const fs::path root = directory / "drivers";
    const DriverInspectResult installed = install_driver_zip(zip, root);
    check(installed.ok, "install succeeds: " + installed.error);
    check(installed.package.staged_dir == root / package.id, "install uses the content id");
    check(fs::is_regular_file(installed.package.staged_dir / "vulkan.ad07XX.so"),
          "the ICD is extracted");
    check(fs::is_regular_file(installed.package.staged_dir / "meta.json"),
          "meta.json is extracted");
    check(read_file(installed.package.staged_dir / "vulkan.ad07XX.so") == make_elf(),
          "the ICD bytes round-trip");

    const DriverInspectResult again = install_driver_zip(zip, root);
    check(again.ok && again.package.id == package.id, "re-import is idempotent");

    std::string error;
    check(remove_driver(root, package.id, error), "remove succeeds: " + error);
    check(!fs::exists(root / package.id), "remove deletes the staged directory");
    check(remove_driver(root, package.id, error), "removing an absent id is idempotent");
    check(!remove_driver(root, "not-an-id", error), "invalid id is rejected");
    check_equal(error, "driver_id_invalid", "invalid id is named");

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

void valid_plain_package() {
    const fs::path directory = make_temp_dir();
    const fs::path zip = directory / "bare-turnip.zip";
    write_zip(zip, {{"libvulkan.so", make_elf(), false}});

    const DriverPackage package = expect_ok(zip);
    check_equal(package.display_name, "bare-turnip", "plain package display name");
    check_equal(package.vendor, "unknown", "plain package vendor");
    check_equal(package.driver_version, "unknown", "plain package version");
    check(package.min_api == 0, "plain package minApi");
    check_equal(package.soname, "libvulkan.so", "plain package soname");

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

void meta_errors() {
    const fs::path directory = make_temp_dir();
    const auto write = [&](const std::string &name,
                           const std::vector<ZipEntry> &entries) {
        const fs::path path = directory / name;
        write_zip(path, entries);
        return path;
    };

    expect_error(write("no-meta.zip", {{"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_library_missing");
    expect_error(write("bad-json.zip", {{"meta.json", bytes_of("{"), false},
                                        {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_meta_invalid");
    expect_error(write("schema.zip", {{"meta.json", bytes_of(meta_json("vulkan.ad07XX.so", 2)), false},
                                      {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_meta_schema");
    expect_error(write("name.zip", {{"meta.json", bytes_of(meta_json("vulkan.ad07XX.so", 1, "")), false},
                                    {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_meta_name");
    expect_error(write("no-library.zip", {{"meta.json", bytes_of(R"({"schemaVersion":1,"name":"x"})"), false},
                                          {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_library_missing");
    expect_error(write("absent-library.zip", {{"meta.json", bytes_of(meta_json("libnope.so")), false},
                                              {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_library_missing");
    expect_error(write("traversal-library.zip", {{"meta.json", bytes_of(meta_json("../libvulkan.so")), false},
                                                 {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_library_missing");

    nlohmann::json bad_min = nlohmann::json::parse(meta_json());
    bad_min["minApi"] = -1;
    expect_error(write("min.zip", {{"meta.json", bytes_of(bad_min.dump()), false},
                                   {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_meta_invalid");
    nlohmann::json bad_vendor = nlohmann::json::parse(meta_json());
    bad_vendor["vendor"] = 5;
    expect_error(write("vendor.zip", {{"meta.json", bytes_of(bad_vendor.dump()), false},
                                      {"vulkan.ad07XX.so", make_elf(), false}}),
                 "driver_meta_invalid");

    expect_error(write("ambiguous.zip", {{"libvulkan.so", make_elf(), false},
                                         {"libother.so", make_elf(), false}}),
                 "driver_library_ambiguous");
    expect_error(write("wrong-name.zip", {{"other.so", make_elf(), false}}),
                 "driver_library_missing");

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

void zip_errors() {
    const fs::path directory = make_temp_dir();

    expect_error(directory / "missing.zip", "driver_zip_missing");
    write_file(directory / "empty.zip", {});
    expect_error(directory / "empty.zip", "driver_zip_unsupported");
    write_file(directory / "text.zip", bytes_of("not a zip"));
    expect_error(directory / "text.zip", "driver_zip_unsupported");

    write_zip(directory / "slip.zip", {{"../evil.so", make_elf(), false},
                                       {"libvulkan.so", make_elf(), false}});
    expect_error(directory / "slip.zip", "driver_zip_slip");
    write_zip(directory / "absolute.zip", {{"__evil.so", make_elf(), false},
                                           {"libvulkan.so", make_elf(), false}});
    patch_name(directory / "absolute.zip", "__evil.so", "/evilx.so");
    expect_error(directory / "absolute.zip", "driver_zip_slip");
    write_zip(directory / "duplicate.zip", {{"libvulkan.so", make_elf(), false},
                                             {"libvulkan.so", make_elf(), false}});
    expect_error(directory / "duplicate.zip", "driver_entry_duplicate");

    const std::string long_name(300, 'a');
    write_zip(directory / "long.zip", {{long_name + ".so", make_elf(), false}});
    expect_error(directory / "long.zip", "driver_entry_name");

    write_zip(directory / "encrypted.zip", {{"libvulkan.so", make_elf(), false}});
    patch_headers(directory / "encrypted.zip", true, false);
    expect_error(directory / "encrypted.zip", "driver_entry_encrypted");

    write_zip(directory / "method.zip", {{"libvulkan.so", make_elf(), false}});
    patch_headers(directory / "method.zip", false, true);
    expect_error(directory / "method.zip", "driver_zip_unsupported");

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

void limits() {
    const fs::path directory = make_temp_dir();
    const fs::path zip = directory / "turnip.zip";
    write_zip(zip, {{"meta.json", bytes_of(meta_json()), true},
                    {"vulkan.ad07XX.so", make_elf(), false}});

    DriverLimits archive_limit;
    archive_limit.max_archive_bytes = 16;
    expect_error(zip, "driver_entry_oversize", archive_limit);

    DriverLimits count_limit;
    count_limit.max_entries = 1;
    expect_error(zip, "driver_entry_oversize", count_limit);

    DriverLimits entry_limit;
    entry_limit.max_entry_bytes = 32;
    expect_error(zip, "driver_entry_oversize", entry_limit);

    DriverLimits total_limit;
    total_limit.max_total_bytes = 32;
    expect_error(zip, "driver_entry_oversize", total_limit);

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

void elf_errors() {
    const fs::path directory = make_temp_dir();
    const auto write = [&](const std::string &name, const std::vector<std::uint8_t> &elf) {
        const fs::path path = directory / name;
        write_zip(path, {{"libvulkan.so", elf, false}});
        return path;
    };

    expect_error(write("class.zip", make_elf(ELFCLASS32)), "driver_elf_class");
    expect_error(write("machine.zip", make_elf(ELFCLASS64, EM_X86_64)), "driver_elf_machine");
    expect_error(write("type.zip", make_elf(ELFCLASS64, EM_AARCH64, ET_REL)), "driver_elf_type");
    expect_error(write("empty-dynsym.zip", make_elf(ELFCLASS64, EM_AARCH64, ET_DYN, 0)),
                 "driver_elf_dynsym");

    std::vector<std::uint8_t> truncated = make_elf();
    truncated.resize(40);
    expect_error(write("truncated.zip", truncated), "driver_elf_truncated");

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

void scan_and_atomicity() {
    const fs::path directory = make_temp_dir();
    const fs::path drop = directory / "drop";
    fs::create_directories(drop);
    write_zip(drop / "a.zip", {{"libvulkan.so", make_elf(), false}});
    write_zip(drop / "b.zip", {{"other.so", make_elf(), false}});
    write_zip(drop / "c.ZIP", {{"libvulkan.so", make_elf(), false}});
    write_file(drop / "notes.txt", bytes_of("ignore"));

    const DriverScanResult scan = scan_drop_folder(drop);
    check(scan.packages.size() == 2, "scan finds both zip files case-insensitively");
    check(scan.errors.size() == 1, "scan reports the invalid zip");
    check(scan.errors.front().path.filename() == "b.zip", "scan names the invalid zip");
    check_equal(scan.errors.front().error, "driver_library_missing", "scan error is named");
    check(scan.packages[0].display_name == "a", "scan is sorted by path");
    check(scan.packages[1].display_name == "c", "scan keeps the uppercase extension");

    const DriverScanResult missing = scan_drop_folder(directory / "absent");
    check(missing.packages.empty() && missing.errors.empty(), "missing drop folder is empty");

    const fs::path root = directory / "drivers";
    write_zip(directory / "bad-install.zip", {{"../evil.so", make_elf(), false},
                                              {"libvulkan.so", make_elf(), false}});
    const DriverInspectResult failed = install_driver_zip(directory / "bad-install.zip", root);
    check(!failed.ok, "bad install fails");
    check_equal(failed.error, "driver_zip_slip", "bad install is named");
    if (fs::exists(root)) {
        std::size_t staging = 0;
        for (const auto &entry : fs::directory_iterator(root)) {
            if (entry.path().filename().string().rfind(".staging-", 0) == 0) ++staging;
        }
        check(staging == 0, "failed install leaves no staging directory");
    }

    write_file(directory / "root-file", bytes_of("not a directory"));
    const DriverInspectResult storage =
        install_driver_zip(drop / "a.zip", directory / "root-file");
    check(!storage.ok, "file root is rejected");
    check_equal(storage.error, "driver_storage_unavailable", "storage error is named");

    std::error_code fs_error;
    fs::remove_all(directory, fs_error);
}

} // namespace

int main() {
    try {
        valid_meta_package();
        valid_plain_package();
        meta_errors();
        zip_errors();
        limits();
        elf_errors();
        scan_and_atomicity();
        std::cout << "driver_package_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

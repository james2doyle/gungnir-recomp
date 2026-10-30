// Intake reader tool (T3) — thin line-oriented CLI over the vendored MIT
// framework subset in third_party/yakumo (see PROVENANCE.md). Reads only;
// performs no decryption and writes only where told (iso-extract --out).
//
// Subcommands:
//   intake_tool iso-extract --iso <file> --out <dir>      extract PSP_GAME tree
//   intake_tool elf-inspect <file> [...]                  validate/inspect PRX ELF
//   intake_tool sfo-info <file>                           dump known PARAM.SFO keys
//
// stdout: line protocol ("KEY value" style). stderr errors are named:
// "ERR <NAME> <detail>" with exit code 2 — the orchestrator
// (tools/intake/intake.py) matches on those names, never on prose.

#include "iso_image.hpp"
#include "platform/utf8_path.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "save_data/param_sfo.hpp"

#include <cstring>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using psprecomp::Elf32Image;
using psprecomp::Error;
using psprecomp::hex32;
namespace savedata = mhp3rd::savedata;

constexpr std::uint32_t kPspRelSectionType = 0x70000020u;   // Gungnir EBOOT
constexpr std::uint32_t kPspRelSectionTypeAlt = 0x700000A0u; // vendored build
constexpr char kPspMagic[] = {0x7E, 'P', 'S', 'P'};          // "~PSP"

class NamedError final : public std::runtime_error {
public:
    NamedError(std::string name, const std::string &detail)
        : std::runtime_error(detail), name_(std::move(name)) {}
    const std::string &name() const noexcept { return name_; }
private:
    std::string name_;
};

int fail(const std::string &name, const std::string &detail) {
    std::cerr << "ERR " << name << " " << detail << "\n";
    return 2;
}
} // namespace (anonymous)

// Vendored guest_memory.cpp declares embedder-provided diagnostics (upstream
// they live in runtime.cpp, which is outside the adopted subset — see
// third_party/yakumo/PROVENANCE.md). The intake tool has no runtime, so the
// hooks are inert; they feed env-gated (PSPRECOMP_* / watch) logging only.
namespace psprecomp {
std::int32_t runtime_thread_uid() noexcept { return -1; }
const char *runtime_thread_name() noexcept { return "intake_tool"; }
std::uint32_t runtime_dispatch_pc() noexcept { return 0u; }
} // namespace psprecomp

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("Cannot open file: " + mhp3rd::path_to_utf8(path));
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (data.empty()) throw Error("Empty file: " + mhp3rd::path_to_utf8(path));
    return data;
}

// --- iso-extract -----------------------------------------------------------

void extract_tree(mhp3rd::IsoImage &iso, const std::string &prefix,
                  const std::filesystem::path &out_dir) {
    for (const std::string &name : iso.list(prefix)) {
        const std::string full = prefix.empty() ? name : prefix + "/" + name;
        const auto entry = iso.find(full);
        if (!entry) continue;
        if (entry->directory) {
            extract_tree(iso, full, out_dir);
            continue;
        }
        const std::filesystem::path dest = out_dir / std::filesystem::path(full);
        std::filesystem::create_directories(dest.parent_path());
        if (entry->size == 0u) {
            std::ofstream touch(dest, std::ios::binary);
            std::cout << "FILE " << full << " " << 0 << "\n";
            continue;
        }
        std::vector<std::uint8_t> data(entry->size);
        const std::size_t got = iso.read(static_cast<std::uint64_t>(entry->lba) * mhp3rd::IsoImage::kSectorSize,
                                         data);
        if (got != entry->size) throw Error("Short read from disc image: " + full);
        std::ofstream out(dest, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) throw Error("Cannot write extraction output: " + full);
        std::cout << "FILE " << full << " " << got << "\n";
    }
}

int cmdIsoExtract(int argc, char **argv) {
    std::string iso_path, out_path;
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iso" && i + 1 < argc) iso_path = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    }
    if (iso_path.empty() || out_path.empty())
        return fail("USAGE", "iso-extract requires --iso <file> --out <dir>");
    try {
        const std::filesystem::path out_dir = std::filesystem::absolute(out_path);
        std::filesystem::create_directories(out_dir);
        mhp3rd::IsoImage iso{std::filesystem::path(iso_path)};
        std::cout << "ISO_SIZE " << iso.size_bytes() << "\n";
        extract_tree(iso, "PSP_GAME", out_dir);
        std::cout << "DONE\n";
        return 0;
    } catch (const Error &e) {
        std::cerr << "ERR ISO_INVALID " << e.what() << "\n";
        return 2;
    }
}

// --- elf-inspect -----------------------------------------------------------

// Stripped ELFs (all section names empty) defeat the vendored name-based
// module-info lookup; content-anchored candidate fallback instead. Documented
// in PROVENANCE.md as a caller-side shim over the vendored subset.
std::vector<psprecomp::PspModuleInfo> find_module_info_candidates(const Elf32Image &elf) {
    constexpr std::size_t kStructSize = 56u;
    const auto &raw = elf.bytes();
    std::vector<psprecomp::PspModuleInfo> candidates;
    for (std::size_t off = 0x80u; off + kStructSize <= raw.size(); off += 4u) {
        const std::uint8_t *p = raw.data() + off;
        const std::uint8_t *name = p + 4u; // 28-byte name field
        if (name[0] < 0x20u || name[0] > 0x7Eu) continue;
        std::size_t z = 0u;
        while (z < 28u && name[z] != 0u) ++z;
        if (z == 0u || z >= 28u || name[z] != 0u) continue;
        if (z > 25u) continue;
        bool clean_name = true; // module names are [A-Za-z0-9_.-] only
        for (std::size_t i = 0; i < z; ++i) {
            const char c = static_cast<char>(name[i]);
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
                clean_name = false;
        }
        if (!clean_name) continue;
        auto u32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(p[at]) | (static_cast<std::uint32_t>(p[at + 1u]) << 8u) |
                (static_cast<std::uint32_t>(p[at + 2u]) << 16u) |
                (static_cast<std::uint32_t>(p[at + 3u]) << 24u);
        };
        const std::uint32_t ent_top = u32(36u), ent_end = u32(40u);
        const std::uint32_t stub_top = u32(44u), stub_end = u32(48u);
        const std::uint32_t gp = u32(32u);
        // Structural plausibility only: bounded spans, a gp that is either 0
        // or address-scale, all four bounds in the same regime window.
        if (!(ent_top < ent_end) || !(stub_top <= stub_end)) continue;
        if (ent_end - ent_top > 0x200000u || stub_end - stub_top > 0x200000u) continue;
        const std::uint32_t max_addr = elf.required_ram_size();
        if (gp != 0u && gp > max_addr) continue;
        if (stub_top > max_addr || stub_end > max_addr ||
            ent_top > max_addr || ent_end > max_addr)
            continue;
        psprecomp::PspModuleInfo info{};
        info.attributes = static_cast<std::uint16_t>(p[0]) |
            (static_cast<std::uint16_t>(p[1]) << 8u);
        info.minor_version = p[2];
        info.major_version = p[3];
        info.name.assign(reinterpret_cast<const char *>(name), z);
        info.gp = gp;
        info.ent_top = ent_top;
        info.ent_end = ent_end;
        info.stub_top = stub_top;
        info.stub_end = stub_end;
        info.address = u32(52u);
        candidates.push_back(std::move(info));
    }
    return candidates;
}

void inspect_elf(const std::filesystem::path &path) {
    const std::vector<std::uint8_t> bytes = [&path] {
        try {
            return read_file(path);
        } catch (const Error &e) {
            throw NamedError("ELF_INVALID", e.what());
        }
    }();
    if (bytes.size() >= sizeof(kPspMagic) &&
        std::memcmp(bytes.data(), kPspMagic, sizeof(kPspMagic)) == 0)
        throw NamedError("ENCRYPTED_EXECUTABLE",
                         "module is ~PSP encrypted; decryption happens out-of-process");

    if (bytes.size() < 52u || bytes[0] != 0x7F || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F')
        throw NamedError("ELF_INVALID", "not an ELF image");

    const std::string module = mhp3rd::path_to_utf8(path.filename());
    Elf32Image elf = Elf32Image::from_bytes(std::move(bytes), module);
    if (!elf.is_psp_prx())
        throw NamedError("ELF_INVALID", "e_type is not PSP PRX (0xFFA0)");

    const auto &segments = elf.segments();
    if (segments.empty())
        throw NamedError("ELF_INVALID", "no ELF segments");

    std::cout << "module " << module << "\n";
    std::cout << "ok yes\n";
    std::cout << "type " << hex32(static_cast<std::uint32_t>(elf.type())) << "\n";
    std::cout << "entry " << hex32(elf.entry()) << "\n";
    std::cout << "segments " << segments.size() << "\n";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto &s = segments[i];
        std::cout << "segment " << i << " " << hex32(s.offset) << " " << hex32(s.vaddr)
                  << " " << s.file_size << " " << s.memory_size << " " << hex32(s.flags)
                  << "\n";
    }

    std::uint32_t rel_section_entries = 0u;
    std::uint32_t rel_32 = 0u, rel_hi16 = 0u, rel_lo16 = 0u, rel_other = 0u;
    for (const auto &section : elf.sections()) {
        if (section.type != kPspRelSectionType && section.type != kPspRelSectionTypeAlt) continue;
        const std::size_t raw_size = section.size;
        if (raw_size % 8u != 0u)
            throw Error("PSP relocation section size is not a multiple of 8: " + section.name);
        if (static_cast<std::uint64_t>(section.offset) + raw_size >
            static_cast<std::uint64_t>(elf.bytes().size()))
            throw Error("PSP relocation section runs past end of file: " + section.name);
        const std::size_t count = raw_size / 8u;
        rel_section_entries += static_cast<std::uint32_t>(count);
        std::cout << "reloc_section " << section.name << " " << hex32(section.type) << " "
                  << count << "\n";
        const std::vector<std::uint8_t> &raw = elf.bytes();
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t at = section.offset + i * 8u + 4u; // r_info = entry + 4
            const std::uint8_t *p = raw.data() + at;
            const std::uint32_t info = static_cast<std::uint32_t>(p[0]) |
                (static_cast<std::uint32_t>(p[1]) << 8u) |
                (static_cast<std::uint32_t>(p[2]) << 16u) |
                (static_cast<std::uint32_t>(p[3]) << 24u);
            switch (info & 0xFFu) {
                case 2: ++rel_32; break;
                case 5: ++rel_hi16; break;
                case 6: ++rel_lo16; break;
                default: ++rel_other; break;
            }
        }
    }
    std::cout << "reloc_sections_entries " << rel_section_entries << "\n";
    std::cout << "reloc_R_MIPS_32 " << rel_32 << "\n";
    std::cout << "reloc_R_MIPS_HI16 " << rel_hi16 << "\n";
    std::cout << "reloc_R_MIPS_LO16 " << rel_lo16 << "\n";
    std::cout << "reloc_other " << rel_other << "\n";

    const std::uint32_t ram = elf.required_ram_size();
    psprecomp::GuestMemory memory(ram);
    constexpr std::uint32_t kLoadBase = psprecomp::kDefaultPspUserLoadBase;
    // Relocation-aware load: the EBOOT is a PRX whose pointer words are
    // module-relative; the PSP kernel relocates them to runtime addresses on
    // load. The vendored applicant handles the same 0x700000A0 flavor this
    // disc's ELF ships (verified below), so scan_imports sees runtime values.
    const auto reloc_stats = elf.load_and_relocate(memory);
    std::cout << "reloc_applied_total " << reloc_stats.total << "\n";
    std::cout << "reloc_applied_invalid " << reloc_stats.invalid << "\n";
    std::cout << "reloc_applied_unsupported " << reloc_stats.unsupported << "\n";

    const auto try_scan = [&](const psprecomp::PspModuleInfo &raw_info, bool shift)
        -> std::vector<psprecomp::PspImport> {
        psprecomp::PspModuleInfo used = raw_info;
        if (shift) {
            used.gp += kLoadBase;
            used.ent_top += kLoadBase;
            used.ent_end += kLoadBase;
            used.stub_top += kLoadBase;
            used.stub_end += kLoadBase;
            used.address += kLoadBase;
        }
        return elf.scan_imports(memory, used);
    };

    std::cout << "module_name -unknown-\n";
    std::cout << "module_gp -unknown-\n";
    std::cout << "module_stub_fields -unknown-\n";
    std::cout << "imports 0\n";

    // Candidate order: vendored name-based lookup first, then content-anchored
    // fallbacks; the import walker itself validates candidates functionally
    // (a wrong struct throws or yields unusable rows).
    std::optional<psprecomp::PspModuleInfo> named = elf.find_module_info(memory);
    std::vector<psprecomp::PspModuleInfo> candidates;
    if (named) candidates.push_back(*named);
    for (const auto &c : find_module_info_candidates(elf)) candidates.push_back(c);
    for (const auto &candidate : candidates) {
        const bool runtime_absolute =
            candidate.stub_top >= psprecomp::kDefaultPspUserLoadBase &&
            candidate.stub_end <= psprecomp::kDefaultPspUserLoadBase + ram &&
            candidate.stub_top < candidate.stub_end;
        try {
            auto imports = try_scan(candidate, !runtime_absolute);
            bool sane = !imports.empty();
            for (const auto &imp : imports) {
                if (imp.library.empty()) sane = false;
                for (const char c : imp.library) {
                    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                        sane = false;
                }
            }
            if (!sane) continue;
            std::cout << "module_name " << candidate.name << "\n";
            std::cout << "module_gp " << hex32(candidate.gp) << "\n";
            std::cout << "module_stub_fields "
                      << (runtime_absolute ? "runtime-absolute" : "module-relative") << "\n";
            std::cout << "imports " << imports.size() << "\n";
            for (const auto &imp : imports) {
                std::cout << "import " << imp.library << " " << hex32(imp.nid) << " "
                          << hex32(imp.stub_address) << "\n";
            }
            return;
        } catch (const Error &) {
            continue; // not a usable module header — keep scanning
        }
    }
    std::cout << "module_info none\n";
}

int cmdElfInspect(int argc, char **argv) {
    if (argc < 1) return fail("USAGE", "elf-inspect requires at least one ELF file");
    for (int i = 0; i < argc; ++i) {
        const std::filesystem::path path = argv[i];
        try {
            inspect_elf(path);
        } catch (const NamedError &e) {
            std::cerr << "ERR " << e.name() << " " << e.what() << "\n";
            return 2;
        } catch (const Error &e) {
            std::cerr << "ERR ELF_INVALID " << e.what() << "\n";
            return 2;
        }
    }
    return 0;
}

// --- sfo-info --------------------------------------------------------------

int cmdSfoInfo(int argc, char **argv) {
    if (argc != 1) return fail("USAGE", "sfo-info requires exactly one PARAM.SFO file");
    const std::vector<std::uint8_t> bytes = read_file(std::filesystem::path(argv[0]));
    const auto sfo = savedata::ParamSfo::parse(bytes);
    if (!sfo) return fail("SFO_INVALID", "PARAM.SFO parse failed");

    // ParamSfo exposes per-key lookups only; probe the standard PARAM.SFO
    // row set (game-side PARAM.SFO) and print what is present.
    const char *string_keys[] = {"DISC_ID", "DISC_VERSION", "TITLE", "CATEGORY",
                                 "PSP_SYSTEM_VER", "SAVEDATA_DIRECTORY"};
    for (const char *key : string_keys) {
        if (const auto value = sfo->string(key)) std::cout << key << "=" << *value << "\n";
    }
    const char *int_keys[] = {"BOOTABLE", "PARENTAL_LEVEL", "REGION_CODE"};
    for (const char *key : int_keys) {
        if (const auto value = sfo->integer(key))
            std::cout << key << "=" << static_cast<unsigned long long>(*value) << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2) return fail("USAGE", "no subcommand");
        const std::string cmd = argv[1];
        if (cmd == "iso-extract") return cmdIsoExtract(argc - 2, argv + 2);
        if (cmd == "elf-inspect") return cmdElfInspect(argc - 2, argv + 2);
        if (cmd == "sfo-info") return cmdSfoInfo(argc - 2, argv + 2);
        return fail("USAGE", "unknown subcommand: " + cmd);
    } catch (const Error &e) {
        std::cerr << "ERR TOOL_ERROR " << e.what() << "\n";
        return 2;
    }
}

#include "session/snapshot.hpp"

#include "audio/sas_core.hpp"
#include "hle/hle_atrac.hpp"
#include "hle/hle_audio.hpp"
#include "hle/hle_common.hpp"
#include "hle/hle_media.hpp"
#include "hle/hle_module.hpp"
#include "hle/hle_utility.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <vector>

namespace gungnir::session {

constexpr char kMagic[4] = {'G', 'S', 'N', 'P'};
constexpr std::size_t kCpuPayloadSize = 848u;
constexpr std::size_t kClockPayloadSize = 40u;
constexpr std::size_t kClockPayloadSizeWallBase = 32u;
constexpr std::size_t kClockPayloadSizeLegacy = 24u;

static void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
}

[[nodiscard]] static std::uint32_t get_u32(const std::uint8_t *p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
        (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

[[nodiscard]] static std::uint64_t get_u64(const std::uint8_t *p) noexcept {
    std::uint64_t value = 0u;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return value;
}

[[nodiscard]] static std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}

static void set_error(SnapshotError &error, SnapshotError::Code code, const std::string &message) {
    error.code = code;
    error.message = message;
}

void encode_context(const psprecomp::AllegrexContext &cpu, std::vector<std::uint8_t> &out) {
    for (std::uint32_t v : cpu.gpr) put_u32(out, v);
    put_u32(out, cpu.hi);
    put_u32(out, cpu.lo);
    put_u32(out, cpu.pc);
    for (float f : cpu.fpr) put_u32(out, std::bit_cast<std::uint32_t>(f));
    put_u32(out, cpu.fcr31);
    for (float f : cpu.vfpu) put_u32(out, std::bit_cast<std::uint32_t>(f));
    for (std::uint32_t v : cpu.vfpu_ctrl) put_u32(out, v);
}

bool decode_context(std::span<const std::uint8_t> payload, psprecomp::AllegrexContext &cpu, SnapshotError &error) {
    if (payload.size() != kCpuPayloadSize) {
        set_error(error, SnapshotError::kBadLength,
            "snapshot_bad_length: cpu expected 848 got " + std::to_string(payload.size()));
        return false;
    }
    const std::uint8_t *p = payload.data();
    for (auto &gpr : cpu.gpr) {
        gpr = get_u32(p);
        p += 4;
    }
    cpu.hi = get_u32(p);
    p += 4;
    cpu.lo = get_u32(p);
    p += 4;
    cpu.pc = get_u32(p);
    p += 4;
    for (auto &fpr : cpu.fpr) {
        fpr = std::bit_cast<float>(get_u32(p));
        p += 4;
    }
    cpu.fcr31 = get_u32(p);
    p += 4;
    for (auto &v : cpu.vfpu) {
        v = std::bit_cast<float>(get_u32(p));
        p += 4;
    }
    for (auto &c : cpu.vfpu_ctrl) {
        c = get_u32(p);
        p += 4;
    }
    return true;
}

void encode_clock(const ClockState &clock, std::vector<std::uint8_t> &out) {
    put_u64(out, clock.now_us);
    put_u64(out, clock.next_vblank_us);
    put_u64(out, clock.vblank_count);
    put_u64(out, clock.wall_base_us);
    put_u64(out, clock.dispatch_phase);
}

bool decode_clock(std::span<const std::uint8_t> payload, ClockState &clock, SnapshotError &error) {
    if (payload.size() != kClockPayloadSize && payload.size() != kClockPayloadSizeWallBase &&
        payload.size() != kClockPayloadSizeLegacy) {
        set_error(error, SnapshotError::kBadLength,
            "snapshot_bad_length: clock expected 40 got " + std::to_string(payload.size()));
        return false;
    }
    clock.now_us = get_u64(payload.data());
    clock.next_vblank_us = get_u64(payload.data() + 8);
    clock.vblank_count = get_u64(payload.data() + 16);
    clock.wall_base_us = payload.size() >= kClockPayloadSizeWallBase ? get_u64(payload.data() + 24) : 0u;
    clock.dispatch_phase = payload.size() == kClockPayloadSize ? get_u64(payload.data() + 32) : 0u;
    return true;
}

bool snapshot_save(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &corpus_hash,
    const std::array<std::uint8_t, 32> &config_hash, const psprecomp::GuestMemory &memory,
    const psprecomp::AllegrexContext &cpu, const ClockState &clock, SnapshotError &error) {
    if (memory.size() != kSnapshotRamSize || memory.vram_size() != kSnapshotVramSize) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: unexpected guest memory size");
        return false;
    }
    std::vector<std::uint8_t> out;
    out.reserve(76u + 8u * 4u + kSnapshotRamSize + kSnapshotVramSize + kCpuPayloadSize + kClockPayloadSize + 8u);
    out.insert(out.end(), {static_cast<std::uint8_t>(kMagic[0]), static_cast<std::uint8_t>(kMagic[1]),
                              static_cast<std::uint8_t>(kMagic[2]), static_cast<std::uint8_t>(kMagic[3])});
    put_u32(out, kSnapshotVersion);
    out.insert(out.end(), corpus_hash.begin(), corpus_hash.end());
    out.insert(out.end(), config_hash.begin(), config_hash.end());
    put_u32(out, 4u);

    const std::uint8_t *ram = memory.raw_pointer(0x08000000u, kSnapshotRamSize);
    const std::uint8_t *vram = memory.raw_pointer(0x04000000u, kSnapshotVramSize);
    if (ram == nullptr || vram == nullptr) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: guest memory not contiguous");
        return false;
    }
    put_u32(out, kTagRam);
    put_u32(out, kSnapshotRamSize);
    out.insert(out.end(), ram, ram + kSnapshotRamSize);
    put_u32(out, kTagVram);
    put_u32(out, kSnapshotVramSize);
    out.insert(out.end(), vram, vram + kSnapshotVramSize);

    std::vector<std::uint8_t> cpu_bytes;
    cpu_bytes.reserve(kCpuPayloadSize);
    encode_context(cpu, cpu_bytes);
    put_u32(out, kTagCpu);
    put_u32(out, static_cast<std::uint32_t>(cpu_bytes.size()));
    out.insert(out.end(), cpu_bytes.begin(), cpu_bytes.end());

    std::vector<std::uint8_t> clock_bytes;
    encode_clock(clock, clock_bytes);
    put_u32(out, kTagClock);
    put_u32(out, static_cast<std::uint32_t>(clock_bytes.size()));
    out.insert(out.end(), clock_bytes.begin(), clock_bytes.end());

    put_u64(out, fnv1a64(out));

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            set_error(error, SnapshotError::kWrite, "snapshot_write_failed: cannot open " + tmp.string());
            return false;
        }
        file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        file.flush();
        if (!file) {
            set_error(error, SnapshotError::kWrite, "snapshot_write_failed: short write " + tmp.string());
            std::remove(tmp.string().c_str());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.string().c_str());
        set_error(error, SnapshotError::kWrite, "snapshot_write_failed: rename failed " + ec.message());
        return false;
    }
    set_error(error, SnapshotError::kOk, "");
    return true;
}

bool snapshot_load(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &expected_corpus,
    const std::array<std::uint8_t, 32> &expected_config, psprecomp::GuestMemory &memory,
    psprecomp::AllegrexContext &cpu, ClockState &clock, SnapshotError &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        set_error(error, SnapshotError::kOpen, "snapshot_open_failed: cannot open " + path.string());
        return false;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 76u + 8u) {
        set_error(error, SnapshotError::kTruncated,
            "snapshot_truncated: size " + std::to_string(data.size()) + " below header");
        return false;
    }
    if (data[0] != static_cast<std::uint8_t>(kMagic[0]) || data[1] != static_cast<std::uint8_t>(kMagic[1]) ||
        data[2] != static_cast<std::uint8_t>(kMagic[2]) || data[3] != static_cast<std::uint8_t>(kMagic[3])) {
        set_error(error, SnapshotError::kBadMagic, "snapshot_bad_magic: expected GSNP");
        return false;
    }
    const std::uint32_t version = get_u32(data.data() + 4);
    if (version != kSnapshotVersion) {
        set_error(error, SnapshotError::kBadVersion,
            "snapshot_bad_version: got " + std::to_string(version) + " expected " +
                std::to_string(kSnapshotVersion));
        return false;
    }
    if (std::memcmp(data.data() + 8, expected_corpus.data(), 32) != 0) {
        set_error(error, SnapshotError::kBadCorpus, "snapshot_bad_corpus: corpus identity mismatch");
        return false;
    }
    if (std::memcmp(data.data() + 40, expected_config.data(), 32) != 0) {
        set_error(error, SnapshotError::kBadConfig, "snapshot_bad_config: session config mismatch");
        return false;
    }
    const std::uint32_t section_count = get_u32(data.data() + 72);
    if (section_count != 4u) {
        set_error(error, SnapshotError::kIncomplete,
            "snapshot_incomplete: expected 4 sections got " + std::to_string(section_count));
        return false;
    }

    const std::uint64_t stored_hash = get_u64(data.data() + data.size() - 8);
    const std::uint64_t computed =
        fnv1a64(std::span<const std::uint8_t>(data.data(), data.size() - 8));
    if (stored_hash != computed) {
        set_error(error, SnapshotError::kBadHash, "snapshot_bad_hash: payload hash mismatch");
        return false;
    }

    std::size_t offset = 76u;
    const std::uint32_t expected_tags[4] = {kTagRam, kTagVram, kTagCpu, kTagClock};
    std::span<const std::uint8_t> ram_span;
    std::span<const std::uint8_t> vram_span;
    std::span<const std::uint8_t> cpu_span;
    std::span<const std::uint8_t> clock_span;
    for (int i = 0; i < 4; ++i) {
        if (offset + 8u > data.size() - 8u) {
            set_error(error, SnapshotError::kTruncated, "snapshot_truncated: section header overruns file");
            return false;
        }
        const std::uint32_t tag = get_u32(data.data() + offset);
        const std::uint32_t length = get_u32(data.data() + offset + 4);
        offset += 8u;
        if (tag != expected_tags[i]) {
            if (tag == kTagRam || tag == kTagVram || tag == kTagCpu || tag == kTagClock) {
                set_error(error, SnapshotError::kDuplicate,
                    "snapshot_duplicate_section: unexpected tag order got " + std::to_string(tag));
            } else {
                set_error(error, SnapshotError::kBadTag, "snapshot_bad_tag: unknown tag " + std::to_string(tag));
            }
            return false;
        }
        if (offset + length > data.size() - 8u) {
            set_error(error, SnapshotError::kTruncated, "snapshot_truncated: section payload overruns file");
            return false;
        }
        const std::span<const std::uint8_t> payload(data.data() + offset, length);
        if (tag == kTagRam) {
            if (length != kSnapshotRamSize) {
                set_error(error, SnapshotError::kBadLength,
                    "snapshot_bad_length: ram expected " + std::to_string(kSnapshotRamSize));
                return false;
            }
            ram_span = payload;
        } else if (tag == kTagVram) {
            if (length != kSnapshotVramSize) {
                set_error(error, SnapshotError::kBadLength,
                    "snapshot_bad_length: edram expected " + std::to_string(kSnapshotVramSize));
                return false;
            }
            vram_span = payload;
        } else if (tag == kTagCpu) {
            cpu_span = payload;
        } else {
            clock_span = payload;
        }
        offset += length;
    }
    if (offset != data.size() - 8u) {
        set_error(error, SnapshotError::kTruncated, "snapshot_truncated: trailing bytes after sections");
        return false;
    }

    psprecomp::AllegrexContext staged_cpu;
    ClockState staged_clock{};
    if (!decode_context(cpu_span, staged_cpu, error)) return false;
    if (!decode_clock(clock_span, staged_clock, error)) return false;

    if (memory.size() != kSnapshotRamSize || memory.vram_size() != kSnapshotVramSize) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: destination memory size mismatch");
        return false;
    }
    std::uint8_t *ram_dst = memory.raw_pointer(0x08000000u, kSnapshotRamSize);
    std::uint8_t *vram_dst = memory.raw_pointer(0x04000000u, kSnapshotVramSize);
    if (ram_dst == nullptr || vram_dst == nullptr) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: destination not contiguous");
        return false;
    }
    std::memcpy(ram_dst, ram_span.data(), kSnapshotRamSize);
    std::memcpy(vram_dst, vram_span.data(), kSnapshotVramSize);
    cpu = staged_cpu;
    clock = staged_clock;
    set_error(error, SnapshotError::kOk, "");
    return true;
}

namespace {

void append_section(std::vector<std::uint8_t> &out, std::uint32_t tag, std::span<const std::uint8_t> payload) {
    put_u32(out, tag);
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

bool write_atomic(const std::filesystem::path &path, std::span<const std::uint8_t> bytes, SnapshotError &error) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            set_error(error, SnapshotError::kWrite, "snapshot_write_failed: cannot open " + tmp.string());
            return false;
        }
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            set_error(error, SnapshotError::kWrite, "snapshot_write_failed: short write " + tmp.string());
            std::remove(tmp.string().c_str());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.string().c_str());
        set_error(error, SnapshotError::kWrite, "snapshot_write_failed: rename failed " + ec.message());
        return false;
    }
    return true;
}

void map_kernel_error(const std::string &message, SnapshotError &error) {
    SnapshotError::Code code = SnapshotError::kBadLength;
    if (message.rfind("snapshot_host_wait:", 0) == 0) code = SnapshotError::kHostWait;
    if (message.rfind("snapshot_busy:", 0) == 0) code = SnapshotError::kBusy;
    if (message.rfind("snapshot_duplicate_uid:", 0) == 0 ||
        message.rfind("snapshot_duplicate_section:", 0) == 0)
        code = SnapshotError::kDuplicate;
    if (message.rfind("snapshot_incomplete:", 0) == 0) code = SnapshotError::kIncomplete;
    if (message.rfind("snapshot_truncated:", 0) == 0) code = SnapshotError::kTruncated;
    if (message.rfind("snapshot_bad_tag:", 0) == 0) code = SnapshotError::kBadTag;
    if (message.rfind("snapshot_bad_hash:", 0) == 0) code = SnapshotError::kBadHash;
    if (message.rfind("snapshot_bad_magic:", 0) == 0) code = SnapshotError::kBadMagic;
    if (message.rfind("snapshot_bad_version:", 0) == 0) code = SnapshotError::kBadVersion;
    if (message.rfind("snapshot_bad_corpus:", 0) == 0) code = SnapshotError::kBadCorpus;
    if (message.rfind("snapshot_bad_config:", 0) == 0) code = SnapshotError::kBadConfig;
    if (message.rfind("snapshot_bad_length:", 0) == 0) code = SnapshotError::kBadLength;
    if (message.rfind("snapshot_dangling_wait:", 0) == 0) code = SnapshotError::kDangling;
    if (message.rfind("snapshot_open_failed:", 0) == 0) code = SnapshotError::kOpen;
    if (message.rfind("snapshot_write_failed:", 0) == 0) code = SnapshotError::kWrite;
    set_error(error, code, message);
}

} // namespace

bool snapshot_save_full(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &corpus_hash,
    const std::array<std::uint8_t, 32> &config_hash, const psprecomp::GuestMemory &memory,
    const psprecomp::AllegrexContext &cpu, const ClockState &clock, const hle::Kernel &kernel,
    SnapshotError &error) {
    if (memory.size() != kSnapshotRamSize || memory.vram_size() != kSnapshotVramSize) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: unexpected guest memory size");
        return false;
    }
    std::vector<std::vector<std::uint8_t>> kernel_sections;
    {
        std::string kernel_error;
        if (!kernel.export_snapshot_kernel(kernel_sections, kernel_error)) {
            map_kernel_error(kernel_error, error);
            return false;
        }
    }
    if (kernel_sections.size() != 12u) {
        set_error(error, SnapshotError::kIncomplete, "snapshot_incomplete: kernel export gave 12 sections");
        return false;
    }
    std::vector<std::uint8_t> io_bytes;
    std::vector<std::uint8_t> audio_bytes;
    std::vector<std::uint8_t> sas_bytes;
    std::vector<std::uint8_t> atrac_bytes;
    std::vector<std::uint8_t> ge_bytes;
    std::vector<std::uint8_t> module_bytes;
    std::vector<std::uint8_t> utility_bytes;
    {
        std::string hle_error;
        if (!hle::export_snapshot_io(io_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::export_snapshot_audio(audio_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!gungnir::audio::export_snapshot_sas(sas_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::export_snapshot_atrac(atrac_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::export_snapshot_media(ge_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::export_snapshot_modules(module_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::export_snapshot_utility(utility_bytes, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
    }
    const std::uint8_t *ram = memory.raw_pointer(0x08000000u, kSnapshotRamSize);
    const std::uint8_t *vram = memory.raw_pointer(0x04000000u, kSnapshotVramSize);
    if (ram == nullptr || vram == nullptr) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: guest memory not contiguous");
        return false;
    }
    std::vector<std::uint8_t> out;
    out.reserve(76u + 8u * 23u + 4096u + kSnapshotRamSize + kSnapshotVramSize);
    out.insert(out.end(), {static_cast<std::uint8_t>(kMagic[0]), static_cast<std::uint8_t>(kMagic[1]),
                              static_cast<std::uint8_t>(kMagic[2]), static_cast<std::uint8_t>(kMagic[3])});
    put_u32(out, kSnapshotVersion);
    out.insert(out.end(), corpus_hash.begin(), corpus_hash.end());
    out.insert(out.end(), config_hash.begin(), config_hash.end());
    put_u32(out, kFullSectionCount);

    put_u32(out, kTagRam);
    put_u32(out, kSnapshotRamSize);
    out.insert(out.end(), ram, ram + kSnapshotRamSize);
    put_u32(out, kTagVram);
    put_u32(out, kSnapshotVramSize);
    out.insert(out.end(), vram, vram + kSnapshotVramSize);

    std::vector<std::uint8_t> cpu_bytes;
    cpu_bytes.reserve(kCpuPayloadSize);
    encode_context(cpu, cpu_bytes);
    append_section(out, kTagCpu, cpu_bytes);

    std::vector<std::uint8_t> clock_bytes;
    encode_clock(clock, clock_bytes);
    append_section(out, kTagClock, clock_bytes);

    constexpr std::uint32_t kernel_tags[12] = {kTagKids, kTagThreads, kTagSema, kTagEvent, kTagMutex, kTagVpl,
        kTagFpl, kTagMsgPipe, kTagCallback, kTagVTimer, kTagMemBlocks, kTagSubInt};
    for (int i = 0; i < 12; ++i)
        append_section(out, kernel_tags[i], kernel_sections[static_cast<std::size_t>(i)]);

    append_section(out, kTagFds, io_bytes);
    append_section(out, kTagAudio, audio_bytes);
    append_section(out, kTagSas, sas_bytes);
    append_section(out, kTagAtrac, atrac_bytes);
    append_section(out, kTagGe, ge_bytes);
    append_section(out, kTagModules, module_bytes);
    append_section(out, kTagUtility, utility_bytes);

    put_u64(out, fnv1a64(out));
    if (!write_atomic(path, out, error)) return false;
    set_error(error, SnapshotError::kOk, "");
    return true;
}

bool snapshot_load_full(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &expected_corpus,
    const std::array<std::uint8_t, 32> &expected_config, psprecomp::GuestMemory &memory,
    psprecomp::AllegrexContext &cpu, ClockState &clock, hle::Kernel &kernel, SnapshotError &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        set_error(error, SnapshotError::kOpen, "snapshot_open_failed: cannot open " + path.string());
        return false;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 76u + 8u) {
        set_error(error, SnapshotError::kTruncated,
            "snapshot_truncated: size " + std::to_string(data.size()) + " below header");
        return false;
    }
    if (data[0] != static_cast<std::uint8_t>(kMagic[0]) || data[1] != static_cast<std::uint8_t>(kMagic[1]) ||
        data[2] != static_cast<std::uint8_t>(kMagic[2]) || data[3] != static_cast<std::uint8_t>(kMagic[3])) {
        set_error(error, SnapshotError::kBadMagic, "snapshot_bad_magic: expected GSNP");
        return false;
    }
    if (get_u32(data.data() + 4) != kSnapshotVersion) {
        set_error(error, SnapshotError::kBadVersion, "snapshot_bad_version: version mismatch");
        return false;
    }
    if (std::memcmp(data.data() + 8, expected_corpus.data(), 32) != 0) {
        set_error(error, SnapshotError::kBadCorpus, "snapshot_bad_corpus: corpus identity mismatch");
        return false;
    }
    if (std::memcmp(data.data() + 40, expected_config.data(), 32) != 0) {
        set_error(error, SnapshotError::kBadConfig, "snapshot_bad_config: session config mismatch");
        return false;
    }
    if (get_u32(data.data() + 72) != kFullSectionCount) {
        set_error(error, SnapshotError::kIncomplete,
            "snapshot_incomplete: expected 23 sections got " + std::to_string(get_u32(data.data() + 72)));
        return false;
    }
    const std::uint64_t stored = get_u64(data.data() + data.size() - 8);
    if (stored != fnv1a64(std::span<const std::uint8_t>(data.data(), data.size() - 8))) {
        set_error(error, SnapshotError::kBadHash, "snapshot_bad_hash: payload hash mismatch");
        return false;
    }

    constexpr std::uint32_t expected_tags[23] = {kTagRam, kTagVram, kTagCpu, kTagClock, kTagKids, kTagThreads,
        kTagSema, kTagEvent, kTagMutex, kTagVpl, kTagFpl, kTagMsgPipe, kTagCallback, kTagVTimer, kTagMemBlocks,
        kTagSubInt, kTagFds, kTagAudio, kTagSas, kTagAtrac, kTagGe, kTagModules, kTagUtility};
    std::size_t offset = 76u;
    std::span<const std::uint8_t> ram_span;
    std::span<const std::uint8_t> vram_span;
    std::span<const std::uint8_t> cpu_span;
    std::span<const std::uint8_t> clock_span;
    std::map<std::uint32_t, std::vector<std::uint8_t>> kernel_sections;
    std::map<std::uint32_t, std::vector<std::uint8_t>> hle_sections;
    for (int i = 0; i < 23; ++i) {
        if (offset + 8u > data.size() - 8u) {
            set_error(error, SnapshotError::kTruncated, "snapshot_truncated: section header overruns file");
            return false;
        }
        const std::uint32_t tag = get_u32(data.data() + offset);
        const std::uint32_t length = get_u32(data.data() + offset + 4);
        offset += 8u;
        if (tag != expected_tags[i]) {
            set_error(error, SnapshotError::kBadTag,
                "snapshot_bad_tag: expected tag " + std::to_string(expected_tags[i]) + " got " +
                    std::to_string(tag));
            return false;
        }
        if (offset + length > data.size() - 8u) {
            set_error(error, SnapshotError::kTruncated, "snapshot_truncated: section payload overruns file");
            return false;
        }
        if (tag == kTagRam) {
            if (length != kSnapshotRamSize) {
                set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: ram size mismatch");
                return false;
            }
            ram_span = std::span<const std::uint8_t>(data.data() + offset, length);
        } else if (tag == kTagVram) {
            if (length != kSnapshotVramSize) {
                set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: edram size mismatch");
                return false;
            }
            vram_span = std::span<const std::uint8_t>(data.data() + offset, length);
        } else if (tag == kTagCpu) {
            cpu_span = std::span<const std::uint8_t>(data.data() + offset, length);
        } else if (tag == kTagClock) {
            clock_span = std::span<const std::uint8_t>(data.data() + offset, length);
        } else if (tag < kTagFds) {
            kernel_sections[tag] = std::vector<std::uint8_t>(data.data() + offset, data.data() + offset + length);
        } else {
            hle_sections[tag] = std::vector<std::uint8_t>(data.data() + offset, data.data() + offset + length);
        }
        offset += length;
    }
    if (offset != data.size() - 8u) {
        set_error(error, SnapshotError::kTruncated, "snapshot_truncated: trailing bytes after sections");
        return false;
    }

    psprecomp::AllegrexContext staged_cpu;
    ClockState staged_clock{};
    if (!decode_context(cpu_span, staged_cpu, error)) return false;
    if (!decode_clock(clock_span, staged_clock, error)) return false;

    {
        std::string hle_error;
        if (!kernel.import_snapshot_kernel(kernel_sections, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        auto need = [&](std::uint32_t tag, std::span<const std::uint8_t> &span) -> bool {
            const auto found = hle_sections.find(tag);
            if (found == hle_sections.end()) {
                set_error(error, SnapshotError::kIncomplete,
                    "snapshot_incomplete: missing hle section tag=" + std::to_string(tag));
                return false;
            }
            span = std::span<const std::uint8_t>(found->second.data(), found->second.size());
            return true;
        };
        std::span<const std::uint8_t> io_span, audio_span, sas_span, atrac_span, ge_span, module_span, utility_span;
        if (!need(kTagFds, io_span) || !need(kTagAudio, audio_span) || !need(kTagSas, sas_span) ||
            !need(kTagAtrac, atrac_span) || !need(kTagGe, ge_span) || !need(kTagModules, module_span) ||
            !need(kTagUtility, utility_span))
            return false;
        if (!hle::import_snapshot_io(io_span, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::import_snapshot_audio(audio_span, kernel, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!gungnir::audio::import_snapshot_sas(sas_span, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::import_snapshot_atrac(atrac_span, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::import_snapshot_media(ge_span, kernel.runtime(), hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::import_snapshot_modules(module_span, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
        if (!hle::import_snapshot_utility(utility_span, hle_error)) {
            map_kernel_error(hle_error, error);
            return false;
        }
    }

    if (memory.size() != kSnapshotRamSize || memory.vram_size() != kSnapshotVramSize) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: destination memory size mismatch");
        return false;
    }
    std::uint8_t *ram_dst = memory.raw_pointer(0x08000000u, kSnapshotRamSize);
    std::uint8_t *vram_dst = memory.raw_pointer(0x04000000u, kSnapshotVramSize);
    if (ram_dst == nullptr || vram_dst == nullptr) {
        set_error(error, SnapshotError::kBadLength, "snapshot_bad_length: destination not contiguous");
        return false;
    }
    std::memcpy(ram_dst, ram_span.data(), kSnapshotRamSize);
    std::memcpy(vram_dst, vram_span.data(), kSnapshotVramSize);
    cpu = staged_cpu;
    clock = staged_clock;
    kernel.restore_clock(staged_clock.now_us, staged_clock.next_vblank_us, staged_clock.vblank_count);
    set_error(error, SnapshotError::kOk, "");
    return true;
}

} // namespace gungnir::session

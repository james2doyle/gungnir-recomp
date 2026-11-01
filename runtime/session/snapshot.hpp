#pragma once

// T15a slice 1 — host save-state container for RAM/EDRAM + CPU + clock.
// T15b slice 2 — threads + kernel objects + timers (tags 5-16).
//
// Format (little-endian, deterministic order):
//   header: magic "GSNP" (4), version u32 (1), corpus_hash (32), config_hash (32),
//           section_count u32 (4 for slice 1, 16 for full)
//   sections in order: RAM (tag 1, 32 MiB), EDRAM (tag 2, 2 MiB),
//                     CPU (tag 3, 848 B), CLOCK (tag 4, 40 B: now_us,
//                     next_vblank_us, vblank_count, wall_base_us,
//                     dispatch_phase; 24/32 B legacy files without the
//                     later fields still load),
//                     KIDS (5), THREADS (6), SEMA (7), EVENT (8), MUTEX (9),
//                     VPL (10), FPL (11), MSGPIPE (12), CALLBACK (13),
//                     VTIMER (14), MEMBLOCKS (15), SUBINT (16)
//     each: tag u32 + length u32 + payload
//   trailer: fnv1a64 over all preceding bytes (u64)
// Atomic write (temp + rename), fail-closed load (no partial restore).
// Slice-1 files (4 sections) are rejected as snapshot_incomplete by load_full.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace psprecomp {
class GuestMemory;
struct AllegrexContext;
} // namespace psprecomp

namespace gungnir::hle {
class Kernel;
}

namespace gungnir::session {

struct ClockState {
    std::uint64_t now_us{};
    std::uint64_t next_vblank_us{};
    std::uint64_t vblank_count{};
    // Host wall-clock base (boot_unix_us): guest wall time as observed through
    // sceRtc/sceKernelLibcTime is wall_base_us + now_us. Saved so a restore in
    // a new host process keeps guest ticks continuous; 0 means "not stored"
    // (legacy files) and the loader keeps the current base.
    std::uint64_t wall_base_us{};
    // Runtime starvation-boundary phase (dispatches_since_import_): preemption
    // boundaries fire every GUNGNIR_STARVATION_INTERVAL dispatches, so the
    // phase selects all future thread interleavings. Saved so a restore keeps
    // the same boundary schedule; 0 means "not stored" (legacy files) and the
    // loader keeps the fresh Runtime's zero phase, matching old behavior.
    std::uint64_t dispatch_phase{};
};

struct SnapshotError {
    enum Code {
        kOk = 0,
        kOpen,
        kTruncated,
        kBadMagic,
        kBadVersion,
        kBadCorpus,
        kBadConfig,
        kBadHash,
        kBadTag,
        kDuplicate,
        kIncomplete,
        kBadLength,
        kWrite,
        kHostWait,
        kBusy,
        kDangling,
    };
    Code code{kOk};
    std::string message;
};

inline constexpr std::uint32_t kSnapshotVersion = 1u;
inline constexpr std::size_t kSnapshotCorpusHashSize = 32u;
inline constexpr std::uint32_t kSnapshotRamSize = 32u * 1024u * 1024u;
inline constexpr std::uint32_t kSnapshotVramSize = 2u * 1024u * 1024u;

inline constexpr std::uint32_t kTagRam = 1u;
inline constexpr std::uint32_t kTagVram = 2u;
inline constexpr std::uint32_t kTagCpu = 3u;
inline constexpr std::uint32_t kTagClock = 4u;
inline constexpr std::uint32_t kTagKids = 5u;
inline constexpr std::uint32_t kTagThreads = 6u;
inline constexpr std::uint32_t kTagSema = 7u;
inline constexpr std::uint32_t kTagEvent = 8u;
inline constexpr std::uint32_t kTagMutex = 9u;
inline constexpr std::uint32_t kTagVpl = 10u;
inline constexpr std::uint32_t kTagFpl = 11u;
inline constexpr std::uint32_t kTagMsgPipe = 12u;
inline constexpr std::uint32_t kTagCallback = 13u;
inline constexpr std::uint32_t kTagVTimer = 14u;
inline constexpr std::uint32_t kTagMemBlocks = 15u;
inline constexpr std::uint32_t kTagSubInt = 16u;
inline constexpr std::uint32_t kTagFds = 17u;
inline constexpr std::uint32_t kTagAudio = 18u;
inline constexpr std::uint32_t kTagSas = 19u;
inline constexpr std::uint32_t kTagAtrac = 20u;
inline constexpr std::uint32_t kTagGe = 21u;
inline constexpr std::uint32_t kTagModules = 22u;
inline constexpr std::uint32_t kTagUtility = 23u;
inline constexpr std::uint32_t kFullSectionCount = 23u;

// Shared context codec (848 B): reused by per-thread saved contexts.
void encode_context(const psprecomp::AllegrexContext &cpu, std::vector<std::uint8_t> &out);
bool decode_context(std::span<const std::uint8_t> payload, psprecomp::AllegrexContext &cpu, SnapshotError &error);

bool snapshot_save(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &corpus_hash,
    const std::array<std::uint8_t, 32> &config_hash, const psprecomp::GuestMemory &memory,
    const psprecomp::AllegrexContext &cpu, const ClockState &clock, SnapshotError &error);

bool snapshot_load(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &expected_corpus,
    const std::array<std::uint8_t, 32> &expected_config, psprecomp::GuestMemory &memory,
    psprecomp::AllegrexContext &cpu, ClockState &clock, SnapshotError &error);

bool snapshot_save_full(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &corpus_hash,
    const std::array<std::uint8_t, 32> &config_hash, const psprecomp::GuestMemory &memory,
    const psprecomp::AllegrexContext &cpu, const ClockState &clock, const hle::Kernel &kernel,
    SnapshotError &error);

bool snapshot_load_full(const std::filesystem::path &path, const std::array<std::uint8_t, 32> &expected_corpus,
    const std::array<std::uint8_t, 32> &expected_config, psprecomp::GuestMemory &memory,
    psprecomp::AllegrexContext &cpu, ClockState &clock, hle::Kernel &kernel, SnapshotError &error);

} // namespace gungnir::session

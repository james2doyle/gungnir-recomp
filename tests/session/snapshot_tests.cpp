// T15a slice 1 — container + RAM/EDRAM + CPU + clock (red first).
//
// Usage: snapshot_tests
// Exit 0 on the asserted contract, 1 on a failed contract.

#include "session/snapshot.hpp"

#include "hle/hle_common.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using gungnir::session::ClockState;
using gungnir::session::SnapshotError;

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::array<std::uint8_t, 32> filled(std::uint8_t value) {
    std::array<std::uint8_t, 32> out{};
    out.fill(value);
    return out;
}

void fill_memory(psprecomp::GuestMemory &memory) {
    for (std::uint32_t i = 0; i < memory.size(); i += 4097u)
        memory.store8(0x08000000u + i, static_cast<std::uint8_t>((i >> 7u) & 0xFFu));
    for (std::uint32_t i = 0; i < memory.vram_size(); i += 511u)
        memory.store8(0x04000000u + i, static_cast<std::uint8_t>((i * 31u) & 0xFFu));
}

void fill_cpu(psprecomp::AllegrexContext &cpu) {
    for (std::size_t i = 0; i < cpu.gpr.size(); ++i) cpu.gpr[i] = 0x1000u + static_cast<std::uint32_t>(i);
    cpu.hi = 0x11111111u;
    cpu.lo = 0x22222222u;
    cpu.pc = 0x08804000u;
    for (std::size_t i = 0; i < cpu.fpr.size(); ++i) cpu.fpr[i] = static_cast<float>(i) * 1.5f;
    cpu.fcr31 = 0x01000000u;
    for (std::size_t i = 0; i < cpu.vfpu.size(); ++i) cpu.vfpu[i] = static_cast<float>(i) * 0.25f;
    for (std::size_t i = 0; i < cpu.vfpu_ctrl.size(); ++i)
        cpu.vfpu_ctrl[i] = 0xE4E4E4E4u ^ static_cast<std::uint32_t>(i);
}

std::uint64_t ram_checksum(const psprecomp::GuestMemory &memory) {
    std::uint64_t sum = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < memory.size(); i += 1024u) {
        sum ^= memory.load8(0x08000000u + i);
        sum *= 1099511628211ull;
    }
    for (std::uint32_t i = 0; i < memory.vram_size(); i += 512u) {
        sum ^= memory.load8(0x04000000u + i);
        sum *= 1099511628211ull;
    }
    return sum;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "gungnir_snapshot_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "slice1.gsnap";

    const auto corpus = filled(0x11u);
    const auto config = filled(0x22u);

    psprecomp::GuestMemory memory;
    psprecomp::AllegrexContext cpu;
    fill_memory(memory);
    fill_cpu(cpu);
    const ClockState clock{123456789u, 16666u * 7u, 42u, gungnir::hle::wall_base_us()};
    const std::uint64_t before = ram_checksum(memory);

    SnapshotError error{};
    check(gungnir::session::snapshot_save(path, corpus, config, memory, cpu, clock, error),
        "save ok: " + error.message);
    check(std::filesystem::exists(path), "snapshot file exists");

    const std::vector<std::uint8_t> first = read_file(path);
    check(!first.empty(), "snapshot non-empty");

    check(gungnir::session::snapshot_save(path, corpus, config, memory, cpu, clock, error),
        "resave ok: " + error.message);
    const std::vector<std::uint8_t> second = read_file(path);
    check(first == second, "two snapshots at same state are byte-identical");

    psprecomp::GuestMemory restored;
    psprecomp::AllegrexContext restored_cpu;
    ClockState restored_clock{};
    check(gungnir::session::snapshot_load(path, corpus, config, restored, restored_cpu, restored_clock, error),
        "load ok: " + error.message);
    check(ram_checksum(restored) == before, "RAM/EDRAM round trip checksum");
    check(restored_cpu.pc == cpu.pc && restored_cpu.hi == cpu.hi && restored_cpu.lo == cpu.lo,
        "CPU pc/hi/lo round trip");
    check(restored_cpu.gpr == cpu.gpr, "CPU gpr round trip");
    check(restored_cpu.fcr31 == cpu.fcr31, "CPU fcr31 round trip");
    check(restored_clock.now_us == clock.now_us && restored_clock.vblank_count == clock.vblank_count &&
              restored_clock.next_vblank_us == clock.next_vblank_us,
        "clock round trip preserves absolute deadline");

    // T15d divergence root cause: guest-visible wall time (sceRtcGetCurrentTick
    // = wall base + virtual now) must survive a process restart. The wall base
    // is host boot time; without it a restore in a new process shifts every
    // tick by the inter-process gap and the guest takes a different branch.
    {
        const std::uint64_t base_before = gungnir::hle::wall_base_us();
        check(base_before != 0u, "wall base is populated");
        const std::uint64_t tick_before = base_before + clock.now_us;
        // Simulate a new host process: the boot-time base moves forward.
        gungnir::hle::set_wall_base_us(base_before + 600'000'000u);
        check(gungnir::hle::wall_base_us() != base_before, "wall base override takes effect");
        psprecomp::GuestMemory reloaded;
        psprecomp::AllegrexContext reloaded_cpu;
        ClockState reloaded_clock{};
        check(gungnir::session::snapshot_load(path, corpus, config, reloaded, reloaded_cpu, reloaded_clock, error),
            "reload ok: " + error.message);
        check(reloaded_clock.wall_base_us == base_before, "wall base restored from snapshot");
        gungnir::hle::set_wall_base_us(reloaded_clock.wall_base_us);
        check(gungnir::hle::wall_base_us() + reloaded_clock.now_us == tick_before,
            "guest tick continuous across simulated process restart");
    }

    // Scheduler phase continuity (T15d): the Runtime starvation-boundary
    // counter is process state, so a restore in a fresh Runtime would shift
    // every future preemption boundary and the thread interleaving with it.
    {
        const ClockState phased{7u, 16666u, 3u, gungnir::hle::wall_base_us(), 19999u};
        const std::filesystem::path phased_path = dir / "phased.gsnap";
        check(gungnir::session::snapshot_save(phased_path, corpus, config, memory, cpu, phased, error),
            "phased save ok: " + error.message);
        psprecomp::GuestMemory phased_mem;
        psprecomp::AllegrexContext phased_cpu;
        ClockState phased_back{};
        check(gungnir::session::snapshot_load(phased_path, corpus, config, phased_mem, phased_cpu, phased_back, error),
            "phased load ok: " + error.message);
        check(phased_back.dispatch_phase == 19999u, "dispatch phase round trips");
    }

    // Legacy files with the 24-byte clock section (pre-wall-base) still load;
    // the loader keeps the current wall base instead of failing.
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> legacy((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto get_u32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(legacy[at]) | (static_cast<std::uint32_t>(legacy[at + 1]) << 8u) |
                (static_cast<std::uint32_t>(legacy[at + 2]) << 16u) |
                (static_cast<std::uint32_t>(legacy[at + 3]) << 24u);
        };
        std::size_t offset = 76u;
        bool found = false;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t tag = get_u32(offset);
            const std::uint32_t length = get_u32(offset + 4);
            if (tag == 4u) {
                check(length == 40u, "new clock section is 40 bytes");
                // Craft the 24-byte original legacy: drop wall base + phase.
                legacy.erase(legacy.begin() + static_cast<std::ptrdiff_t>(offset + 8 + 24),
                    legacy.begin() + static_cast<std::ptrdiff_t>(offset + 8 + 40));
                legacy[offset + 4] = 24u;
                legacy[offset + 5] = 0u;
                legacy[offset + 6] = 0u;
                legacy[offset + 7] = 0u;
                found = true;
                break;
            }
            offset += 8u + length;
        }
        check(found, "clock section located for legacy rewrite");
        auto fnv1a64 = [](const std::vector<std::uint8_t> &bytes) {
            std::uint64_t hash = 1469598103934665603ull;
            for (std::uint8_t b : bytes) {
                hash ^= b;
                hash *= 1099511628211ull;
            }
            return hash;
        };
        legacy.resize(legacy.size() - 8u);
        const std::uint64_t hash = fnv1a64(legacy);
        for (int i = 0; i < 8; ++i)
            legacy.push_back(static_cast<std::uint8_t>((hash >> (8u * i)) & 0xFFu));
        const std::filesystem::path legacy_path = dir / "legacy.gsnap";
        std::ofstream out(legacy_path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(legacy.data()), static_cast<std::streamsize>(legacy.size()));
        out.close();
        psprecomp::GuestMemory legacy_mem;
        psprecomp::AllegrexContext legacy_cpu;
        ClockState legacy_clock{};
        const std::uint64_t current_base = gungnir::hle::wall_base_us();
        check(gungnir::session::snapshot_load(legacy_path, corpus, config, legacy_mem, legacy_cpu, legacy_clock, error),
            "legacy clock load ok: " + error.message);
        check(legacy_clock.wall_base_us == 0u, "legacy clock reports no stored wall base");
        check(legacy_clock.dispatch_phase == 0u, "legacy clock reports no stored phase");
        check(ram_checksum(legacy_mem) == before, "legacy RAM round trip checksum");
    }

    {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> truncated((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        truncated.resize(truncated.size() / 2u);
        const std::filesystem::path bad = dir / "truncated.gsnap";
        std::ofstream out(bad, std::ios::binary);
        out.write(reinterpret_cast<const char *>(truncated.data()), static_cast<std::streamsize>(truncated.size()));
        out.close();
        const std::uint64_t untouched = ram_checksum(restored);
        psprecomp::GuestMemory target;
        psprecomp::AllegrexContext target_cpu;
        ClockState target_clock{};
        fill_memory(target);
        const std::uint64_t target_before = ram_checksum(target);
        check(!gungnir::session::snapshot_load(bad, corpus, config, target, target_cpu, target_clock, error),
            "truncated load fails");
        check(!error.message.empty(), "truncated load names an error");
        check(ram_checksum(target) == target_before, "truncated load leaves session untouched");
        check(ram_checksum(restored) == untouched, "previous session untouched");
    }

    {
        std::vector<std::uint8_t> flipped = first;
        flipped[flipped.size() / 2u] ^= 0x01u;
        const std::filesystem::path bad = dir / "flipped.gsnap";
        std::ofstream out(bad, std::ios::binary);
        out.write(reinterpret_cast<const char *>(flipped.data()), static_cast<std::streamsize>(flipped.size()));
        out.close();
        psprecomp::GuestMemory target;
        psprecomp::AllegrexContext target_cpu;
        ClockState target_clock{};
        fill_memory(target);
        const std::uint64_t target_before = ram_checksum(target);
        check(!gungnir::session::snapshot_load(bad, corpus, config, target, target_cpu, target_clock, error),
            "flipped payload byte fails");
        check(ram_checksum(target) == target_before, "flipped load leaves session untouched");
    }

    {
        psprecomp::GuestMemory target;
        psprecomp::AllegrexContext target_cpu;
        ClockState target_clock{};
        check(!gungnir::session::snapshot_load(path, filled(0x99u), config, target, target_cpu, target_clock, error),
            "wrong corpus hash fails");
        check(error.code == SnapshotError::kBadCorpus, "wrong corpus names snapshot_bad_corpus");
    }

    {
        std::vector<std::uint8_t> wrong_version = first;
        // version is bytes 4..7 LE; bump it.
        wrong_version[4] = 0x7Fu;
        const std::filesystem::path bad = dir / "bad_version.gsnap";
        std::ofstream out(bad, std::ios::binary);
        out.write(
            reinterpret_cast<const char *>(wrong_version.data()), static_cast<std::streamsize>(wrong_version.size()));
        out.close();
        psprecomp::GuestMemory target;
        psprecomp::AllegrexContext target_cpu;
        ClockState target_clock{};
        check(!gungnir::session::snapshot_load(bad, corpus, config, target, target_cpu, target_clock, error),
            "wrong version fails");
        check(error.code == SnapshotError::kBadVersion, "wrong version names snapshot_bad_version");
    }

    if (failures != 0) {
        std::cerr << "snapshot_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "snapshot_tests: OK\n";
    return 0;
}

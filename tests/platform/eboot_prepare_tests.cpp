// Synthetic vectors for on-device ~PSP EBOOT preparation (Yakumo-style).
// Every fixture is hand-authored bytes; no game data is read. The encryptor
// below inverts gungnir::platform::prepare_executable: it builds a minimal
// 0x150-byte header whose wrapped payload key unwraps to a chosen key, and
// a payload encrypted under that key.

#include "platform/android/eboot_prepare.hpp"

#include "tiny-aes-c/aes.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

using Block = std::array<std::uint8_t, 16>;

constexpr Block kTagKey = {
    0xEB, 0xFF, 0x40, 0xD8, 0xB4, 0x1A, 0xE1, 0x66, 0x91, 0x3B, 0x8F, 0x64, 0xB6, 0xFC, 0xB7, 0x12};
constexpr Block kHeaderKey = {
    0x11, 0x5A, 0x5D, 0x20, 0xD5, 0x3A, 0x8D, 0xD3, 0x9C, 0xC5, 0xAF, 0x41, 0x0F, 0x0F, 0x18, 0x6F};
constexpr Block kPayloadWrappingKey = {
    0x98, 0xC9, 0x40, 0x97, 0x5C, 0x1D, 0x10, 0xE8, 0x7F, 0xE6, 0x0E, 0xA3, 0xFD, 0x03, 0xA8, 0xBA};
constexpr std::uint32_t kSupportedTag = 0xD91613F0u;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void cbc_encrypt(const Block &key, std::vector<std::uint8_t> &data) {
    const Block zero_iv{};
    AES_ctx context{};
    AES_init_ctx_iv(&context, key.data(), zero_iv.data());
    AES_CBC_encrypt_buffer(&context, data.data(), data.size());
}

void cbc_decrypt_for_mask(std::vector<std::uint8_t> &data) {
    const Block zero_iv{};
    AES_ctx context{};
    AES_init_ctx_iv(&context, kHeaderKey.data(), zero_iv.data());
    AES_CBC_decrypt_buffer(&context, data.data(), data.size());
}

std::vector<std::uint8_t> derive_mask_enc() {
    std::vector<std::uint8_t> mask;
    for (std::uint8_t index = 0; index < 9u; ++index) {
        Block block = kTagKey;
        block[0] = index;
        mask.insert(mask.end(), block.begin(), block.end());
    }
    cbc_decrypt_for_mask(mask);
    return mask;
}

void write_le32(std::vector<std::uint8_t> &v, std::size_t off, std::uint32_t value) {
    v[off] = value & 0xFFu;
    v[off + 1] = (value >> 8) & 0xFFu;
    v[off + 2] = (value >> 16) & 0xFFu;
    v[off + 3] = (value >> 24) & 0xFFu;
}

// Build an EBOOT.BIN that prepare_executable decrypts to `plain` (ELF bytes).
std::vector<std::uint8_t> build_eboot(std::span<const std::uint8_t> plain, const Block &payload_key) {
    const std::uint32_t size = static_cast<std::uint32_t>(plain.size());
    const std::size_t padded = (plain.size() + 15u) & ~std::size_t{15u};

    // Payload: pad + CBC-encrypt under payload_key.
    std::vector<std::uint8_t> payload(padded, 0u);
    std::memcpy(payload.data(), plain.data(), plain.size());
    cbc_encrypt(payload_key, payload);

    // Header record: R holds the wrapped key at 0x24. Invert the unwrap:
    // K -> enc(wrap) -> xor mask[0x50] -> enc(header) -> xor mask[0x10] = R[0x24].
    const std::vector<std::uint8_t> mask = derive_mask_enc();
    std::vector<std::uint8_t> step(payload_key.begin(), payload_key.end());
    cbc_encrypt(kPayloadWrappingKey, step); // single block, zero IV
    for (int i = 0; i < 16; ++i) step[i] ^= mask[0x50 + i];
    cbc_encrypt(kHeaderKey, step);
    for (int i = 0; i < 16; ++i) step[i] ^= mask[0x10 + i];
    std::vector<std::uint8_t> record(0x60, 0u);
    std::memcpy(record.data() + 0x24, step.data(), 16);
    cbc_encrypt(kHeaderKey, record); // matches cbc_decrypt in unwrap

    std::vector<std::uint8_t> header(0x150, 0u);
    std::memcpy(header.data() + 0x00, "~PSP", 4);
    header[0x06] = 0; // attributes, bit0 clear
    write_le32(header, 0x28, size);
    header[0x7C] = 9; // UMD game executable
    write_le32(header, 0xB0, size);
    write_le32(header, 0xD0, kSupportedTag);
    // Scatter the 0x60 record back to its four fields.
    std::memcpy(header.data() + 0x140, record.data() + 0x00, 0x10);
    std::memcpy(header.data() + 0x12C, record.data() + 0x10, 0x14);
    std::memcpy(header.data() + 0x80, record.data() + 0x24, 0x30);
    std::memcpy(header.data() + 0xC0, record.data() + 0x54, 0x0C);

    std::vector<std::uint8_t> eboot = header;
    eboot.insert(eboot.end(), payload.begin(), payload.end());
    return eboot;
}

Block test_key() {
    return Block{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
}

std::vector<std::uint8_t> elf_plain(std::size_t size) {
    std::vector<std::uint8_t> elf(size, 0u);
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    for (std::size_t i = 4; i < size; ++i) elf[i] = static_cast<std::uint8_t>(i & 0xFFu);
    return elf;
}

void round_trip() {
    const auto plain = elf_plain(4096);
    const auto eboot = build_eboot(plain, test_key());
    const auto out = gungnir::platform::prepare_executable(eboot);
    check(out == plain, "synthetic EBOOT round-trips to its ELF");
    const auto odd = elf_plain(4097);
    const auto eboot_odd = build_eboot(odd, test_key());
    const auto out_odd = gungnir::platform::prepare_executable(eboot_odd);
    check(out_odd == odd, "unpadded odd size round-trips");
    int calls = 0;
    (void)gungnir::platform::prepare_executable(eboot, [&](std::uint64_t, std::uint64_t) { ++calls; });
    check(calls >= 1, "progress is reported");
}

void rejects() {
    const auto plain = elf_plain(2048);
    const auto eboot = build_eboot(plain, test_key());

    std::vector<std::uint8_t> bad_magic = eboot;
    bad_magic[0] = 'X';
    bool threw = false;
    try { (void)gungnir::platform::prepare_executable(bad_magic); } catch (...) { threw = true; }
    check(threw, "bad magic is rejected");

    std::vector<std::uint8_t> bad_tag = eboot;
    bad_tag[0xD0] ^= 0xFFu;
    threw = false;
    try { (void)gungnir::platform::prepare_executable(bad_tag); } catch (...) { threw = true; }
    check(threw, "unknown tag is rejected");

    std::vector<std::uint8_t> short_header(eboot.begin(), eboot.begin() + 0x40);
    threw = false;
    try { (void)gungnir::platform::prepare_executable(short_header); } catch (...) { threw = true; }
    check(threw, "truncated header is rejected");

    // Valid header but payload that decrypts to non-ELF.
    std::vector<std::uint8_t> non_elf(1024, 0xAAu);
    const auto bad = build_eboot(non_elf, test_key());
    threw = false;
    try { (void)gungnir::platform::prepare_executable(bad); } catch (...) { threw = true; }
    check(threw, "non-ELF payload is rejected");

    // Tampered payload must not silently produce the original ELF (key wrong).
    std::vector<std::uint8_t> tampered = eboot;
    tampered[0x150] ^= 0xFFu;
    bool differs_or_throws = false;
    try {
        const auto out = gungnir::platform::prepare_executable(tampered);
        differs_or_throws = (out != plain);
    } catch (...) { differs_or_throws = true; }
    check(differs_or_throws, "tampered payload does not round-trip");
}

} // namespace

int main() {
    try {
        round_trip();
        rejects();
        std::cout << "eboot_prepare_tests: OK\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}

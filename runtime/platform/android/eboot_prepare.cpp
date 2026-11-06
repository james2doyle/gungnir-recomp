// Prepares the game's executable from the encrypted EBOOT.BIN on the user's
// own disc. Written for this project from public descriptions of the PSP
// executable header ("~PSP") and of the PSP's AES-based crypto engine;
// behaviour ported from Yakumo's executable_preparation (see
// docs/SOURCE_PROVENANCE.md reference log). The file is an 0x150-byte header
// followed by the executable, encrypted with AES-128 in CBC mode under a
// per-file key. The header keeps that key wrapped twice: under the engine's
// fixed decryption key, and inside header fields that are encrypted and
// masked with material derived from the key its tag selects.
//
// Only one tag is supported: 0xD91613F0, the tag on the Gungnir USA disc.
// Its tag key is the public PSP key-table entry for that tag (the same table
// pspdecrypt ships; Yakumo's table holds the neighbouring 0xD9160BF0 entry
// for its own title). The header key is KIRK slot 0x5D and the payload
// wrapping key is KIRK1, identical for every title.

#include "platform/android/eboot_prepare.hpp"

#include "psprecomp/common.hpp"

#include "tiny-aes-c/aes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace gungnir::platform {
namespace {

using Block = std::array<std::uint8_t, 16>;

constexpr std::size_t kHeaderSize = 0x150u;

// Header fields used here.
constexpr std::size_t kMagicOffset = 0x00u;       // "~PSP"
constexpr std::size_t kAttributesOffset = 0x06u;  // bit 0 set: compressed
constexpr std::size_t kImageSizeOffset = 0x28u;   // size of the decrypted image
constexpr std::size_t kFileTypeOffset = 0x7Cu;    // 9: UMD game executable
constexpr std::size_t kPayloadSizeOffset = 0xB0u; // size of the encrypted payload
constexpr std::size_t kTagOffset = 0xD0u;

constexpr std::uint32_t kSupportedTag = 0xD91613F0u;
constexpr std::uint8_t kUmdGameExecutable = 9u;

// Key selected by kSupportedTag, from the public PSP key tables (pspdecrypt
// PrxDecrypter key_D91613F0, the same table Yakumo uses for its own tag).
constexpr Block kTagKey = {
    0xEB, 0xFF, 0x40, 0xD8, 0xB4, 0x1A, 0xE1, 0x66, 0x91, 0x3B, 0x8F, 0x64, 0xB6, 0xFC, 0xB7, 0x12};
// Crypto-engine key slot 0x5D (plain AES decryption, command 7), which this tag
// uses for the header (libkirk kirk_4_7 key for 0x5D).
constexpr Block kHeaderKey = {
    0x11, 0x5A, 0x5D, 0x20, 0xD5, 0x3A, 0x8D, 0xD3, 0x9C, 0xC5, 0xAF, 0x41, 0x0F, 0x0F, 0x18, 0x6F};
// The engine's fixed key for signed and encrypted blocks (command 1, KIRK1),
// which wraps the payload key.
constexpr Block kPayloadWrappingKey = {
    0x98, 0xC9, 0x40, 0x97, 0x5C, 0x1D, 0x10, 0xE8, 0x7F, 0xE6, 0x0E, 0xA3, 0xFD, 0x03, 0xA8, 0xBA};

std::uint32_t read_le32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(data[offset + 2u]) << 16u) | (static_cast<std::uint32_t>(data[offset + 3u]) << 24u);
}

// AES-128-CBC decryption with a zero IV, in place. The length must be a
// multiple of the block size.
void cbc_decrypt(const Block &key, std::span<std::uint8_t> data) {
    const Block zero_iv{};
    AES_ctx context{};
    AES_init_ctx_iv(&context, key.data(), zero_iv.data());
    AES_CBC_decrypt_buffer(&context, data.data(), data.size());
}

Block block_at(std::span<const std::uint8_t> data, std::size_t offset) {
    Block block{};
    std::memcpy(block.data(), data.data() + offset, block.size());
    return block;
}

void xor_into(Block &target, std::span<const std::uint8_t> mask) {
    for (std::size_t i = 0; i < target.size(); ++i) target[i] ^= mask[i];
}

void append(
    std::vector<std::uint8_t> &out, std::span<const std::uint8_t> header, std::size_t offset, std::size_t size) {
    out.insert(out.end(), header.begin() + static_cast<std::ptrdiff_t>(offset),
        header.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

// The tag key, repeated over nine blocks that each carry their own index in
// their first byte, decrypted with the header key.
std::vector<std::uint8_t> derive_mask() {
    std::vector<std::uint8_t> mask;
    for (std::uint8_t index = 0; index < 9u; ++index) {
        Block block = kTagKey;
        block[0] = index;
        mask.insert(mask.end(), block.begin(), block.end());
    }
    cbc_decrypt(kHeaderKey, mask);
    return mask;
}

// Recovers the payload key from the header.
Block unwrap_payload_key(std::span<const std::uint8_t> header) {
    // Four header fields form one 0x60-byte encrypted record. Decrypted, it
    // holds a check value and the header's hash, followed from offset 0x24 by
    // the payload key still wrapped under the fixed key and masked.
    std::vector<std::uint8_t> record;
    append(record, header, 0x140u, 0x10u);
    append(record, header, 0x12Cu, 0x14u);
    append(record, header, 0x80u, 0x30u);
    append(record, header, 0xC0u, 0x0Cu);
    cbc_decrypt(kHeaderKey, record);

    const std::vector<std::uint8_t> mask = derive_mask();
    Block key = block_at(record, 0x24u);
    xor_into(key, std::span(mask).subspan(0x10u, 16u));
    cbc_decrypt(kHeaderKey, key);
    xor_into(key, std::span(mask).subspan(0x50u, 16u));
    cbc_decrypt(kPayloadWrappingKey, key);
    return key;
}

} // namespace

std::vector<std::uint8_t> prepare_executable(
    std::span<const std::uint8_t> eboot_bin, const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
    // The disc identity (size+CRC + DISC_ID) is checked by the caller; these
    // checks document the one layout handled here.
    if (eboot_bin.size() < kHeaderSize || std::memcmp(eboot_bin.data() + kMagicOffset, "~PSP", 4u) != 0 ||
        read_le32(eboot_bin, kTagOffset) != kSupportedTag || eboot_bin[kFileTypeOffset] != kUmdGameExecutable ||
        (eboot_bin[kAttributesOffset] & 1u) != 0u)
        throw psprecomp::Error("EBOOT.BIN has an unexpected header");
    const std::uint32_t size = read_le32(eboot_bin, kPayloadSizeOffset);
    const std::size_t padded_size = (static_cast<std::size_t>(size) + 15u) & ~std::size_t{15u};
    if (size == 0u || size != read_le32(eboot_bin, kImageSizeOffset) || padded_size > eboot_bin.size() - kHeaderSize)
        throw psprecomp::Error("EBOOT.BIN has an unexpected payload size");

    const Block key = unwrap_payload_key(eboot_bin.first(kHeaderSize));
    std::vector<std::uint8_t> executable(
        eboot_bin.begin() + kHeaderSize, eboot_bin.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + padded_size));
    // CBC carries its chaining value in the context, so the payload decrypts
    // in slices that report progress in between.
    constexpr std::size_t kSlice = 1024u * 1024u;
    const Block zero_iv{};
    AES_ctx context{};
    AES_init_ctx_iv(&context, key.data(), zero_iv.data());
    for (std::size_t offset = 0; offset < executable.size(); offset += kSlice) {
        const std::size_t length = std::min(kSlice, executable.size() - offset);
        AES_CBC_decrypt_buffer(&context, executable.data() + offset, length);
        if (progress) progress(offset + length, executable.size());
    }
    executable.resize(size);

    // The decrypted image must be an ELF; anything else means the unwrap
    // produced the wrong key (wrong tag) or a corrupt header. No dump-hash
    // check: hashes pin a specific rip and stay in gitignored local profiles
    // (docs/LEGAL.md §5). The caller already pinned the disc by size+CRC and
    // DISC_ID, so an ELF of the header size is the identity.
    if (executable.size() < 4u || executable[0] != 0x7Fu || executable[1] != 'E' || executable[2] != 'L' ||
        executable[3] != 'F')
        throw psprecomp::Error("The prepared executable is not an ELF");
    return executable;
}

} // namespace gungnir::platform

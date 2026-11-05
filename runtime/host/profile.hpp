#ifndef GUNGNIR_HOST_PROFILE_HPP
#define GUNGNIR_HOST_PROFILE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gungnir::host {

// Per-title host patches: explicit, named, opt-in values live in the
// gitignored profiles/<title>/local/ directory. They are host state, never
// guest state: they are not part of snapshot identity and are never written
// into a snapshot. An unvalidated value is refused, never best-efforted.
//
// Reference (behavioural only, recorded in docs/SOURCE_PROVENANCE.md):
// BasedYuki/the-3rd-birthday-recomp stores named hook addresses in a
// versions/<title>.toml and applies them by matching guest addresses. That
// shape - named typed entries, explicit opt-in, no silent defaults - is what
// this type provides; no code is copied.

enum class ProfileError {
    kNone,
    kUnknownKey,
    kMalformedValue,
    kTypeMismatch,
    kDuplicateKey,
};

class ProfileValue {
public:
    enum class Kind { kU32, kFloat };

    ProfileValue() = default;
    explicit ProfileValue(std::uint32_t value) : kind_(Kind::kU32), u32_(value) {}
    explicit ProfileValue(float value) : kind_(Kind::kFloat), float_(value) {}

    Kind kind() const noexcept { return kind_; }
    std::uint32_t as_u32() const noexcept { return u32_; }
    float as_float() const noexcept { return float_; }

    std::string render() const;

private:
    Kind kind_{Kind::kU32};
    std::uint32_t u32_{};
    float float_{};
};

struct ProfileResult;

class HostProfile {
public:
    // Parse `text` (one `key = value` per line, # comments, blank lines).
    // Every recognised key has a fixed type; unknown keys, malformed values,
    // type mismatches and duplicates are named errors. Nothing is ever
    // silently defaulted into existence.
    static ProfileResult parse(std::string_view text);

    std::size_t size() const noexcept { return entries_.size(); }
    const ProfileValue *lookup(std::string_view key) const noexcept;

    // One "profile key=value" line per applied entry, for the boot log.
    std::vector<std::string> applied_lines() const;

    // Host state: a profile contributes no snapshot bytes.
    std::string snapshot_payload() const { return {}; }
    bool snapshotted() const noexcept { return false; }

private:
    friend struct ProfileResult;
    void insert(std::string key, ProfileValue value);
    std::unordered_map<std::string, ProfileValue> entries_;
};

struct ProfileResult {
    bool ok() const noexcept { return error == ProfileError::kNone; }
    ProfileError error{ProfileError::kNone};
    std::string message;
    HostProfile profile;
};

}  // namespace gungnir::host

#endif  // GUNGNIR_HOST_PROFILE_HPP

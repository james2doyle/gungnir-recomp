// 75yfmuis — per-title profile surface for host patches.
//
// The mechanism the camera/matrix hooks plug into. Explicit, named, opt-in
// values with typed parsing. The rules that break naively:
//   - an unknown key is a named error, never a silently ignored line;
//   - a malformed value is a named error, never a best-effort default;
//   - a recognised-but-invalid entry must not be defaulted into existence;
//   - an absent profile changes nothing (empty, no error);
//   - lookups are typed (a u32 address is not a float FOV).
//
// Red-first: written before runtime/host/profile.* existed.

#include "host/profile.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using gungnir::host::HostProfile;
using gungnir::host::ProfileError;
using gungnir::host::ProfileResult;
using gungnir::host::ProfileValue;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void a_named_u32_entry_parses() {
    const std::string text =
        "cam_projection = 0x08BBB194\n"
        "cam_setup = 0x08B435B8\n";
    ProfileResult result = HostProfile::parse(text);
    check(result.ok(), "a well-formed profile parses");
    check(result.error == ProfileError::kNone, "no error on a well-formed profile");
    check(result.profile.size() == 2u, "both named entries are kept");
    const ProfileValue *address = result.profile.lookup("cam_projection");
    check(address != nullptr, "cam_projection is present");
    check(address != nullptr && address->kind() == ProfileValue::Kind::kU32,
        "an address is a u32, not a float or string");
    check(address != nullptr && address->as_u32() == 0x08BBB194u,
        "the address value survives parsing exactly");
}

void a_named_float_entry_parses() {
    const std::string text = "widescreen_fov_scale = 1.3333\n";
    ProfileResult result = HostProfile::parse(text);
    check(result.ok(), "a float-valued profile parses");
    const ProfileValue *value = result.profile.lookup("widescreen_fov_scale");
    check(value != nullptr && value->kind() == ProfileValue::Kind::kFloat,
        "a scale is a float");
    check(value != nullptr && value->as_float() > 1.3332f &&
        value->as_float() < 1.3334f, "the float value survives parsing");
}

void an_unknown_key_is_a_named_error() {
    const std::string text =
        "cam_projection = 0x08BBB194\n"
        "turboboost = 1\n";
    ProfileResult result = HostProfile::parse(text);
    check(!result.ok(), "an unknown key is refused, not ignored");
    check(result.error == ProfileError::kUnknownKey,
        "the failure is named kUnknownKey");
    check(result.message.find("turboboost") != std::string::npos,
        "the error names the offending key");
}

void a_malformed_value_is_a_named_error() {
    const std::string text = "cam_projection = 0xZZZ\n";
    ProfileResult result = HostProfile::parse(text);
    check(!result.ok(), "a malformed value is refused, not best-efforted");
    check(result.error == ProfileError::kMalformedValue,
        "the failure is named kMalformedValue");
    check(result.message.find("cam_projection") != std::string::npos,
        "the error names the offending entry");
}

void a_recognised_but_invalid_entry_is_not_defaulted() {
    const std::string text = "cam_projection =\n";
    ProfileResult result = HostProfile::parse(text);
    check(!result.ok(), "an empty value for a recognised key is refused");
    check(result.error == ProfileError::kMalformedValue,
        "the failure is named kMalformedValue");
    check(result.profile.size() == 0u,
        "no entry is silently defaulted into the profile");
}

void a_float_in_a_u32_slot_is_refused() {
    const std::string text = "cam_projection = 1.5\n";
    ProfileResult result = HostProfile::parse(text);
    check(!result.ok(), "a float in an address slot is refused");
    check(result.error == ProfileError::kTypeMismatch,
        "the failure is named kTypeMismatch, not a silent coercion");
}

void comments_and_blank_lines_are_accepted() {
    const std::string text =
        "# per-title host patches\n"
        "\n"
        "cam_projection = 0x08BBB194  # hook site\n"
        "\n";
    ProfileResult result = HostProfile::parse(text);
    check(result.ok(), "comments and blank lines parse");
    check(result.profile.size() == 1u, "the comment contributes no entry");
}

void a_duplicate_key_is_a_named_error() {
    const std::string text =
        "cam_projection = 0x08BBB194\n"
        "cam_projection = 0x08BBB195\n";
    ProfileResult result = HostProfile::parse(text);
    check(!result.ok(), "a duplicate key is refused");
    check(result.error == ProfileError::kDuplicateKey,
        "the failure is named kDuplicateKey");
}

void an_absent_profile_changes_nothing() {
    ProfileResult empty = HostProfile::parse("");
    check(empty.ok(), "an absent/empty profile is not an error");
    check(empty.profile.size() == 0u, "an absent profile has no entries");
    check(empty.profile.lookup("cam_projection") == nullptr,
        "an absent profile has no lookupable entries");
    check(empty.profile.applied_lines().empty(),
        "an absent profile logs nothing as applied");
}

void applied_lines_name_every_entry() {
    const std::string text =
        "cam_projection = 0x08BBB194\n"
        "cam_setup = 0x08B435B8\n";
    ProfileResult result = HostProfile::parse(text);
    check(result.ok(), "profile parses");
    const std::vector<std::string> lines = result.profile.applied_lines();
    check(lines.size() == 2u, "one applied line per entry");
    bool saw_projection = false;
    bool saw_setup = false;
    for (const std::string &line : lines) {
        check(line.rfind("profile ", 0) == 0,
            "each applied line is prefixed 'profile '");
        if (line.find("cam_projection=") != std::string::npos) saw_projection = true;
        if (line.find("cam_setup=") != std::string::npos) saw_setup = true;
    }
    check(saw_projection && saw_setup,
        "every applied entry is named in the applied lines");
}

void a_change_to_the_profile_never_enters_the_snapshot() {
    // The profile is host state: it must not be part of snapshot identity.
    // Pinned here as a source-level property of the stored snapshot fields.
    const std::string text = "cam_projection = 0x08BBB194\n";
    ProfileResult result = HostProfile::parse(text);
    check(result.ok(), "profile parses");
    check(result.profile.snapshot_payload().empty(),
        "the profile contributes no bytes to a snapshot payload");
    check(!result.profile.snapshotted(),
        "the profile is flagged as never-snapshotted host state");
}

}  // namespace

int main() {
    try {
        a_named_u32_entry_parses();
        a_named_float_entry_parses();
        an_unknown_key_is_a_named_error();
        a_malformed_value_is_a_named_error();
        a_recognised_but_invalid_entry_is_not_defaulted();
        a_float_in_a_u32_slot_is_refused();
        comments_and_blank_lines_are_accepted();
        a_duplicate_key_is_a_named_error();
        an_absent_profile_changes_nothing();
        applied_lines_name_every_entry();
        a_change_to_the_profile_never_enters_the_snapshot();
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    std::cout << "host_profile_tests: 11 contracts passed\n";
    return 0;
}

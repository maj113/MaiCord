#pragma once
#include "maicord/protocol/gateway_commands.hpp"

namespace maicord {
enum class SelfStatusSettingsOperation : u8 {
    fetch,
    update
};
inline constexpr u32 kSelfStatusSettingsMaximumGroupBytes = 16 * 1024;
inline constexpr u32 kSelfStatusSettingsMaximumPatchBytes = 23 * 1024;
enum class SelfStatusSettingsError : u8 {
    none,
    input,
    base64,
    protobuf,
    capacity
};
struct SelfStatusSettingsProjection {
    u64 expires_at_ms, created_at_ms;
    u32 group_bytes;
    GatewayUserStatus status;
    bool present, known;
};
// Extracts only group 11 from PreloadedUserSettings; other groups never enter
// storage. Repeated group occurrences are concatenated in protobuf merge order.
// Validates before touching either output; no whole-proto decode allocation.
SelfStatusSettingsError self_status_settings_decode_base64(
    Utf8View input, Slice<u8> group, SelfStatusSettingsProjection* projection) noexcept;
// Partial settings updates replace a present top-level group. This writer
// retains every opaque field in the observed StatusSettings group except the
// known status/expiry/creation fields. Unrelated top-level groups are omitted.
// Scratch must hold group.size+64 bytes and must not alias group or output.
GatewayCommandWriteResult self_status_settings_write_patch(ConstSlice<u8> group, GatewayUserStatus status,
                                                           u64 unix_now_ms, Slice<u8> scratch,
                                                           Slice<u8> output) noexcept;
// StatusSettings.custom_status (field 2): text 1, emoji_id 2 (fixed64),
// emoji_name 3, expires_at_ms 4 (fixed64), created_at_ms 5 (fixed64).
inline constexpr u32 kSelfCustomStatusTextBytes = 512; // 128 code points
inline constexpr u32 kSelfCustomStatusTextCodepoints = 128;
inline constexpr u32 kSelfCustomStatusEmojiBytes = 64;
struct SelfCustomStatus {
    u64 emoji_id, expires_at_ms, created_at_ms;
    char text[kSelfCustomStatusTextBytes];
    char emoji_name[kSelfCustomStatusEmojiBytes];
    u16 text_bytes;
    u8 emoji_name_bytes;
    bool present;
    u8 reserved[4];
};
static_assert(sizeof(SelfCustomStatus) == 608);
// Absent or unreadable custom status reads as not present.
bool self_status_settings_read_custom(ConstSlice<u8> group, SelfCustomStatus* output) noexcept;
// Shown while present with text or emoji and not past its expiry.
bool self_custom_status_active(const SelfCustomStatus&, u64 unix_now_ms) noexcept;
bool self_custom_status_equal(const SelfCustomStatus&, const SelfCustomStatus&) noexcept;
// Replaces custom_status (null or an empty status removes it) and keeps every
// other StatusSettings field byte-for-byte. Scratch must hold group.size+704
// bytes and must not alias group or output.
GatewayCommandWriteResult self_status_settings_write_custom_patch(ConstSlice<u8> group, const SelfCustomStatus*,
                                                                  Slice<u8> scratch, Slice<u8> output) noexcept;
// StatusSettings.show_current_game (field 3, BoolValue): "Share my activity".
// Absent reads as shared, the service default.
bool self_status_settings_read_show_game(ConstSlice<u8> group, bool* share) noexcept;
// Replaces show_current_game and keeps every other StatusSettings field.
// Scratch must hold group.size+16 bytes and must not alias group or output.
GatewayCommandWriteResult self_status_settings_write_show_game_patch(ConstSlice<u8> group, bool share,
                                                                     Slice<u8> scratch, Slice<u8> output) noexcept;
// Custom status dialog choices: Don't clear, 30 minutes, 1 hour, 4 hours, Today.
enum class SelfCustomStatusClear : u8 { never, minutes_30, hour_1, hours_4, today, count };
// Expiry for a clear-after choice; Today ends at the next local midnight, given
// the local time of day in milliseconds.
u64 self_custom_status_expiry(SelfCustomStatusClear, u64 unix_now_ms, u64 local_ms_of_day) noexcept;
// Builds a status from dialog text. The emoji field takes a Unicode emoji or a
// custom emoji as <:name:id> / <a:name:id>; surrounding spaces are ignored.
// Returns false for overlong or malformed input. An empty result clears.
bool self_custom_status_build(Utf8View emoji, Utf8View text, SelfCustomStatusClear clear, u64 unix_now_ms,
                              u64 local_ms_of_day, SelfCustomStatus* output) noexcept;
} // namespace maicord

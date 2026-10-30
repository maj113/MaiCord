#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {

// PreloadedUserSettings (settings-proto type 1). The store keeps the verbatim
// bytes of selected top-level groups, so unknown fields survive every rewrite.
// The server replaces whole top-level groups on a partial update and on PATCH;
// edits are therefore field-level overlays rebased onto the latest server
// group and serialized as complete groups.
enum UserSettingsGroup : u8 {
    user_settings_versions = 1,
    user_settings_text_and_images = 6,
    user_settings_notifications = 7,
    user_settings_privacy = 8,
    user_settings_status = 11,
    user_settings_localization = 12,
    user_settings_appearance = 13,
    user_settings_clips = 19,
};
constexpr u32 user_settings_group_bit(u32 group) noexcept {
    return group && group < 32 ? 1u << group : 0u;
}
// Status (11) and guild folders (14) have dedicated owners and are excluded.
inline constexpr u32 kUserSettingsStoredGroups =
    user_settings_group_bit(user_settings_versions) | user_settings_group_bit(user_settings_text_and_images) |
    user_settings_group_bit(user_settings_notifications) | user_settings_group_bit(user_settings_privacy) |
    user_settings_group_bit(user_settings_localization) | user_settings_group_bit(user_settings_appearance);
inline constexpr u32 kUserSettingsProtoMaximumProtoBytes = 16 * 1024 * 1024;
inline constexpr u32 kUserSettingsProtoArenaBytes = 64 * 1024;
inline constexpr u32 kUserSettingsProtoMaximumGroupBytes = 24 * 1024;
inline constexpr u32 kUserSettingsTextBytes = 64;
inline constexpr u32 kUserSettingsEditCapacity = 24;
// Gateway hand-off only: READY auth_session_id_hash rides in this private
// field beside the extracted groups. It is never part of a PATCH.
inline constexpr u32 kUserSettingsSessionHashField = 1000;
inline constexpr u32 kUserSettingsSessionHashBytes = 64;
// Hand-off marker (varint 1) for a partial USER_SETTINGS_PROTO_UPDATE.
inline constexpr u32 kUserSettingsPartialField = 1001;
// Save delay per edit class, as the official client schedules proto writes: a user action on a
// setting goes out at once (INFREQUENT_USER_ACTION, 0 s); per-server lists, toggled in runs,
// wait 10 s (FREQUENT_USER_ACTION). A shorter class pulls a pending write forward.
inline constexpr u64 kUserSettingsImmediateMilliseconds = 0;
inline constexpr u64 kUserSettingsFrequentMilliseconds = 10'000;
// The REST request body limit (64 KiB) bounds the base64 partial message.
inline constexpr u32 kUserSettingsPatchBytes = 64 * 1024;
inline constexpr u32 kUserSettingsPartialMaximumBytes = 47 * 1024;

enum class UserSettingsProtoError : u8 { none, input, base64, protobuf, capacity };

// Leaf encodings. Wrapper kinds are the google.protobuf *Value messages;
// false/zero wrappers encode as an empty (still present) message. Plain kinds
// omit proto3 defaults. id_member addresses one ID of a repeated fixed64 list
// (privacy.restricted_guild_ids and friends): number is the ID, present is
// whether the list holds it. Lists are read packed or unpacked and written
// packed and deduplicated, new IDs appended; an empty list is omitted.
enum class UserSettingsKind : u8 { boolean, uint32, uint64, string, enumeration, plain_bool, id_member };
// group.field or group.field.child (child != 0 addresses a field of the
// sub-message `field`).
struct UserSettingsField {
    u8 group, field, child;
    UserSettingsKind kind;
};
struct UserSettingsValue {
    u64 number;
    char text[kUserSettingsTextBytes];
    u8 text_bytes;
    bool present;
};
constexpr UserSettingsValue user_settings_number(u64 n) noexcept {
    return {n, {}, 0, true};
}
// An id_member value: `id` in (member) or out of the list.
constexpr UserSettingsValue user_settings_member(u64 id, bool member) noexcept {
    return {id, {}, 0, member};
}
UserSettingsValue user_settings_text(Utf8View text) noexcept;

// Wire codec. All functions validate complete input before writing output.
// Last occurrence wins, as in protobuf merging. Strings longer than
// kUserSettingsTextBytes are reported as unreadable. id_member fields read
// through user_settings_proto_contains instead.
bool user_settings_proto_read(ConstSlice<u8> group, UserSettingsField, UserSettingsValue*) noexcept;
// Whether an id_member list holds `id`. False for malformed input too.
bool user_settings_proto_contains(ConstSlice<u8> group, UserSettingsField, u64 id) noexcept;
// Rewrites one group with field replaced (or cleared when !value.present).
// Every other byte sequence is preserved; repeated occurrences of the field
// (or its parent sub-message) collapse into one. Returns ~0u on failure.
u32 user_settings_proto_rewrite(ConstSlice<u8> group, UserSettingsField, const UserSettingsValue&,
                                Slice<u8> output) noexcept;
// Copies the masked top-level fields, each repeated group concatenated in
// merge order into a single occurrence, ascending by field number. A non-null
// session hash is appended as the private hand-off field.
struct UserSettingsExtractResult {
    u32 bytes, present_mask;
    UserSettingsProtoError error;
    constexpr bool ok() const noexcept { return error == UserSettingsProtoError::none; }
};
UserSettingsExtractResult user_settings_proto_extract_base64(Utf8View base64, u32 mask, Utf8View session_hash,
                                                             Slice<u8> output) noexcept;
// True when an extracted message carries the partial-update marker.
bool user_settings_proto_partial(ConstSlice<u8> extracted) noexcept;
// Borrowed group bytes inside an extracted message; empty when absent.
ConstSlice<u8> user_settings_proto_group(ConstSlice<u8> extracted, u32 group) noexcept;

// PATCH /users/@me/settings-proto/1 body: {"settings":"<base64>"} plus
// "required_data_version" when nonzero or `required` (an edit made offline
// pins the data version it was made against, which may be 0). The partial message may contain only
// length-delimited top-level groups 2..31 other than status; guild folders
// (group 14) travel alone, from their own owner.
bool user_settings_proto_patch_valid(ConstSlice<u8> partial) noexcept;
u32 user_settings_proto_write_patch_json(ConstSlice<u8> partial, u32 required_data_version,
                                         Slice<u8> output, bool required = false) noexcept;
// {"settings":"<base64>","out_of_date":bool} from GET/PATCH. Escaped base64
// must be unescaped by the caller into `unescape` scratch.
struct UserSettingsResponse {
    UserSettingsExtractResult extracted;
    bool out_of_date, settings_present;
};
UserSettingsResponse user_settings_proto_decode_response(Utf8View body, u32 mask, Slice<char> unescape,
                                                         Slice<u8> output) noexcept;

// Optimistic store. Single in-flight PATCH; edits made meanwhile wait for its
// completion. Server snapshots never discard pending edits: they rebase.
struct UserSettingsEdit {
    UserSettingsField field;
    UserSettingsValue value;
};
struct UserSettingsStore {
    u8 server[kUserSettingsProtoArenaBytes];
    u8 scratch[kUserSettingsProtoArenaBytes];
    UserSettingsEdit dirty[kUserSettingsEditCapacity];
    UserSettingsEdit inflight[kUserSettingsEditCapacity];
    char session_hash[kUserSettingsSessionHashBytes];
    u64 revision, due_ms;
    u32 server_bytes, present_mask, data_version, dirty_count, inflight_count;
    u32 offline_data_version;
    u8 session_hash_bytes;
    bool loaded, live, inflight_active, refetch;
    // An offline edit pinned offline_data_version (which may be 0).
    bool offline_pinned;
    // The pending write waits for a 429 retry time; new edits do not pull it forward.
    bool rate_limited;
};
enum class UserSettingsOutcome : u8 { saved, out_of_date, rate_limited, invalid_data, failed };
struct UserSettingsCompletion {
    bool rolled_back, retry;
};
void user_settings_store_reset(UserSettingsStore*) noexcept;
// Full snapshots replace every stored group; partial ones replace the groups
// they carry. Input is an extracted message (see above).
bool user_settings_store_apply(UserSettingsStore*, ConstSlice<u8> extracted, bool partial) noexcept;
// Effective value: pending edit, then in-flight edit, then server.
bool user_settings_store_read(const UserSettingsStore&, UserSettingsField, UserSettingsValue*) noexcept;
// Effective membership of `id` in an id_member list, by the same precedence.
// Edits are keyed by field and ID, so toggles of different IDs batch together.
bool user_settings_store_contains(const UserSettingsStore&, UserSettingsField, u64 id) noexcept;
ConstSlice<u8> user_settings_store_server_group(const UserSettingsStore&, u32 group) noexcept;
// Records an edit. The batch is due after the edit's delay class (the shortest wins) unless a
// rate-limit retry is pending. Edits while offline pin the data version for conflict checks.
bool user_settings_store_set(UserSettingsStore*, UserSettingsField, const UserSettingsValue&, u64 now_ms) noexcept;
bool user_settings_store_due(const UserSettingsStore&, u64 now_ms) noexcept;
// Serializes the dirty groups into a partial message and moves the edits in
// flight. Returns the message bytes (0 when nothing to send or on failure).
// *required reports whether required_data_version must be sent.
u32 user_settings_store_begin(UserSettingsStore*, Slice<u8> partial, u32* required_data_version,
                              bool* required = nullptr) noexcept;
// response is the extracted message of the reply (empty on failure).
UserSettingsCompletion user_settings_store_complete(UserSettingsStore*, UserSettingsOutcome,
                                                    ConstSlice<u8> response, u64 retry_at_ms) noexcept;

} // namespace maicord

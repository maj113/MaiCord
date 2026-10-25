#pragma once
#include "maicord/core/json_reader.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {
// Structured presence activities (rich presence). Custom status stays in
// UserAppearance; this store holds the other activity types per user, in the
// order the server sent them. Every string is owned, bounded UTF-8.
enum class UserActivityType : u8 { playing = 0, streaming = 1, listening = 2, watching = 3, custom = 4, competing = 5 };
enum class UserActivityField : u8 {
    name,
    details,
    state,
    large_image,
    large_text,
    small_image,
    small_text,
    url,
    button_0,
    button_1,
    // The presence session (received only); activity metadata is keyed by it.
    session,
    count
};
inline constexpr u32 kUserActivityFieldCount = static_cast<u32>(UserActivityField::count);
inline constexpr u32 kUserActivityTextBytes = 1024;
inline constexpr u32 kUserActivityMaximum = 3;
// Per-field byte budgets; text is cut at a code point, image keys (up to 313
// characters, e.g. mp:external/...) are kept whole or dropped.
inline constexpr u16 kUserActivityFieldBudget[kUserActivityFieldCount]{128, 128, 128, 320, 128,
                                                                       320, 128, 256, 96, 96, 64};
// The largest user_activities_write record: marker, count and three
// activities' fixed fields and full text.
inline constexpr u32 kUserActivityRecordMaximumBytes =
    2 + kUserActivityMaximum * (4 + 4 * 8 + 3 * 4 + 2 * kUserActivityFieldCount + kUserActivityTextBytes);
enum UserActivityFlag : u32 {
    user_activity_flag_join = 1u << 1,
    user_activity_flag_embedded = 1u << 8,
};
// Facts about fields that are not kept whole.
enum UserActivityDetail : u8 {
    user_activity_has_party = 1u << 0,
    // party.id starts with "spotify:" (the Spotify integration's sessions).
    user_activity_spotify_party = 1u << 1,
    // status_display_type + 1 (0 absent): which field a status line names.
    user_activity_display_shift = 2,
    user_activity_display_mask = 3u << user_activity_display_shift,
};
// activity.platform; desktop when absent.
enum class UserActivityPlatform : u8 { desktop, xbox, ps4, ps5, ios, android, embedded, samsung, meta_quest };
struct UserActivity {
    u64 application_id;
    u64 start_ms, end_ms, created_ms; // Unix milliseconds; zero when absent.
    u32 party_size, party_max, flags;
    u16 lengths[kUserActivityFieldCount]; // Packed in field order inside text.
    UserActivityType type;
    u8 button_count, detail;
    UserActivityPlatform platform;
    char text[kUserActivityTextBytes];
};
struct UserActivitySet {
    u64 user_id;
    u32 last_seen;
    u8 count;
    UserActivity items[kUserActivityMaximum];
};
Utf8View user_activity_text(const UserActivity&, UserActivityField) noexcept;
// The Spotify integration's listening activity: named "Spotify" with a
// "spotify:" party.
bool user_activity_is_spotify(const UserActivity&) noexcept;
// One-line status: "Playing X", "Streaming X", "Listening to X", "Watching X",
// "Competing in X". Returns the byte count written (0 if none).
u32 user_activity_summary(UserActivityType, Utf8View name, Utf8View details, Slice<char> output) noexcept;
u32 user_activity_summary(const UserActivity&, Slice<char> output) noexcept;
// A user row's status line text: the game's name, Spotify's artists ("A, B"),
// "Listening to X", "Watching X", "Competing in X" or "Streaming X", where X
// is the name (a stream's details) or the field status_display_type names.
u32 user_activity_status_text(const UserActivity&, Slice<char> output) noexcept;
// Card heading: "Playing", "Playing on Xbox", "Streaming Twitch", "Listening
// to Spotify" (with details, else "Listening to"), "Watching X", ...
u32 user_activity_heading(const UserActivity&, Slice<char> output) noexcept;
// Image URL for an asset key, requested at 160 px: numeric application assets
// (PNG), mp: media proxy paths (WebP) and spotify: album art (JPEG on
// i.scdn.co). Twitch and YouTube previews and unknown prefixes return 0.
u32 user_activity_image_url(const UserActivity&, bool large, Slice<char> output,
                            MediaAttachmentFormat* format) noexcept;
// An application's icon (GET /applications/public), as PNG at 160 px.
u32 user_activity_application_icon_url(u64 application_id, Utf8View icon, Slice<char> output) noexcept;
// Card artwork, in the official fallback order: the large asset, the
// application's icon, then the small asset in the large slot.
enum class UserActivityArt : u8 { none, large, application, small_image };
struct UserActivityArtwork {
    UserActivityArt large;
    // The small asset rides on the large slot's corner.
    bool badge;
};
UserActivityArtwork user_activity_artwork(const UserActivity&, bool application_icon) noexcept;

// GET /applications/public?application_ids=... for up to kUserActivityMaximum
// applications. Zero on a zero ID or a full output.
u32 user_activity_applications_target(ConstSlice<u64> ids, Slice<u8> output) noexcept;
// One application of that response: its ID and icon hash (empty when it has
// none). Calls visit per element; false on malformed JSON.
using UserActivityApplicationVisit = void (*)(void* context, u64 id, Utf8View icon) noexcept;
bool user_activity_applications_decode(Utf8View body, Slice<u32> structurals, UserActivityApplicationVisit visit,
                                       void* context) noexcept;
// GET /users/{user}/sessions/{session}/activities/{application or 0}/metadata.
u32 user_activity_metadata_target(u64 user_id, Utf8View session, u64 application_id, Slice<u8> output) noexcept;
// metadata.button_urls[index] when it is an http(s) URL; 0 otherwise.
u32 user_activity_button_url(Utf8View body, Slice<u32> structurals, u32 index, Slice<char> output) noexcept;

// Decodes presence.activities into non-custom activities (at most
// kUserActivityMaximum). *present reports whether the presence carried an
// activities member; when false the output is cleared and the caller keeps
// what it had. Oversized fields are truncated, never fatal.
bool user_activities_decode(const JsonIndexedDocument& doc, JsonValue presence, u64 user_id,
                            UserActivitySet* output, bool* present) noexcept;

// Mailbox wire form (validated on read). An empty set writes a marker only.
u32 user_activities_write(const UserActivitySet&, Slice<u8> output) noexcept;
// Decodes straight into *output: on false it is unspecified, and on success
// items past count and text past each field's length keep their old bytes.
bool user_activities_read(ConstSlice<u8> input, u64 user_id, UserActivitySet* output) noexcept;

// Users' activities in their wire form: a sorted index over one compacting
// byte bank, so a user costs its record (typically 100-250 bytes) instead of
// a whole UserActivitySet. Past 2048 users or the bank budget the least
// recently updated users are evicted, guild-scoped records before friends'.
struct UserActivityStore {
    StableStorage records, order, bank;
    u32 count, clock, revision;
    u32 bank_used, bank_dead; // bank end and reclaimable bytes (headers included)
};
// Copies a user's activities into *output; false (output unspecified) when none.
bool user_activity_find(const UserActivityStore&, u64 user_id, UserActivitySet* output) noexcept;
// A user's activity count without decoding them (zero when none).
u32 user_activity_count(const UserActivityStore&, u64 user_id) noexcept;
// The first activity's type without decoding the record; false when none.
bool user_activity_first_type(const UserActivityStore&, u64 user_id, UserActivityType* output) noexcept;
// Replaces a user's activities; an empty set removes the record. Revision
// advances only on a change.
bool user_activity_upsert(UserActivityStore*, const UserActivitySet&) noexcept;
// One presence's activities from guild `scope` (0: a friend or DM presence).
// The store keeps one set per user, so scopes merge as the official client's
// guild-less lookup does: a set with activities replaces the record; an empty
// set clears it only from the scope that wrote it or from scope 0, so a
// server that hides the activity cannot erase the friend presence's. Offline
// clears every scope.
bool user_activity_apply(UserActivityStore*, const UserActivitySet&, u64 scope, bool offline) noexcept;
void user_activity_release(UserActivityStore*) noexcept;
} // namespace maicord

#pragma once
#include "maicord/core/json_reader.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/utf.hpp"

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
    count
};
inline constexpr u32 kUserActivityFieldCount = static_cast<u32>(UserActivityField::count);
inline constexpr u32 kUserActivityTextBytes = 768;
inline constexpr u32 kUserActivityMaximum = 3;
// Per-field byte budgets; text is cut at a code point, image keys are kept
// whole or dropped.
inline constexpr u16 kUserActivityFieldBudget[kUserActivityFieldCount]{128, 128, 128, 256, 128,
                                                                       256, 128, 256, 96, 96};
enum UserActivityFlag : u32 {
    user_activity_flag_instance = 1u << 0,
    user_activity_flag_join = 1u << 1,
    user_activity_flag_sync = 1u << 4,
    user_activity_flag_play = 1u << 5,
};
struct UserActivity {
    u64 application_id;
    u64 start_ms, end_ms, created_ms; // Unix milliseconds; zero when absent.
    u32 party_size, party_max, flags;
    u16 lengths[kUserActivityFieldCount]; // Packed in field order inside text.
    UserActivityType type;
    u8 button_count;
    char text[kUserActivityTextBytes];
};
struct UserActivitySet {
    u64 user_id;
    u32 last_seen;
    u8 count;
    UserActivity items[kUserActivityMaximum];
};
Utf8View user_activity_text(const UserActivity&, UserActivityField) noexcept;
// Spotify listening activity (type 2 from the Spotify integration).
bool user_activity_is_spotify(const UserActivity&) noexcept;
// One-line status: "Playing X", "Streaming X", "Listening to X", "Watching X",
// "Competing in X". Returns the byte count written (0 if none).
u32 user_activity_summary(UserActivityType, Utf8View name, Utf8View details, Slice<char> output) noexcept;
u32 user_activity_summary(const UserActivity&, Slice<char> output) noexcept;
// Card heading: "Playing", "Listening to Spotify", "Watching", ...
u32 user_activity_heading(const UserActivity&, Slice<char> output) noexcept;
// Image URL for an asset key: mp: media proxy paths and numeric application
// assets. Spotify/Twitch/YouTube keys have no CDN-hosted URL and return 0.
// *webp is set for proxied images (requested as WebP), cleared for PNG.
u32 user_activity_image_url(const UserActivity&, bool large, Slice<char> output, bool* webp) noexcept;
// Fraction of start..end elapsed at now, clamped to [0, 1]; -1 without both.
float user_activity_progress(const UserActivity&, u64 unix_now_ms) noexcept;

// Decodes presence.activities into non-custom activities (at most
// kUserActivityMaximum). *present reports whether the presence carried an
// activities member; when false the output is cleared and the caller keeps
// what it had. Oversized fields are truncated, never fatal.
bool user_activities_decode(const JsonIndexedDocument& doc, JsonValue presence, u64 user_id,
                            UserActivitySet* output, bool* present) noexcept;

// Mailbox wire form (validated on read). An empty set writes a marker only.
u32 user_activities_write(const UserActivitySet&, Slice<u8> output) noexcept;
bool user_activities_read(ConstSlice<u8> input, u64 user_id, UserActivitySet* output) noexcept;

// Users' activities in their wire form: a sorted index over one compacting
// byte bank, so a user costs its record (typically 100-250 bytes) instead of
// a whole UserActivitySet. Past 2048 users or the bank budget the least
// recently updated users are evicted.
struct UserActivityStore {
    StableStorage records, order, bank;
    u32 count, clock, revision;
    u32 bank_used, bank_dead; // bank end and reclaimable bytes (headers included)
};
// Copies a user's activities into *output; false (output unspecified) when none.
bool user_activity_find(const UserActivityStore&, u64 user_id, UserActivitySet* output) noexcept;
// A user's activity count without decoding them (zero when none).
u32 user_activity_count(const UserActivityStore&, u64 user_id) noexcept;
// Replaces a user's activities; an empty set removes the record. Revision
// advances only on a change.
bool user_activity_upsert(UserActivityStore*, const UserActivitySet&) noexcept;
void user_activity_release(UserActivityStore*) noexcept;
} // namespace maicord

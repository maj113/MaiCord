#pragma once
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/json_reader.hpp"

namespace maicord {
inline constexpr u32 kProfileBioMaximumCodepoints = 300;
inline constexpr u32 kProfileBioMaximumUtf8Bytes = kProfileBioMaximumCodepoints * 4;
template <u32 N> struct ProfileString {
    char data[N];
    u16 bytes;
    Utf8View view() const noexcept {
        return {data, bytes};
    }
};
struct NameStyle {
    u32 colors[8];
    u16 font_id, effect_id;
    u8 color_count;
};
enum UserAppearanceField : u32 {
    appearance_bot = 1,
    appearance_flags = 2,
    appearance_name = 4,
    appearance_decoration = 8,
    appearance_nameplate = 16,
    appearance_guild_tag = 32,
    appearance_activity = 64,
    appearance_avatar = 128,
    // With appearance_activity: the line is the custom status, not a
    // "Playing X" summary of another activity.
    appearance_custom_status = 256,
};
inline constexpr u32 kAppearanceActivityBytes = 256;
inline constexpr u32 kAppearanceStatusEmojiBytes = 64;
struct UserAppearance {
    u64 user_id, guild_id;
    u64 public_flags, primary_guild_id, status_emoji_id, nameplate_sku;
    u32 fields, last_seen;
    NameStyle name;
    ProfileString<64> decoration;
    ProfileString<128> nameplate;
    ProfileString<34> avatar;
    ProfileString<34> primary_badge;
    ProfileString<32> primary_tag;
    ProfileString<kAppearanceActivityBytes> activity;
    ProfileString<kAppearanceStatusEmojiBytes> status_emoji;
    bool bot, primary_enabled, emoji_animated;
};
// A string of a stored appearance record. The offset is relative to this
// field, so a record is one contiguous allocation whose strings follow it.
struct AppearanceText {
    u16 offset, bytes;
    Utf8View view() const noexcept {
        return {reinterpret_cast<const char*>(this) + offset, bytes};
    }
};
// The cached form of a UserAppearance: fixed fields, then only the string
// bytes in use (a typical record is about a fifth of the decoded size).
// Records live in the store's arena and are never copied by value; a pointer
// from user_appearance_find stays valid until the next upsert or release.
struct UserAppearanceRecord {
    u64 user_id, guild_id;
    u64 public_flags, primary_guild_id, status_emoji_id, nameplate_sku;
    u32 fields, last_seen;
    NameStyle name;
    AppearanceText decoration, nameplate, avatar, primary_badge, primary_tag, activity, status_emoji;
    u16 bytes; // record and strings
    u16 units; // allocated 8-byte units
    bool bot, primary_enabled, emoji_animated;
    UserAppearanceRecord(const UserAppearanceRecord&) = delete;
    UserAppearanceRecord& operator=(const UserAppearanceRecord&) = delete;
};
struct UserAppearanceStore {
    // records: variable-length UserAppearanceRecords in 8-byte units;
    // order: their unit offsets sorted by (user, guild).
    StableStorage records, order;
    u32 count, clock, revision;
    u32 used_units, garbage_units;
};
const UserAppearanceRecord* user_appearance_find(const UserAppearanceStore& store, u64 user,
                                                 u64 guild = 0) noexcept;
// The user's activity line from any scope: the global record when it carries
// activity, else the first server record that does (presence is global).
const UserAppearanceRecord* user_appearance_find_activity(const UserAppearanceStore& store,
                                                          u64 user) noexcept;
void user_appearance_expand(const UserAppearanceRecord& record, UserAppearance* output) noexcept;
bool user_appearance_upsert(UserAppearanceStore* store, const UserAppearance& value) noexcept;
u32 user_appearance_write(const UserAppearance& value, Slice<u8> output) noexcept;
bool user_appearance_read(ConstSlice<u8> input, u64 user, u64 guild, UserAppearance* output) noexcept;
// Reads an appearance record that may be followed by other data (the
// presence activities trailer); *consumed receives its byte length.
bool user_appearance_read_prefix(ConstSlice<u8> input, u64 user, u64 guild, UserAppearance* output,
                                 u32* consumed) noexcept;
void user_appearance_release(UserAppearanceStore* store) noexcept;
// All strings are copied into bounded owned fields before the Gateway/REST buffer is reused.
bool user_appearance_decode(const JsonIndexedDocument& doc, JsonValue user, u64 user_id, u64 guild_id,
                               UserAppearance* output) noexcept;
bool activity_decode(const JsonIndexedDocument& doc, JsonValue presence,
                             UserAppearance* output) noexcept;

// A profile badge: the tooltip and accessible name (description), the
// /badge-icons/ hash and the https link a click opens (empty when none).
struct ProfileBadge {
    ProfileString<64> id;
    ProfileString<34> icon;
    ProfileString<192> description;
    ProfileString<128> link;
};
struct ProfileConnection {
    ProfileString<32> type;
    ProfileString<128> name;
    // metadata.created_at (ISO 8601) when the platform reports it.
    ProfileString<40> created_at;
    bool verified;
};
struct ProfilePeer {
    u64 id;
    ProfileString<128> name;
    ProfileString<34> icon;
};
struct ProfileEffectLayer {
    ProfileString<384> source;
    ProfileString<64> id;
    u32 start_ms, duration_ms, loop_delay_ms, z_index;
    i32 x, y;
    bool loop;
};
struct ProfileEffect {
    u64 sku;
    ProfileString<384> reduced_source;
    ProfileEffectLayer layers[8];
    u8 count;
};
bool profile_effect_decode(Utf8View json, Slice<u32> scratch, u64 sku,
                                   ProfileEffect* output) noexcept;
struct UserProfile {
    u64 user_id, guild_id;
    UserAppearance user_appearance, guild_appearance;
    ProfileString<128> username, display_name, nickname;
    ProfileString<34> avatar, banner, guild_banner;
    ProfileString<kProfileBioMaximumUtf8Bytes> bio, guild_bio;
    // 40 code points (the API's limit), up to 4 bytes each.
    ProfileString<160> pronouns, guild_pronouns;
    ProfileString<40> joined_at;
    u32 theme_colors[2], guild_theme_colors[2], accent_color;
    u64 effect_sku_id, guild_effect_sku_id;
    // guild_member.roles; guilds allow 250 roles.
    u64 roles[256];
    // User badges, then the guild member's (guild_badges).
    ProfileBadge badges[24];
    ProfileConnection connections[16];
    ProfilePeer mutual_guilds[100], mutual_friends[100];
    u32 mutual_friend_count;
    u16 role_count, badge_count, connection_count, mutual_guild_count, mutual_friends_count;
    u8 premium_type;
    bool has_theme, has_guild_theme, private_profile;
};
bool user_profile_decode(Utf8View json, Slice<u32> scratch, u64 expected_user, u64 guild,
                            UserProfile* output) noexcept;
} // namespace maicord

#pragma once

#include "maicord/model/user_profile.hpp"

namespace maicord {
struct RestRouteKey;
enum class RestMethod : u8;

// Profile editing (main and per-server profiles): save requests, profile loads,
// the owned collectibles list and the flattened field errors of a rejected save.
// Bodies hold only changed fields; "" clears text, null removes images, colours
// and collectibles.
inline constexpr u32 kProfileEditDisplayNameCodepoints = 32;
inline constexpr u32 kProfileEditNicknameCodepoints = 32;
inline constexpr u32 kProfileEditPronounsCodepoints = 40;
inline constexpr u32 kProfileEditBioCodepoints = 190;
inline constexpr u32 kProfileEditDescriptionBytes = 256;
inline constexpr u32 kProfileEditCollectibleMaximum = 8;
inline constexpr u64 kProfileEditRouteBase = 0x5052454400000000ull;

enum class ProfileEditOperation : u8 {
    user_patch,           // PATCH /users/@me
    user_profile_patch,   // PATCH /users/@me/profile
    member_patch,         // PATCH /guilds/{g}/members/@me
    member_profile_patch, // PATCH /guilds/{g}/profile/@me
    // GET /users/{u}/profile?with_mutual_guilds=false&with_mutual_friends=false
    //     &with_mutual_friends_count=false[&guild_id={g}]
    profile_fetch,
    collectibles,        // GET /users/@me/collectibles-purchases?variants_return_style=2
    collectible_product, // GET /collectibles-products/{sku}
    count,
};

// Collectible item types (the API's item type values).
enum class ProfileCollectibleType : u8 {
    avatar_decoration = 0,
    profile_effect = 1,
    nameplate = 2,
    profile_frame = 3,
};
struct ProfileCollectibleRef {
    u64 sku;
    ProfileCollectibleType type;
};

enum UserPatchField : u8 {
    user_patch_avatar = 1u << 0,
    user_patch_global_name = 1u << 1,
    user_patch_avatar_decoration = 1u << 2,
};
// avatar: a data URI, empty sends null. avatar_description rides along a new
// avatar. A zero decoration sku sends null.
struct UserPatch {
    u8 fields;
    Utf8View avatar, avatar_description, global_name;
    u64 avatar_decoration_sku;
};
enum MemberEditField : u8 {
    member_edit_nick = 1u << 0,
    member_edit_avatar = 1u << 1,
    member_edit_avatar_decoration = 1u << 2,
};
struct MemberEditPatch {
    u8 fields;
    Utf8View nick, avatar, avatar_description;
    u64 avatar_decoration_sku;
};
enum ProfilePatchField : u8 {
    profile_patch_banner = 1u << 0,
    profile_patch_bio = 1u << 1,
    profile_patch_pronouns = 1u << 2,
    profile_patch_accent = 1u << 3,
    profile_patch_theme = 1u << 4,
    profile_patch_collectibles = 1u << 5,
};
// theme_colors as [primary, accent], null, or (per-server reset) [null,null].
enum class ProfileThemePatch : u8 {
    colors,
    none,
    reset,
};
struct ProfilePatch {
    u8 fields;
    ProfileThemePatch theme;
    bool accent_none;
    u8 collectible_count;
    Utf8View banner, bio, pronouns;
    u32 accent_color;
    u32 theme_colors[2];
    u64 collectibles[kProfileEditCollectibleMaximum];
};

struct ProfileEditRequest {
    ProfileEditOperation operation;
    u8 reserved[7];
    // Member operations, and the optional server of profile_fetch.
    u64 guild_id;
    // profile_fetch: the profile's user; collectible_product: the sku.
    u64 id;
    const UserPatch* user;
    const MemberEditPatch* member;
    const ProfilePatch* profile;
};

struct ProfileEditWriteResult {
    u32 bytes;
    bool ok;
};
RestMethod profile_edit_method(ProfileEditOperation) noexcept;
bool profile_edit_request_valid(const ProfileEditRequest&) noexcept;
// The route, target and body writers below require a request this accepts.
RestRouteKey profile_edit_route(const ProfileEditRequest&) noexcept;
inline constexpr u32 kProfileEditMaximumTargetBytes = 192;
ProfileEditWriteResult profile_edit_write_target(const ProfileEditRequest&, Slice<u8> output) noexcept;
// Capacity a body needs (an upper bound: text may escape to six bytes per
// byte); zero for bodyless requests.
u32 profile_edit_body_capacity(const ProfileEditRequest&) noexcept;
ProfileEditWriteResult profile_edit_write_body(const ProfileEditRequest&, Slice<u8> output) noexcept;

// The editable values a GET profile answers with (user and, with a server,
// its member and member profile). Text is copied and bounded.
struct ProfileEditSaved {
    u64 user_id, guild_id;
    u8 premium_type;
    ProfileString<64> username;
    ProfileString<128> global_name;
    ProfileString<34> avatar, banner;
    ProfileString<kProfileBioMaximumUtf8Bytes> bio;
    ProfileString<160> pronouns;
    ProfileString<64> decoration_asset;
    u64 decoration_sku;
    u32 accent_color, theme_colors[2];
    bool has_accent, has_theme;
    ProfileCollectibleRef collectibles[kProfileEditCollectibleMaximum];
    u8 collectible_count;
    // Server values.
    ProfileString<128> nick;
    ProfileString<34> member_avatar, member_banner;
    ProfileString<kProfileBioMaximumUtf8Bytes> member_bio;
    ProfileString<160> member_pronouns;
    ProfileString<64> member_decoration_asset;
    u64 member_decoration_sku;
    u32 member_theme_colors[2];
    bool has_member_theme;
    ProfileCollectibleRef member_collectibles[kProfileEditCollectibleMaximum];
    u8 member_collectible_count;
};
bool profile_edit_decode_profile(Utf8View body, Slice<u32> structurals, u64 user, u64 guild,
                                 ProfileEditSaved* output) noexcept;
// The sku of a type in a collectibles list; zero when absent.
u64 profile_edit_collectible(ConstSlice<ProfileCollectibleRef> list, ProfileCollectibleType type) noexcept;

// One owned avatar decoration or profile effect.
struct ProfileCollectible {
    u64 sku;
    ProfileCollectibleType type;
    ProfileString<96> label;
    // Decorations: the preset asset; effects: the thumbnail URL.
    ProfileString<384> asset;
};
// Owned decorations and effects from GET collectibles-purchases (variant
// groups flattened, other types skipped). ~0u for a malformed body.
u32 profile_edit_decode_collectibles(Utf8View body, Slice<u32> structurals, Slice<ProfileCollectible> output) noexcept;

// A rejected save, flattened to the first message of each field the page shows,
// plus the top-level message.
enum class ProfileEditErrorField : u8 {
    global_name,
    nick,
    pronouns,
    avatar,
    banner,
    bio,
    misc,
    count,
};
struct ProfileEditErrors {
    ProfileString<192> fields[static_cast<u32>(ProfileEditErrorField::count)];
    ProfileString<192> message;
    u64 code;
    bool username;
};
bool profile_edit_decode_errors(Utf8View body, Slice<u32> structurals, ProfileEditErrors* output) noexcept;

} // namespace maicord

#pragma once
#include "maicord/protocol/profile_edit.hpp"

namespace maicord {

// Profiles page policy: what changed against the saved profile, what the
// account may change (Nitro and permissions), whether Save is allowed and the
// two partial PATCH bodies of a save. Pure; the application owns the page,
// images and requests.
enum class ProfileEditorTab : u8 {
    main,  // "Main Profile": PATCH /users/@me, then /users/@me/profile
    guild, // "Per-server Profiles": the member, then the member profile
};
// Premium type 2 (Nitro) unlocks premium profile customization.
inline constexpr u8 kProfileEditorNitroPremiumType = 2;

enum ProfileEditorField : u16 {
    profile_editor_name = 1u << 0, // display name (main) or server nickname (guild)
    profile_editor_pronouns = 1u << 1,
    profile_editor_avatar = 1u << 2,
    profile_editor_decoration = 1u << 3,
    profile_editor_effect = 1u << 4,
    profile_editor_banner = 1u << 5,
    profile_editor_theme = 1u << 6,
    profile_editor_accent = 1u << 7, // main profile "Banner Color" (no Nitro)
    profile_editor_bio = 1u << 8,
    profile_editor_all = (1u << 9) - 1,
};

enum class ProfileImageEdit : u8 {
    keep,
    // A new image; its data URI is filled when the save prepares it.
    replace,
    // Main: no image (null). Guild: back to the account image (null).
    remove,
};

// Saved values of the tab being edited (guild: the member's own values;
// empty text and false flags mean the account value shows).
struct ProfileEditorBase {
    Utf8View name, pronouns, bio;
    bool avatar, banner;
    u64 decoration_sku, effect_sku;
    bool has_theme, has_accent;
    u32 theme[2], accent;
    // The saved collectibles list (effects and frames go in one list).
    ConstSlice<ProfileCollectibleRef> collectibles;
};
struct ProfileEditorDraft {
    Utf8View name, pronouns, bio;
    ProfileImageEdit avatar, banner;
    Utf8View avatar_uri, avatar_description, banner_uri;
    // MD5 (lowercase hex) of the picked original files, or empty.
    Utf8View avatar_md5, banner_md5;
    bool decoration_set, effect_set, theme_set, accent_set;
    u64 decoration_sku, effect_sku;
    // theme_set with theme_reset: guild "Reset Theme" ([null,null]); main: null.
    bool theme_reset;
    u32 theme[2], accent;
};

// Fields the account may change on a tab. Main: everything except Banner
// and Theme without Nitro (Banner Color is offered instead). Guild:
// Nickname (CHANGE_NICKNAME) and Pronouns are free; the rest needs Nitro.
struct ProfileEditorGate {
    u16 enabled;
};
ProfileEditorGate profile_editor_gate(ProfileEditorTab tab, u8 premium_type, bool change_nickname) noexcept;
// Explanation for a gated field (no upsell); empty when enabled.
Utf8View profile_editor_gate_reason(ProfileEditorTab tab, ProfileEditorGate gate, ProfileEditorField field) noexcept;

// profile_editor_* bits the draft changes, normalised (an empty display name
// over none is no change).
u16 profile_editor_changes(ProfileEditorTab tab, const ProfileEditorBase& base,
                           const ProfileEditorDraft& draft) noexcept;
// Remaining bio characters as the counter shows them (may be negative).
i32 profile_editor_bio_remaining(Utf8View bio, u32 limit = kProfileEditBioCodepoints) noexcept;
// Every change is allowed by the gate (resetting a gated per-server value
// stays allowed) and every text is within its input limit.
bool profile_editor_changes_allowed(ProfileEditorTab tab, const ProfileEditorBase& base,
                                    const ProfileEditorDraft& draft, ProfileEditorGate gate) noexcept;
// The partial bodies of a save: main fills user and profile, guild fills
// member and profile; a body whose fields stay zero is not sent. False when
// a change is not allowed or a replaced image has no data URI yet.
bool profile_editor_patches(ProfileEditorTab tab, const ProfileEditorBase& base, const ProfileEditorDraft& draft,
                            ProfileEditorGate gate, UserPatch* user, MemberEditPatch* member,
                            ProfilePatch* profile) noexcept;

// "#5865F2" for 0x5865F2.
u32 profile_editor_format_color(u32 color, Slice<char> output) noexcept;

} // namespace maicord

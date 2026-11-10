#pragma once
#include "maicord/protocol/user/profile_edit.hpp"

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

// The display name fonts and effects the editor offers (Userdoccers "Display Name Font" / "Display Name Effect"),
// with each effect's colour count and starting colours. The default font is gg sans (11).
struct ProfileNameFont {
    u16 id;
    Utf8View name;
};
inline constexpr ProfileNameFont kProfileNameFonts[]{
    {11, utf8_literal("gg sans")},  {12, utf8_literal("Tempo")},     {3, utf8_literal("Sakura")},
    {4, utf8_literal("Jellybean")}, {6, utf8_literal("Modern")},     {7, utf8_literal("Medieval")},
    {8, utf8_literal("8Bit")},      {10, utf8_literal("Vampyre")},   {13, utf8_literal("Monkey Bars")},
    {14, utf8_literal("Mainframe")}, {15, utf8_literal("Headbang")}, {16, utf8_literal("Journal")}};
inline constexpr u32 kProfileNameFontCount = sizeof(kProfileNameFonts) / sizeof(kProfileNameFonts[0]);
struct ProfileNameEffect {
    u16 id;
    Utf8View name;
    u8 colors;
    u32 defaults[kProfileNameStyleColors];
};
// The official client's starting colours: its text colour for Solid, its first gradient preset, and its first Gummy and
// Prism palettes (four and five colours).
inline constexpr ProfileNameEffect kProfileNameEffects[]{
    {1, utf8_literal("Solid"), 1, {0xDBDEE1}},
    {2, utf8_literal("Gradient"), 2, {0x2AAEA6, 0xFFC490}},
    {3, utf8_literal("Neon"), 1, {0x691DED}},
    {4, utf8_literal("Toon"), 1, {0xF42098}},
    {5, utf8_literal("Pop"), 1, {0x0FCF86}},
    {8, utf8_literal("Gummy"), 4, {0xE9A9D5, 0xA9DDF0, 0xB9A7EE, 0xB9EACB}},
    {7, utf8_literal("Prism"), 5, {0xFE9242, 0x35ED7E, 0xFFE047, 0xFF4CD2, 0x5865F2}}};
inline constexpr u32 kProfileNameEffectCount = sizeof(kProfileNameEffects) / sizeof(kProfileNameEffects[0]);
// The official client's suggested name colours (the swatches beside its custom colour).
inline constexpr u32 kProfileNameColorSuggestions[]{0x24E36D, 0x1C98EB, 0xC32AFF, 0xDA0149, 0xF93722, 0xC6A615};
// The table index of a font or effect id; ~0u when the editor does not offer it.
u32 profile_name_font_index(u16 id) noexcept;
u32 profile_name_effect_index(u16 id) noexcept;
// The style with the effect at `index` and its starting colours (the font kept, gg sans when none).
ProfileNameStyle profile_name_style_with_effect(const ProfileNameStyle& style, u32 index) noexcept;
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
    profile_editor_name_style = 1u << 9, // display name font, effect and colours (Nitro)
    profile_editor_nameplate = 1u << 10,
    profile_editor_all = (1u << 11) - 1,
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
    u64 decoration_sku, effect_sku, nameplate_sku;
    bool has_theme, has_accent;
    u32 theme[2], accent;
    ProfileNameStyle name_style;
    // The saved collectibles list (effects and frames go in one list).
    ConstSlice<ProfileCollectibleRef> collectibles;
};
struct ProfileEditorDraft {
    Utf8View name, pronouns, bio;
    ProfileImageEdit avatar, banner;
    Utf8View avatar_uri, avatar_description, banner_uri;
    // MD5 (lowercase hex) of the picked original files, or empty.
    Utf8View avatar_md5, banner_md5;
    bool decoration_set, effect_set, theme_set, accent_set, name_style_set, nameplate_set;
    u64 decoration_sku, effect_sku, nameplate_sku;
    // theme_set with theme_reset: guild "Reset Theme" ([null,null]); main: null.
    bool theme_reset;
    u32 theme[2], accent;
    // A style without font and effect removes it.
    ProfileNameStyle name_style;
};

// Fields the account may change on a tab. Main: everything except Banner,
// Theme and the display name style without Nitro (Banner Color is offered
// instead). Guild: Nickname (CHANGE_NICKNAME) and Pronouns are free; the rest needs Nitro.
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
i32 profile_editor_bio_remaining(Utf8View bio) noexcept;
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

} // namespace maicord

#include "maicord/model/profile_editor.hpp"
#include "maicord/core/color_hex.hpp"

namespace maicord {
namespace {
bool same(Utf8View a, Utf8View b) noexcept {
    return utf8_equal(a, b);
}
u32 codepoints(Utf8View text) noexcept {
    const auto checked = utf8_validate(text);
    return checked.valid ? checked.codepoints : ~0u;
}
bool within(Utf8View text, u32 limit) noexcept {
    return codepoints(text) <= limit;
}
bool image_changed(ProfileImageEdit edit, bool saved) noexcept {
    return edit == ProfileImageEdit::replace || (edit == ProfileImageEdit::remove && saved);
}
bool theme_changed(ProfileEditorTab, const ProfileEditorBase& base, const ProfileEditorDraft& draft) noexcept {
    if (!draft.theme_set)
        return false;
    if (draft.theme_reset)
        return base.has_theme;
    return !base.has_theme || base.theme[0] != draft.theme[0] || base.theme[1] != draft.theme[1];
}
// Resetting a per-server value to the account value needs no Nitro.
bool resets(ProfileEditorTab tab, const ProfileEditorDraft& draft, ProfileEditorField field) noexcept {
    if (tab != ProfileEditorTab::guild)
        return false;
    switch (field) {
    case profile_editor_avatar:
        return draft.avatar == ProfileImageEdit::remove;
    case profile_editor_banner:
        return draft.banner == ProfileImageEdit::remove;
    case profile_editor_bio:
        return !draft.bio.bytes;
    case profile_editor_theme:
        return draft.theme_reset;
    case profile_editor_decoration:
        return !draft.decoration_sku;
    case profile_editor_effect:
        return !draft.effect_sku;
    default:
        return false;
    }
}
} // namespace

ProfileEditorGate profile_editor_gate(ProfileEditorTab tab, u8 premium_type, bool change_nickname) noexcept {
    const bool nitro = premium_type == kProfileEditorNitroPremiumType;
    if (tab == ProfileEditorTab::main) {
        u16 enabled = profile_editor_all;
        if (!nitro)
            enabled &= static_cast<u16>(~(profile_editor_banner | profile_editor_theme));
        return {enabled};
    }
    u16 enabled = profile_editor_pronouns;
    if (change_nickname)
        enabled |= profile_editor_name;
    if (nitro)
        enabled |= profile_editor_avatar | profile_editor_decoration | profile_editor_effect | profile_editor_banner |
                   profile_editor_theme | profile_editor_bio;
    return {enabled};
}

Utf8View profile_editor_gate_reason(ProfileEditorTab tab, ProfileEditorGate gate, ProfileEditorField field) noexcept {
    if (gate.enabled & field)
        return {};
    if (tab == ProfileEditorTab::guild && field == profile_editor_name)
        return utf8_literal("You don't have permission to change your nickname in this server.");
    if (tab == ProfileEditorTab::guild && field == profile_editor_accent)
        return {};
    return utf8_literal("Only with Nitro.");
}

u16 profile_editor_changes(ProfileEditorTab tab, const ProfileEditorBase& base, const ProfileEditorDraft& draft) noexcept {
    u16 changed = 0;
    if (!same(base.name, draft.name))
        changed |= profile_editor_name;
    if (!same(base.pronouns, draft.pronouns))
        changed |= profile_editor_pronouns;
    if (!same(base.bio, draft.bio))
        changed |= profile_editor_bio;
    if (image_changed(draft.avatar, base.avatar))
        changed |= profile_editor_avatar;
    if (image_changed(draft.banner, base.banner))
        changed |= profile_editor_banner;
    if (draft.decoration_set && draft.decoration_sku != base.decoration_sku)
        changed |= profile_editor_decoration;
    if (draft.effect_set && draft.effect_sku != base.effect_sku)
        changed |= profile_editor_effect;
    if (theme_changed(tab, base, draft))
        changed |= profile_editor_theme;
    if (tab == ProfileEditorTab::main && draft.accent_set &&
        (!base.has_accent || base.accent != draft.accent))
        changed |= profile_editor_accent;
    return changed;
}

bool profile_editor_bio_valid(Utf8View bio, u32 limit) noexcept {
    return within(bio, limit);
}

i32 profile_editor_bio_remaining(Utf8View bio, u32 limit) noexcept {
    const u32 used = codepoints(bio);
    return used == ~0u ? 0 : static_cast<i32>(limit) - static_cast<i32>(used);
}

bool profile_editor_changes_allowed(ProfileEditorTab tab, const ProfileEditorBase& base, const ProfileEditorDraft& draft,
                                    ProfileEditorGate gate) noexcept {
    const u16 changed = profile_editor_changes(tab, base, draft);
    for (u16 bit = 1; bit && bit <= profile_editor_bio; bit = static_cast<u16>(bit << 1)) {
        if (!(changed & bit) || (gate.enabled & bit))
            continue;
        if (!resets(tab, draft, static_cast<ProfileEditorField>(bit)))
            return false;
    }
    if (tab == ProfileEditorTab::guild && (changed & profile_editor_accent))
        return false;
    const u32 name_limit =
        tab == ProfileEditorTab::main ? kProfileEditDisplayNameCodepoints : kProfileEditNicknameCodepoints;
    return within(draft.name, name_limit) && within(draft.pronouns, kProfileEditPronounsCodepoints) &&
           profile_editor_bio_valid(draft.bio);
}

bool profile_editor_patches(ProfileEditorTab tab, const ProfileEditorBase& base, const ProfileEditorDraft& draft,
                            ProfileEditorGate gate, UserPatch* user, MemberEditPatch* member,
                            ProfilePatch* profile) noexcept {
    if (!profile || (tab == ProfileEditorTab::main ? !user : !member))
        return false;
    *profile = {};
    if (user)
        *user = {};
    if (member)
        *member = {};
    if (!profile_editor_changes_allowed(tab, base, draft, gate))
        return false;
    const u16 changed = profile_editor_changes(tab, base, draft);
    const bool new_avatar = (changed & profile_editor_avatar) && draft.avatar == ProfileImageEdit::replace;
    const bool new_banner = (changed & profile_editor_banner) && draft.banner == ProfileImageEdit::replace;
    if ((new_avatar && !draft.avatar_uri.bytes) || (new_banner && !draft.banner_uri.bytes))
        return false;
    const Utf8View avatar = new_avatar ? draft.avatar_uri : Utf8View{};
    const Utf8View description = new_avatar ? draft.avatar_description : Utf8View{};
    if (tab == ProfileEditorTab::main) {
        if (changed & profile_editor_avatar) {
            user->fields |= user_patch_avatar;
            user->avatar = avatar;
            user->avatar_description = description;
        }
        if (changed & profile_editor_name) {
            user->fields |= user_patch_global_name;
            user->global_name = draft.name;
        }
        if (changed & profile_editor_decoration) {
            user->fields |= user_patch_avatar_decoration;
            user->avatar_decoration_sku = draft.decoration_sku;
        }
    } else {
        if (changed & profile_editor_name) {
            member->fields |= member_edit_nick;
            member->nick = draft.name;
        }
        if (changed & profile_editor_avatar) {
            member->fields |= member_edit_avatar;
            member->avatar = avatar;
            member->avatar_description = description;
        }
        if (changed & profile_editor_decoration) {
            member->fields |= member_edit_avatar_decoration;
            member->avatar_decoration_sku = draft.decoration_sku;
        }
    }
    if (changed & profile_editor_banner) {
        profile->fields |= profile_patch_banner;
        profile->banner = new_banner ? draft.banner_uri : Utf8View{};
    }
    if (changed & profile_editor_bio) {
        profile->fields |= profile_patch_bio;
        profile->bio = draft.bio;
    }
    if (changed & profile_editor_pronouns) {
        profile->fields |= profile_patch_pronouns;
        profile->pronouns = draft.pronouns;
    }
    if (changed & profile_editor_accent) {
        profile->fields |= profile_patch_accent;
        profile->accent_color = draft.accent;
    }
    if (changed & profile_editor_theme) {
        profile->fields |= profile_patch_theme;
        profile->theme = !draft.theme_reset              ? ProfileThemePatch::colors
                         : tab == ProfileEditorTab::guild ? ProfileThemePatch::reset
                                                          : ProfileThemePatch::none;
        profile->theme_colors[0] = draft.theme[0];
        profile->theme_colors[1] = draft.theme[1];
    }
    if (changed & profile_editor_effect) {
        // The saved list without its effect, plus the new effect (collectibles_sku_ids).
        profile->fields |= profile_patch_collectibles;
        for (u32 i = 0; i < base.collectibles.count && profile->collectible_count < kProfileEditCollectibleMaximum; ++i)
            if (base.collectibles[i].type != ProfileCollectibleType::profile_effect && base.collectibles[i].sku)
                profile->collectibles[profile->collectible_count++] = base.collectibles[i].sku;
        if (draft.effect_sku && profile->collectible_count < kProfileEditCollectibleMaximum)
            profile->collectibles[profile->collectible_count++] = draft.effect_sku;
    }
    return true;
}

u32 profile_editor_format_color(u32 color, Slice<char> output) noexcept {
    return color > 0xFFFFFF ? 0 : color_hex_format(color << 8 | 0xFFu, false, output);
}

bool profile_editor_parse_color(Utf8View text, u32* color) noexcept {
    u32 rgba = 0;
    if (!color || !color_hex_parse(text, false, &rgba))
        return false;
    *color = rgba >> 8;
    return true;
}

} // namespace maicord

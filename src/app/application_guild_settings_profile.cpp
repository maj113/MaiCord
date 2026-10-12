#include "application_guild_settings_internal.hpp"
#include "application_account_presentation.hpp"
#include "application_settings.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {
namespace {

const GuildRecord* guild() noexcept {
    return chat_state_store_find_guild(state(), owner->guild_id);
}
bool has_banner() noexcept {
    const auto* g = guild();
    return g && g->banner_hash_bytes;
}
bool patch_pending() noexcept {
    GuildSettingsPatch patch{};
    return guild_server_profile_patch(owner->profile_base, owner->profile_draft, &patch);
}
bool name_valid() noexcept {
    return guild_server_name_valid({owner->profile_draft.name, owner->profile_draft.name_bytes});
}

void load() noexcept {
    release_picked(&owner->icon);
    release_picked(&owner->banner);
    owner->profile_base = owner->profile_draft = guild_server_profile_from_state(state(), owner->guild_id);
    owner->profile_conflict = owner->profile_echo = false;
    set_editor(editor_name, {owner->profile_draft.name, owner->profile_draft.name_bytes});
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable() && can_manage_guild();
    const auto* g = guild();
    const Utf8View guild_name = g ? bank.add(chat_state_store_guild_name(state(), *g)) : Utf8View{};
    const bool has_icon = g && g->icon_hash_bytes;
    const bool animated_icon = guild_animated_icon_available(premium_tier(), guild_features());
    const u32 header_row = push(
        {control_header, guild_name.bytes ? guild_name : utf8_literal("Server"),
         picked_text(bank, p.icon,
                     animated_icon ? utf8_literal("Server icon. PNG, GIF, JPEG or WebP; square images look best.")
                                   : utf8_literal("Server icon. PNG, JPEG or WebP; square images look best.")),
         UiSettingsControlKind::action, true, false, {}, 0, edit ? utf8_literal("Change icon") : Utf8View{}, nullptr},
        {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault});
    // The server's own icon covers the initials; a pending removal previews them.
    if (header_row < kControlCapacity && !p.icon.clear)
        p.avatar_media[header_row] = application_guild_icon_media(p.guild_id);
    if (has_icon || pending(p.icon))
        push({control_icon_remove, utf8_literal("Remove icon"), {}, UiSettingsControlKind::action,
              edit && (p.icon.set || (has_icon && !p.icon.clear)), false, {}, 0,
              p.icon.set && !has_icon ? utf8_literal("Discard") : utf8_literal("Remove"), nullptr},
             {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    if (p.profile_conflict)
        push(notice(utf8_literal("Changed elsewhere"),
                    utf8_literal("Someone else updated these settings. Load the latest values or keep editing."),
                    utf8_literal("Load latest")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    push({control_name, utf8_literal("Server name"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
          &editor_view(editor_name, control_name, utf8_literal("Server name"), edit)});
    push(category(control_banner_category, utf8_literal("Banner")));
    // BANNER or boost level 2; GIFs need ANIMATED_BANNER or level 3.
    const auto* record = guild_details();
    const bool tier_known = record && (record->known & guild_detail_premium_tier);
    const bool banner = guild_banner_available(premium_tier(), guild_features());
    const bool stored_banner = has_banner();
    push({control_banner, utf8_literal("Server Banner Background"),
          picked_text(bank, p.banner,
                      !tier_known ? utf8_literal("Checking which uploads this server allows…")
                      : banner ? utf8_literal("This image will display at the top of your channels list. The "
                                              "recommended minimum size is 960x540 and recommended aspect ratio is 16:9.")
                               : utf8_literal("Unlock with Boosting: server banners need boost level 2.")),
          UiSettingsControlKind::action, edit && connected() && banner, false, {}, 0,
          utf8_literal("Upload Background"), nullptr});
    if (stored_banner || pending(p.banner))
        push({control_banner_remove, utf8_literal("Remove banner"), {}, UiSettingsControlKind::action,
              edit && (p.banner.set || (stored_banner && !p.banner.clear)), false, {}, 0,
              p.banner.set && !stored_banner ? utf8_literal("Discard") : utf8_literal("Remove"), nullptr},
             {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    if (!can_manage_guild())
        return;
    const u64 id = activation.value.target.control_id;
    if (id == control_header) {
        if (activation.button || !activation.pointer)
            start_pick(Pick::icon);
    } else if (id == control_banner)
        start_pick(Pick::banner);
    else if (id == control_icon_remove || id == control_banner_remove) {
        auto& picked = id == control_icon_remove ? p.icon : p.banner;
        const auto* g = guild();
        const bool stored = id == control_icon_remove ? g && g->icon_hash_bytes : has_banner();
        release_picked(&picked);
        picked.clear = stored;
        repaint();
    }
}

bool dirty() noexcept {
    return !guild_server_profile_equal(owner->profile_base, owner->profile_draft) || pending(owner->icon) ||
           pending(owner->banner);
}

bool can_save() noexcept {
    return can_manage_guild() && !owner->profile_conflict && name_valid() &&
           (patch_pending() || pending(owner->icon) || pending(owner->banner));
}

// Text first, then the images.
Op next_save() noexcept {
    return patch_pending()          ? Op::server_name
           : pending(owner->icon)   ? Op::icon
           : pending(owner->banner) ? Op::banner
                                    : Op::none;
}

bool reconcile(u64 now) noexcept {
    auto& p = *owner;
    const auto current = guild_server_profile_from_state(state(), p.guild_id);
    if (p.profile_echo) {
        if (guild_server_profile_equal(current, p.profile_base) || deadline_due(now, p.profile_echo_until))
            p.profile_echo = false;
        return false;
    }
    if (guild_server_profile_equal(current, p.profile_base) || mutation_pending())
        return false;
    if (dirty()) {
        const bool changed = !p.profile_conflict;
        p.profile_conflict = true;
        return changed;
    }
    load();
    return true;
}

void notice_action() noexcept {
    if (owner->profile_conflict)
        load();
}

bool ensure_data() noexcept {
    if (owner->profile_requested || !profile_fetch_due())
        return false;
    owner->profile_requested = true;
    queue(Op::profile_get);
    return true;
}

Utf8View invalid() noexcept {
    return name_valid() ? Utf8View{} : utf8_literal("Server names need 2–100 characters.");
}

} // namespace

const PageOps kProfilePage{load,      project,   activate,    nullptr,       dirty,  can_save,
                           next_save, reconcile, ensure_data, notice_action, invalid};

} // namespace maicord::app_detail::server_settings

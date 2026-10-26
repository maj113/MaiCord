#include "application_guild_settings_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail::server_settings {
namespace {

const GuildRecord* guild() noexcept {
    return chat_state_store_find_guild(state(), owner->guild_id);
}
// The custom invite link shows only to administrators.
bool administrator() noexcept {
    return owner->access.owner || permission_has(owner->access.permissions, kPermissionAdministrator);
}
bool vanity_feature() noexcept {
    return guild_features() & guild_feature_vanity_url;
}
Utf8View stored_banner() noexcept {
    const auto* g = guild();
    return g ? chat_state_store_guild_banner_hash(*g) : Utf8View{};
}
Utf8View stored_splash() noexcept {
    const auto* record = guild_details();
    return record ? guild_detail_store_splash(*record) : Utf8View{};
}
GuildBoostDraft stored() noexcept {
    const auto* record = guild_details();
    return {GuildProfileImageEdit::keep, GuildProfileImageEdit::keep,
            record && record->premium_progress_bar_enabled};
}
bool vanity_dirty() noexcept {
    const auto& p = *owner;
    const auto text = editor_text(editor_vanity);
    return p.vanity_loaded && !(text.bytes == p.vanity_base_bytes && bytes_equal(text.data, p.vanity_base, text.bytes));
}
bool guild_dirty() noexcept {
    return !guild_boost_equal(owner->boost_base, owner->boost_draft);
}

void load() noexcept {
    auto& p = *owner;
    release_picked(&p.banner);
    release_picked(&p.splash);
    release_uri(&p.banner_uri, &p.banner_uri_bytes);
    release_uri(&p.splash_uri, &p.splash_uri_bytes);
    p.boost_base = p.boost_draft = stored();
    p.boost_echo = false;
    p.vanity_error = false;
    set_editor(editor_vanity, {p.vanity_base, p.vanity_base_bytes});
}

void image_rows(Bank& bank, u64 id, u64 remove_id, const Utf8View& label, const Utf8View& description, u64 feature,
                const Utf8View& level, GuildProfileImageEdit edit, const Picked& picked, bool stored_image,
                Utf8View* chip) noexcept {
    const bool unlocked = guild_features() & feature;
    const bool allowed = editable() && can_manage_guild();
    // "Unlocked" when the feature is present, else the boost level that brings it.
    *chip = unlocked ? utf8_literal("Unlocked") : level;
    push_choices(id, label,
                 edit == GuildProfileImageEdit::remove ? utf8_literal("Removed when you save.")
                                                       : picked_text(bank, picked, description),
                 UiSettingsControlKind::action, allowed && unlocked && connected(), {chip, 1}, 0,
                 unlocked ? utf8_literal("Upload Background") : utf8_literal("Unlock with Boosting"));
    const bool shown = edit == GuildProfileImageEdit::replace || (stored_image && edit == GuildProfileImageEdit::keep);
    if (shown)
        push_row(remove_id, utf8_literal("Remove"), {}, UiSettingsControlKind::action, allowed, false,
                 edit == GuildProfileImageEdit::replace && !stored_image ? utf8_literal("Discard")
                                                                         : utf8_literal("Remove"),
                 {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    auto& d = p.boost_draft;
    const bool edit = editable() && can_manage_guild();
    push_row(control_progress_bar, utf8_literal("Show Boost progress bar"),
             utf8_literal("Servers with the Boost progress bar enabled get more Boosts. This progress bar will display "
                          "in your channel list, attached to your server name (or server banner if you have one set)."),
             UiSettingsControlKind::toggle, edit, d.progress_bar);
    static Utf8View chips[2];
    image_rows(bank, control_banner, control_banner_remove, utf8_literal("Server Banner Background"),
               utf8_literal("This image will display at the top of your channels list. The recommended minimum size "
                            "is 960x540 and recommended aspect ratio is 16:9."),
               guild_feature_banner, utf8_literal("LVL 2"), d.banner, p.banner, stored_banner().bytes != 0, &chips[0]);
    image_rows(bank, control_splash, control_splash_remove, utf8_literal("Server Invite Background"),
               utf8_literal("This image will display when your server invite is viewed in a browser, as well as in "
                            "invite confirmation screens and Server Onboarding. The recommended minimum size is "
                            "1920x1080 and recommended aspect ratio is 16:9."),
               guild_feature_invite_splash, utf8_literal("LVL 1"), d.splash, p.splash, stored_splash().bytes != 0,
               &chips[1]);
    if (!administrator())
        return;
    push_category(control_vanity_category, utf8_literal("Custom Invite Link"),
                  utf8_literal("Bring others to your server easily with your own customized invite link. Heads up "
                               "though, anyone with the link can join and you'll need at least one text channel that "
                               "is open to all server members."));
    if (!vanity_feature()) {
        push_notice(utf8_literal("A custom invite link needs boost level 3."), utf8_literal("Unlock with Boosting"));
        return;
    }
    if (p.vanity_failed) {
        push_error(utf8_literal("Couldn't load the custom invite link"), {}, utf8_literal("Try again"));
        return;
    }
    if (!p.vanity_loaded) {
        push_loading(utf8_literal("Loading the custom invite link…"));
        return;
    }
    const u32 start = bank.used;
    bank.add("https://discord.gg/");
    bank.add(editor_text(editor_vanity));
    if (p.vanity_base_bytes) {
        bank.add(" · ");
        bank.number(p.vanity_uses);
        bank.add(p.vanity_uses == 1 ? utf8_literal(" use") : utf8_literal(" uses"));
    }
    push_editor(control_vanity, utf8_literal("Invite URL"), bank.since(start), UiSettingsControlKind::edit, edit,
                editor_vanity, {});
    if (p.vanity_error)
        push_row(control_notice, guild_vanity_error_text(p.vanity_error_code), {}, UiSettingsControlKind::error, true,
                 false, {}, {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    if (p.vanity_base_bytes)
        push_row(control_vanity_remove, utf8_literal("Remove Custom Invite Link"), {}, UiSettingsControlKind::action,
                 edit, false, utf8_literal("Remove"),
                 {UiSettingsVariant::critical, ui_settings_presentation_compact, kUiSettingsNoDefault});
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    if (!can_manage_guild())
        return;
    auto& d = p.boost_draft;
    const u64 id = activation.value.target.control_id;
    if (id == control_progress_bar) {
        d.progress_bar = activation.value.checked;
        ui_settings_scene_animate_toggle(&p.scene, activation.value.target, activation.value.checked,
                                         clock_monotonic_milliseconds());
    } else if (id == control_banner && (guild_features() & guild_feature_banner)) {
        start_pick(Pick::banner);
        return;
    } else if (id == control_splash && (guild_features() & guild_feature_invite_splash)) {
        start_pick(Pick::splash);
        return;
    } else if (id == control_banner_remove || id == control_splash_remove) {
        const bool banner = id == control_banner_remove;
        release_picked(banner ? &p.banner : &p.splash);
        if (banner)
            release_uri(&p.banner_uri, &p.banner_uri_bytes);
        else
            release_uri(&p.splash_uri, &p.splash_uri_bytes);
        const bool stored_image = (banner ? stored_banner() : stored_splash()).bytes != 0;
        (banner ? d.banner : d.splash) = stored_image ? GuildProfileImageEdit::remove : GuildProfileImageEdit::keep;
    } else if (id == control_vanity_remove && administrator() && p.vanity_loaded) {
        // The draft becomes "" and saves with the page's bar.
        set_editor(editor_vanity, {});
        p.vanity_error = false;
    } else
        return;
    repaint();
}

bool dirty() noexcept {
    return guild_dirty() || vanity_dirty();
}

bool can_save() noexcept {
    return can_manage_guild() && dirty() && (!vanity_dirty() || (administrator() && vanity_feature()));
}

// The vanity PATCH first; the guild PATCH follows only when it succeeded.
Op next_save() noexcept {
    return vanity_dirty() && administrator() ? Op::vanity_update : guild_dirty() ? Op::boost_update : Op::none;
}

bool reconcile(u64 now) noexcept {
    auto& p = *owner;
    const auto current = stored();
    if (p.boost_echo) {
        if (current.progress_bar == p.boost_base.progress_bar || deadline_due(now, p.boost_echo_until))
            p.boost_echo = false;
        return false;
    }
    if (current.progress_bar == p.boost_base.progress_bar || mutation_pending() || guild_dirty())
        return false;
    p.boost_base.progress_bar = p.boost_draft.progress_bar = current.progress_bar;
    return true;
}

// The custom invite link is read once per settings visit.
bool ensure_data() noexcept {
    auto& p = *owner;
    if (!administrator() || p.vanity_requested)
        return false;
    p.vanity_requested = true;
    p.vanity_failed = false;
    queue(Op::vanity_get);
    return true;
}

void notice_action() noexcept {
    auto& p = *owner;
    if (!p.vanity_failed)
        return;
    p.vanity_requested = false;
    if (!busy() && connected())
        ensure_data();
}

// A new banner's data URI as the banner upload prepares it.
bool prepare_banner() noexcept {
    auto& p = *owner;
    if (p.banner_uri)
        return true;
    if (!p.banner.data || !g_app.account.api.guild_assets)
        return false;
    const u32 bytes = guild_image_data_uri_bytes(p.banner.info.kind, p.banner.info.bytes);
    auto* uri = bytes ? static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                      : nullptr;
    if (!uri)
        return false;
    GuildImageInfo info{};
    GuildAssetRequest request{};
    request.operation = GuildAssetOperation::image_probe;
    request.profile = GuildImageProfile::banner;
    request.image = {p.banner.data, p.banner.bytes};
    request.info = &info;
    request.data_uri = {uri, bytes};
    if (!g_app.account.api.guild_assets(g_app.account.instance, &request, 0).ok() ||
        info.error != GuildImageError::none) {
        VirtualFree(uri, 0, MEM_RELEASE);
        return false;
    }
    p.banner_uri = uri;
    p.banner_uri_bytes = bytes;
    return true;
}

// A new invite splash is the file itself as a data URI.
bool prepare_splash() noexcept {
    auto& p = *owner;
    if (p.splash_uri)
        return true;
    const auto kind = p.splash.info.source_kind;
    const u32 bytes = p.splash.data ? guild_image_data_uri_bytes(kind, p.splash.bytes) : 0;
    auto* uri = bytes ? static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                      : nullptr;
    if (!uri)
        return false;
    if (guild_image_write_data_uri(kind, {p.splash.data, p.splash.bytes}, {uri, bytes}) != bytes) {
        VirtualFree(uri, 0, MEM_RELEASE);
        return false;
    }
    p.splash_uri = uri;
    p.splash_uri_bytes = bytes;
    return true;
}

} // namespace

void release_uri(u8** uri, u32* bytes) noexcept {
    if (*uri) {
        secure_clear_bytes(*uri, *bytes);
        VirtualFree(*uri, 0, MEM_RELEASE);
    }
    *uri = nullptr;
    *bytes = 0;
}

void boost_vanity_loaded(const GuildAdminVanity* vanity) noexcept {
    auto& p = *owner;
    p.vanity_loaded = vanity != nullptr;
    p.vanity_failed = vanity == nullptr;
    if (!vanity)
        return;
    const bool keep_draft = p.page == Page::boost_perks && vanity_dirty();
    copy_bytes(p.vanity_base, vanity->code, vanity->code_bytes);
    p.vanity_base_bytes = vanity->code_bytes;
    p.vanity_uses = vanity->uses;
    if (!keep_draft)
        set_editor(editor_vanity, {p.vanity_base, p.vanity_base_bytes});
}

bool boost_vanity_patch(GuildSettingsPatch* patch) noexcept {
    if (!administrator() || !vanity_dirty())
        return false;
    reset_value(patch);
    patch->fields = guild_settings_patch_vanity;
    patch->vanity_code = editor_text(editor_vanity);
    return true;
}

bool boost_guild_patch(GuildSettingsPatch* patch) noexcept {
    auto& p = *owner;
    const auto& d = p.boost_draft;
    if ((d.banner == GuildProfileImageEdit::replace && !prepare_banner()) ||
        (d.splash == GuildProfileImageEdit::replace && !prepare_splash()))
        return false;
    if (!guild_boost_patch(d, stored_banner(), stored_splash(),
                           {reinterpret_cast<const char*>(p.banner_uri), p.banner_uri_bytes},
                           {reinterpret_cast<const char*>(p.splash_uri), p.splash_uri_bytes}, patch))
        return false;
    if (d.banner == GuildProfileImageEdit::replace)
        patch->banner_original = {p.banner.data, p.banner.bytes};
    if (d.splash == GuildProfileImageEdit::replace)
        patch->splash_original = {p.splash.data, p.splash.bytes};
    return true;
}

void boost_saved(Op op, bool ok, const Utf8View& body, u64 now) noexcept {
    auto& p = *owner;
    if (op == Op::vanity_update) {
        if (ok) {
            GuildAdminVanity vanity{};
            const auto text = editor_text(editor_vanity);
            if (guild_admin_decode_vanity(body, {p.scratch, kScratchUnits}, &vanity).ok()) {
                copy_bytes(p.vanity_base, vanity.code, vanity.code_bytes);
                p.vanity_base_bytes = vanity.code_bytes;
                p.vanity_uses = vanity.uses;
            } else {
                copy_bytes(p.vanity_base, text.data, text.bytes);
                p.vanity_base_bytes = static_cast<u8>(text.bytes);
            }
            set_editor(editor_vanity, {p.vanity_base, p.vanity_base_bytes});
            p.vanity_error = false;
        } else {
            p.vanity_error = true;
            p.vanity_error_code = body.bytes ? guild_asset_error_code(body, {p.scratch, kScratchUnits}) : 0;
        }
        return;
    }
    if (!ok)
        return;
    release_picked(&p.banner);
    release_picked(&p.splash);
    release_uri(&p.banner_uri, &p.banner_uri_bytes);
    release_uri(&p.splash_uri, &p.splash_uri_bytes);
    p.boost_draft.banner = p.boost_draft.splash = GuildProfileImageEdit::keep;
    p.boost_base = p.boost_draft;
    p.boost_echo = true;
    p.boost_echo_until = deadline_after(now, kEchoMs);
}

const PageOps kBoostPage{load,      project,   activate,    nullptr,       dirty,  can_save,
                         next_save, reconcile, ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings

#include "application_guild_settings_internal.hpp"
#include "maicord/core/memory.hpp"
#include "application_settings.hpp"
#include "maicord/app/account_profile_edit.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail::server_settings {
namespace {

// The profile is read again only when older than this.
constexpr u64 kProfileFreshMs = 60'000;
// Any editor size: the default crop is the centred square at zoom 1.
constexpr float kCropEditorWidth = 432, kCropEditorHeight = 350;

// Kept across openings, like the official client's profile store.
struct ProfileCache {
    u64 guild_id;
    u64 fetched_at;
    bool ok;
    GuildAdminProfile profile;
} cache;

const GuildRecord* guild() noexcept {
    return chat_state_store_find_guild(state(), owner->guild_id);
}
bool has_icon() noexcept {
    const auto& d = owner->profile_draft;
    return d.icon == GuildProfileImageEdit::replace ||
           (d.icon == GuildProfileImageEdit::keep && d.profile.icon_bytes);
}
bool name_valid() noexcept {
    const auto& p = owner->profile_draft.profile;
    return !p.name_known || guild_server_name_valid({p.name, p.name_bytes});
}
bool read_pending() noexcept {
    return busy() && owner->request.op == Op::profile_get;
}

// The picked icon with the data URI made from it.
void release_icon_pick() noexcept {
    release_picked(&owner->icon);
    release_uri(&owner->icon_uri, &owner->icon_uri_bytes);
}
void load() noexcept {
    auto& p = *owner;
    release_icon_pick();
    const auto* profile = server_profile();
    p.profile_loaded = profile != nullptr;
    p.profile_base = p.profile_draft = profile ? guild_server_profile_draft(*profile) : GuildServerProfileDraft{};
    const auto& d = p.profile_draft.profile;
    set_editor(editor_name, {d.name, d.name_bytes});
    set_editor(editor_description, {d.description, d.description_bytes});
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    if (!p.profile_loaded) {
        bool settled = false;
        server_profile(&settled);
        if (settled && !read_pending())
            push_error(utf8_literal("Something went wrong"), utf8_literal("We couldn't load the profile for this server."),
                       utf8_literal("Retry"));
        else
            push_loading(utf8_literal("Loading the server profile…"));
        return;
    }
    const bool edit = editable() && can_manage_guild();
    auto& d = p.profile_draft;
    push_editor(control_name, utf8_literal("Name"), {}, UiSettingsControlKind::edit, edit, editor_name,
                utf8_literal("Name"));

    push_category(control_banner_category, utf8_literal("Icon"));
    const auto* g = guild();
    const Utf8View guild_name = g ? bank.add(chat_state_store_guild_name(state(), *g)) : Utf8View{};
    const Utf8View icon_text =
        d.icon == GuildProfileImageEdit::remove ? utf8_literal("Removed when you save.")
        : d.icon == GuildProfileImageEdit::replace
            ? [&]() noexcept {
                  const u32 start = bank.used;
                  bank.add("New: ");
                  bank.number(p.icon_info.width);
                  bank.add("×");
                  bank.number(p.icon_info.height);
                  bank.add(". Save to apply.");
                  return bank.since(start);
              }()
            : utf8_literal("We recommend an image of at least 512x512.");
    const u32 header_row =
        push_row(control_header, guild_name.bytes ? guild_name : utf8_literal("Server"), icon_text,
                 UiSettingsControlKind::action, true, false, edit ? utf8_literal("Change Server Icon") : Utf8View{},
                 {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault});
    // The stored icon until a save replaces or removes it.
    if (header_row < kControlCapacity && d.icon == GuildProfileImageEdit::keep && d.profile.icon_bytes)
        p.avatar_media[header_row] = application_guild_icon_media(p.guild_id);
    if (has_icon())
        push_row(control_icon_remove, utf8_literal("Remove Icon"), {}, UiSettingsControlKind::action, edit, false,
                 utf8_literal("Remove"),
                 {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});

    // "Server Icon Color" and the presets; an uploaded banner selects none.
    auto& names = p.color_names;
    names[0] = utf8_literal("Server Icon Color");
    for (u32 i = 0; i < kGuildProfileBannerColorCount; ++i)
        names[i + 1] = kGuildProfileBannerNames[i];
    const u32 banner = guild_server_profile_banner_choice(d);
    push_choices(control_banner_colors, utf8_literal("Banner"), {}, UiSettingsControlKind::swatches, edit,
                 {names, kGuildProfileBannerColorCount + 1}, banner == ~0u ? kUiSettingsNoChoice : banner);

    push_editor(control_description, utf8_literal("Description"),
                utf8_literal("How did your server get started? Why should people join?"),
                UiSettingsControlKind::multiline, edit, editor_description,
                utf8_literal("Tell the world a bit about this server."));

    const bool discoverable = d.profile.discoverable;
    push_row(control_private, utf8_literal("Private Profile"),
             discoverable ? utf8_literal("Discoverable servers cannot have their content hidden. You can change this "
                                         "setting in Access")
                          : utf8_literal("When enabled, only server members can view profile content. Non-members won't "
                                         "be able to see this content unless they have an invite."),
             UiSettingsControlKind::toggle, edit && !discoverable, guild_server_profile_private(d));
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    if (!can_manage_guild() || !p.profile_loaded)
        return;
    const u64 id = activation.value.target.control_id;
    auto& d = p.profile_draft;
    if (id == control_header) {
        if (activation.button || !activation.pointer)
            start_pick(Pick::icon);
        return;
    }
    if (id == control_icon_remove) {
        release_icon_pick();
        d.icon = p.profile_base.profile.icon_bytes ? GuildProfileImageEdit::remove : GuildProfileImageEdit::keep;
    } else if (id == control_private && !d.profile.discoverable) {
        guild_server_profile_set_private(&d, p.profile_base.profile, activation.value.checked);
        ui_settings_scene_animate_toggle(&p.scene, activation.value.target, activation.value.checked,
                                         clock_monotonic_milliseconds());
    } else if (id == control_banner_colors)
        guild_server_profile_set_banner_choice(&d, activation.value.selected_choice);
    else
        return;
    repaint();
}

bool dirty() noexcept {
    return owner->profile_loaded && !guild_server_profile_equal(owner->profile_base, owner->profile_draft);
}

bool can_save() noexcept {
    const auto& d = owner->profile_draft.profile;
    return can_manage_guild() && dirty() && name_valid() &&
           guild_server_description_valid({d.description, d.description_bytes});
}

Op next_save() noexcept {
    return dirty() ? Op::profile_update : Op::none;
}

bool ensure_data() noexcept {
    if (owner->profile_requested || !profile_fetch_due())
        return false;
    owner->profile_requested = true;
    queue(Op::profile_get);
    return true;
}

void notice_action() noexcept {
    if (owner->profile_loaded || read_pending())
        return;
    // Retry forces a read.
    cache.fetched_at = 0;
    owner->profile_requested = false;
    if (!busy() && connected())
        ensure_data();
}

Utf8View invalid() noexcept {
    return name_valid() ? Utf8View{} : utf8_literal("Server names need 2–100 characters.");
}

// The new icon's data URI, cropped as the official client's cropper does by
// default: the centred square, a PNG for stills and a GIF for animated GIFs.
bool prepare_icon() noexcept {
    auto& p = *owner;
    if (p.icon_uri)
        return true;
    if (!p.icon.data || !g_app.account.api.profile_image)
        return false;
    const auto state = image_crop_begin(ImageCropUse::avatar, p.icon_info.width, p.icon_info.height,
                                        kCropEditorWidth, kCropEditorHeight);
    const u32 capacity = account_profile_image_uri_capacity(p.icon_info);
    auto* uri = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!uri)
        return false;
    ProfileImageRequest request{};
    request.source = {p.icon.data, p.icon.bytes};
    request.crop = image_crop_output(state);
    ProfileImageInfo info{};
    request.info = &info;
    request.data_uri = {uri, capacity};
    if (!g_app.account.api.profile_image(g_app.account.instance, &request) || !request.data_uri_bytes) {
        VirtualFree(uri, 0, MEM_RELEASE);
        return false;
    }
    p.icon_uri = uri;
    p.icon_uri_bytes = request.data_uri_bytes;
    return true;
}

} // namespace

const GuildAdminProfile* server_profile(bool* settled) noexcept {
    const bool current = owner && cache.guild_id == owner->guild_id && cache.fetched_at;
    if (settled)
        *settled = current;
    return current && cache.ok ? &cache.profile : nullptr;
}

bool profile_fetch_due() noexcept {
    return !owner || cache.guild_id != owner->guild_id || !cache.fetched_at ||
           deadline_due(clock_monotonic_milliseconds(), deadline_after(cache.fetched_at, kProfileFreshMs));
}

void server_profile_loaded(const GuildAdminProfile* profile) noexcept {
    // A failed read keeps the last good profile of this guild.
    const bool keep = !profile && cache.ok && cache.guild_id == owner->guild_id;
    cache.guild_id = owner->guild_id;
    cache.fetched_at = clock_monotonic_milliseconds();
    if (profile) {
        cache.profile = *profile;
        cache.ok = true;
    } else if (!keep)
        cache.ok = false;
    auto& p = *owner;
    // A clean or unloaded page takes the new read.
    if (p.page == Page::profile && (!p.profile_loaded || !dirty()))
        load();
}

bool profile_patch(GuildSettingsPatch* patch, GuildServerProfileWrite* write,
                   Slice<GuildServerProfileTraitWrite> traits) noexcept {
    auto& p = *owner;
    if (!p.profile_loaded || (p.profile_draft.icon == GuildProfileImageEdit::replace && !prepare_icon()))
        return false;
    if (!guild_server_profile_write(p.profile_draft, {reinterpret_cast<const char*>(p.icon_uri), p.icon_uri_bytes},
                                    state(), p.guild_id, traits, write))
        return false;
    reset_value(patch);
    patch->fields = guild_settings_patch_profile;
    patch->profile = write;
    return true;
}

void profile_saved(bool ok, const Utf8View& body) noexcept {
    auto& p = *owner;
    if (!ok)
        return;
    GuildAdminProfile saved{};
    if (guild_admin_decode_profile(body, {p.scratch, kScratchUnits}, &saved).ok()) {
        cache.guild_id = p.guild_id;
        cache.fetched_at = clock_monotonic_milliseconds();
        cache.profile = saved;
        cache.ok = true;
        load();
        return;
    }
    // An unreadable reply still saved the draft.
    release_icon_pick();
    p.profile_draft.icon = GuildProfileImageEdit::keep;
    p.profile_base = p.profile_draft;
}

const PageOps kProfilePage{load,      project, activate,    nullptr,       dirty,  can_save,
                           next_save, nullptr, ensure_data, notice_action, invalid};

} // namespace maicord::app_detail::server_settings

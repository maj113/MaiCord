#include "profile_editor_internal.hpp"
#include "maicord/os/text.hpp"
#include "../chat/composer/text_input.hpp"
#include "../shell/overlay_input.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/model/chat/asset_hash.hpp"
#include "maicord/model/guild/member_moderation.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/local_draft_preview.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord::app_detail {
namespace {

constexpr float kPreviewWidth = 300;
// Local previews of picked images use their own key space ("PR" ids) with
// the thumbnail bit, so composer and forum previews never collide with them.
constexpr u64 kPreviewIdBase = 0x5052000000000000ull;

static_assert(ui_settings_style_valid(kEditorStyle));

} // namespace
namespace profile_editor_detail {

Owner* owner;
u64 lifetime_serial;

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready;
}
ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
bool scope_current() noexcept {
    if (!owner || !application_account_available() || owner->account_scope != g_app.plugin_account_scope_serial ||
        owner->profile_id != g_app.account.status.profile_id || state().self_user_id != owner->self_user_id)
        return false;
    if (owner->tab != ProfileEditorTab::guild || !owner->guild_id)
        return true;
    const auto* guild = chat_state_store_find_guild(state(), owner->guild_id);
    return guild && guild->generation == owner->guild_generation && !guild->unavailable;
}
bool busy() noexcept {
    return owner && (owner->queued || owner->request_id);
}
bool saving() noexcept {
    return owner && owner->saving;
}
bool editable() noexcept {
    return owner && owner->scene.active && owner->loaded && !saving() && owner->dialog_kind == Dialog::none;
}

} // namespace profile_editor_detail
namespace {

bool nitro() noexcept {
    return (owner->loaded ? owner->saved.premium_type : state().self_premium_type) == kProfileEditorNitroPremiumType;
}
bool can_change_nickname() noexcept {
    if (owner->tab != ProfileEditorTab::guild || !owner->guild_id)
        return false;
    const auto& ui = *g_app.account_ui;
    return moderation_capabilities(ui.chat_state, ui.guild_member_store, owner->guild_id, owner->self_user_id,
                                   clock_unix_milliseconds() / 1000)
        .nickname_self;
}

} // namespace
namespace profile_editor_detail {

ProfileEditorGate gate() noexcept {
    return profile_editor_gate(owner->tab, owner->loaded ? owner->saved.premium_type : state().self_premium_type,
                               can_change_nickname());
}

} // namespace profile_editor_detail
namespace {

// The drafted decoration or effect SKU, else the saved one.
u64 current_sku(bool decoration) noexcept {
    const auto& d = owner->draft;
    const auto& b = owner->base;
    return decoration ? (d.decoration_set ? d.decoration_sku : b.decoration_sku)
                      : (d.effect_set ? d.effect_sku : b.effect_sku);
}

struct Bank {
    char* data;
    u32 capacity, used;
    Utf8View add(const Utf8View& text) noexcept {
        const u32 take = minimum(text.bytes, capacity - used);
        copy_bytes(data + used, text.data, take);
        used += take;
        return {data + used - take, take};
    }
    Utf8View number(i64 value) noexcept {
        const u32 start = used;
        if (value < 0 && used < capacity) {
            data[used++] = '-';
            value = -value;
        }
        char digits[20];
        u32 count = 0;
        u64 v = static_cast<u64>(value);
        do {
            digits[count++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (count && used < capacity)
            data[used++] = digits[--count];
        return {data + start, used - start};
    }
    Utf8View since(u32 start) const noexcept {
        return {data + start, used - start};
    }
};
template <u32 N> void assign(ProfileString<N>* to, const Utf8View& from) noexcept {
    to->bytes = static_cast<u16>(utf8_copy_truncated(to->data, from));
}

} // namespace
namespace profile_editor_detail {

char* editor_buffer(u32 index) noexcept {
    return index == editor_name ? owner->name : index == editor_pronouns ? owner->pronouns : owner->bio;
}
u32 editor_capacity(u32 index) noexcept {
    return index == editor_name ? kNameBytes : index == editor_pronouns ? kPronounsBytes : kBioBytes;
}

} // namespace profile_editor_detail
namespace {

// The official client allows typing past the bio limit (the counter goes negative and
// Save disables); the name and pronouns inputs stop at their maxLength.
u32 editor_limit(u32 index) noexcept {
    return index == editor_name       ? (owner->tab == ProfileEditorTab::main ? kProfileEditDisplayNameCodepoints
                                                                              : kProfileEditNicknameCodepoints)
           : index == editor_pronouns ? kProfileEditPronounsCodepoints
                                      : kProfileBioMaximumCodepoints;
}

} // namespace
namespace profile_editor_detail {

void load_editor(u32 index, const Utf8View& text) noexcept {
    const u32 bytes = utf8_copy_truncated(editor_buffer(index), editor_capacity(index), text);
    Editors::load(index, bytes, editor_limit(index), index == editor_bio);
}

} // namespace profile_editor_detail
namespace {

Utf8View editor_text(u32 index) noexcept {
    return message_composer_text(owner->models[index]);
}

} // namespace
namespace profile_editor_detail {

u32 editor_of(u64 control) noexcept {
    return control == control_name       ? editor_name
           : control == control_pronouns ? editor_pronouns
           : control == control_bio      ? editor_bio
                                         : editor_count;
}

} // namespace profile_editor_detail
namespace {

void release_uri(Picked& picked) noexcept {
    if (picked.uri) {
        SecureZeroMemory(picked.uri, picked.uri_bytes);
        pages_release(picked.uri);
    }
    picked.uri = nullptr;
    picked.uri_bytes = 0;
    picked.has_md5 = false;
}
void retire_preview(Picked& picked) noexcept {
    if (picked.admitted && picked.instance == g_app.account.instance && g_app.account.api.media_local_preview) {
        AccountMediaLocalPreviewRequest exact{};
        exact.key = picked.request.key;
        exact.owner_scope = picked.request.owner_scope;
        exact.owner_generation = picked.request.owner_generation;
        g_app.account.api.media_local_preview(picked.instance, AccountLocalPreviewOperation::retire, &exact, 0, 0);
    }
    if (g_app.renderer.preview_media_atlas && media_asset_key_valid(picked.request.key))
        media_atlas_unpin(g_app.renderer.preview_media_atlas, picked.request.key);
}

} // namespace
namespace profile_editor_detail {

void release_picked(Picked& picked) noexcept {
    retire_preview(picked);
    release_uri(picked);
    if (picked.data) {
        SecureZeroMemory(picked.data, picked.bytes);
        pages_release(picked.data);
    }
    // Paths are long; clear only the header fields and the used path prefix.
    clear_bytes(picked.path, picked.path_units * static_cast<u32>(sizeof(wchar_t)));
    picked.data = nullptr;
    picked.bytes = picked.path_units = picked.description_bytes = 0;
    reset_value(&picked.info);
    reset_value(&picked.crop);
    reset_value(&picked.request);
    picked.instance = nullptr;
    picked.poll_at = 0;
    picked.admitted = picked.ready = picked.failed = false;
}

} // namespace profile_editor_detail
namespace {

void move_picked(Picked& to, Picked& from) noexcept {
    release_picked(to);
    copy_bytes(&to, &from, static_cast<u32>(offsetof(Picked, path)));
    copy_bytes(to.path, from.path, from.path_units * static_cast<u32>(sizeof(wchar_t)));
    // The source keeps no ownership.
    from.data = nullptr;
    from.uri = nullptr;
    from.admitted = false;
    reset_value(&from.request);
    release_picked(from);
}

Utf8View saved_name() noexcept {
    const auto& s = owner->saved;
    return owner->tab == ProfileEditorTab::main ? s.global_name.view() : s.nick.view();
}
void build_base() noexcept {
    auto& p = *owner;
    const auto& s = p.saved;
    auto& b = p.base;
    reset_value(&b);
    const bool guild = p.tab == ProfileEditorTab::guild;
    b.name = saved_name();
    b.pronouns = guild ? s.member_pronouns.view() : s.pronouns.view();
    b.bio = guild ? s.member_bio.view() : s.bio.view();
    b.avatar = guild ? s.member_avatar.bytes != 0 : s.avatar.bytes != 0;
    b.banner = guild ? s.member_banner.bytes != 0 : s.banner.bytes != 0;
    b.decoration_sku = guild ? s.member_decoration_sku : s.decoration_sku;
    const u8 count = guild ? s.member_collectible_count : s.collectible_count;
    for (u32 i = 0; i < count; ++i)
        p.base_collectibles[i] = guild ? s.member_collectibles[i] : s.collectibles[i];
    b.collectibles = {p.base_collectibles, count};
    b.effect_sku = profile_edit_collectible(b.collectibles, ProfileCollectibleType::profile_effect);
    b.has_theme = guild ? s.has_member_theme : s.has_theme;
    b.theme[0] = guild ? s.member_theme_colors[0] : s.theme_colors[0];
    b.theme[1] = guild ? s.member_theme_colors[1] : s.theme_colors[1];
    b.has_accent = !guild && s.has_accent;
    b.accent = guild ? 0 : s.accent_color;
}

} // namespace
namespace profile_editor_detail {

ProfileEditorDraft current_draft() noexcept {
    auto d = owner->draft;
    d.name = editor_text(editor_name);
    d.pronouns = editor_text(editor_pronouns);
    d.bio = editor_text(editor_bio);
    d.avatar_uri = {reinterpret_cast<const char*>(owner->avatar.uri), owner->avatar.uri_bytes};
    d.avatar_description = {owner->avatar.description, owner->avatar.description_bytes};
    d.banner_uri = {reinterpret_cast<const char*>(owner->banner.uri), owner->banner.uri_bytes};
    if (owner->avatar.has_md5)
        d.avatar_md5 = {owner->avatar.md5, 32};
    if (owner->banner.has_md5)
        d.banner_md5 = {owner->banner.md5, 32};
    return d;
}
u16 changes() noexcept {
    return owner && owner->loaded ? profile_editor_changes(owner->tab, owner->base, current_draft()) : 0;
}
// Fresh draft from the saved values (Reset, a tab or server change, a save).
void discard() noexcept {
    auto& p = *owner;
    application_composer_text_input_suspend();
    build_base();
    load_editor(editor_name, p.base.name);
    load_editor(editor_pronouns, p.base.pronouns);
    load_editor(editor_bio, p.base.bio);
    release_picked(p.avatar);
    release_picked(p.banner);
    reset_value(&p.draft);
    p.draft.theme[0] = p.base.theme[0];
    p.draft.theme[1] = p.base.theme[1];
    p.draft.accent = p.base.accent;
    p.draft.decoration_sku = p.base.decoration_sku;
    p.draft.effect_sku = p.base.effect_sku;
    const bool guild = p.tab == ProfileEditorTab::guild;
    assign(&p.decoration_asset, guild && p.saved.member_decoration_asset.bytes ? p.saved.member_decoration_asset.view()
                                                                               : p.saved.decoration_asset.view());
    reset_value(&p.errors);
    p.failed = false;
    reset_value(&p.status);
}

u32 effective_theme(u32* out) noexcept {
    const auto& d = owner->draft;
    const auto& b = owner->base;
    if (d.theme_set && !d.theme_reset) {
        out[0] = d.theme[0];
        out[1] = d.theme[1];
        return 2;
    }
    if (d.theme_set && d.theme_reset)
        return 0;
    if (b.has_theme) {
        out[0] = b.theme[0];
        out[1] = b.theme[1];
        return 2;
    }
    return 0;
}

} // namespace profile_editor_detail
namespace {

// The saved profile with every pending change applied, as the popout
// would show it after Save (the official client feeds its popout the pending values).
void compose_preview() noexcept {
    auto& p = *owner;
    auto& v = p.preview_profile;
    v = p.base_profile;
    const auto d = current_draft();
    const bool guild = p.tab == ProfileEditorTab::guild;
    v.user_id = p.self_user_id;
    v.guild_id = guild ? p.guild_id : 0;
    if (!guild) {
        assign(&v.display_name, d.name.bytes ? d.name : v.username.view());
        assign(&v.pronouns, d.pronouns);
        assign(&v.bio, d.bio);
        reset_value(&v.guild_bio);
        reset_value(&v.guild_pronouns);
        reset_value(&v.nickname);
    } else {
        assign(&v.nickname, d.name);
        assign(&v.guild_pronouns, d.pronouns);
        assign(&v.guild_bio, d.bio);
    }
    // A response without the avatar hash shows the cached user's, as the
    // popout seeds its card.
    if (!v.avatar.bytes && g_app.account_ui)
        if (const auto* user = user_store_find(g_app.account_ui->user_store, p.self_user_id))
            assign(&v.avatar, user_store_avatar_hash(*user));
    if (d.avatar == ProfileImageEdit::remove) {
        if (guild)
            reset_value(&v.guild_appearance.avatar);
        else
            reset_value(&v.avatar);
    }
    if (d.banner == ProfileImageEdit::remove) {
        if (guild)
            reset_value(&v.guild_banner);
        else
            reset_value(&v.banner);
    }
    u32 colors[2]{};
    const bool theme = effective_theme(colors) == 2;
    if (guild) {
        v.has_guild_theme = theme;
        v.guild_theme_colors[0] = colors[0];
        v.guild_theme_colors[1] = colors[1];
    } else {
        v.has_theme = theme;
        v.theme_colors[0] = colors[0];
        v.theme_colors[1] = colors[1];
        v.accent_color = d.accent_set ? d.accent : p.base.accent;
    }
    // The drafted decoration (asset of an owned item, or none).
    auto& appearance = guild ? v.guild_appearance : v.user_appearance;
    const bool decorated = d.decoration_set ? d.decoration_sku && p.decoration_asset.bytes : p.decoration_asset.bytes;
    appearance.decoration = decorated ? p.decoration_asset : ProfileString<64>{};
    if (decorated)
        appearance.fields |= appearance_decoration;
    const u64 effect = current_sku(false);
    if (guild)
        v.guild_effect_sku_id = effect;
    else
        v.effect_sku_id = effect;
    p.effect_wanted = effect;
}
// The aside heading for the current tab.
Utf8View preview_heading(Bank& bank) noexcept {
    if (owner->tab == ProfileEditorTab::main)
        return utf8_literal("Preview");
    const auto* guild = chat_state_store_find_guild(state(), owner->guild_id);
    const u32 start = bank.used;
    bank.add(utf8_literal("Preview For "));
    if (guild)
        bank.add(chat_state_store_guild_name(state(), *guild));
    return bank.since(start);
}
// Lays the card out at anchor. visible is the page area it may draw in (the
// aside's clip), so a scrolled card is cut at the scroller's edges; empty
// while the aside is out of view.
bool layout_preview(const UiRect& anchor, const UiRect& visible) noexcept {
    auto& p = *owner;
    if (!p.loaded)
        return false;
    compose_preview();
    auto& s = p.preview;
    s.presentation = ProfilePresentation::preview;
    reset_value(&s.pending_avatar);
    reset_value(&s.pending_banner);
    if (p.draft.avatar == ProfileImageEdit::replace && p.avatar.ready)
        s.pending_avatar = {p.avatar.request.key, p.avatar.crop, true};
    if (p.draft.banner == ProfileImageEdit::replace && p.banner.ready)
        s.pending_banner = {p.banner.request.key, p.banner.crop, true};
    s.measure = &g_app.account_ui->text_measure_cache;
    s.effect = p.effect.sku && p.effect.sku == p.effect_wanted ? &p.effect : nullptr;
    s.reduced_motion = application_reduced_motion();
    s.phase = ProfilePhase::ready;
    s.presence = UiPresence::online;
    const auto viewport = ui_rect_has_area(visible) ? visible : application_viewport_dips();
    if (!p.preview_open) {
        if (!profile_scene_open(&s, &p.preview_profile, &state(), &g_app.account_ui->user_store, p.self_user_id,
                                anchor, viewport))
            return false;
        p.preview_open = true;
        return true;
    }
    s.anchor = anchor;
    return profile_scene_layout(&s, viewport, true);
}

MediaAttachmentFormat format_of(GuildImageKind kind) noexcept {
    switch (kind) {
    case GuildImageKind::png:
    case GuildImageKind::apng:
        return MediaAttachmentFormat::png;
    case GuildImageKind::gif:
        return MediaAttachmentFormat::gif;
    case GuildImageKind::jpeg:
        return MediaAttachmentFormat::jpeg;
    case GuildImageKind::webp:
        return MediaAttachmentFormat::webp;
    default:
        return MediaAttachmentFormat::unknown;
    }
}

} // namespace
namespace profile_editor_detail {

bool request_preview(Picked& picked, const FileSnapshot& snapshot) noexcept {
    const u32 generation = ++owner->preview_serial | kLocalDraftPreviewThumbnailVariant;
    const auto key = media_asset_local_draft_preview(kPreviewIdBase | owner->preview_serial, generation,
                                                     format_of(picked.info.kind));
    reset_value(&picked.request);
    picked.request.key = key;
    picked.request.snapshot = snapshot;
    picked.request.path = {picked.path, picked.path_units};
    picked.request.owner_scope = kPreviewIdBase | (owner->profile_id & 0xffffffffffffull);
    picked.request.owner_generation = generation;
    picked.instance = g_app.account.instance;
    picked.ready = picked.failed = picked.admitted = false;
    picked.poll_at = 0;
    return media_asset_key_valid(key) && local_draft_preview_request_valid(picked.request);
}
Picked* picked_for(MediaAssetKey key) noexcept {
    if (!owner || !media_asset_key_valid(key))
        return nullptr;
    Picked* candidates[] = {&owner->staged, &owner->avatar, &owner->banner};
    for (auto* picked : candidates)
        if (picked->data && media_asset_key_equal(picked->request.key, key))
            return picked;
    return nullptr;
}
bool pump_previews(u64 now) noexcept {
    if (!owner || !g_app.account.api.media_local_preview || !g_app.account.instance)
        return false;
    bool changed = false;
    Picked* candidates[] = {&owner->staged, &owner->avatar, &owner->banner};
    const u64 unix_now = clock_unix_milliseconds() / 1000;
    for (auto* picked : candidates) {
        if (!picked->data || picked->failed || !media_asset_key_valid(picked->request.key))
            continue;
        if (picked->ready && g_app.renderer.preview_media_atlas &&
            !media_atlas_peek(g_app.renderer.preview_media_atlas, picked->request.key).ok()) {
            picked->ready = false;
            picked->poll_at = 0;
        }
        if (picked->ready || now < picked->poll_at)
            continue;
        picked->request.path = {picked->path, picked->path_units};
        const auto result = g_app.account.api.media_local_preview(
            picked->instance, AccountLocalPreviewOperation::request, &picked->request, now, unix_now);
        if (result.state == AccountMediaRequestState::queued || result.state == AccountMediaRequestState::pending ||
            result.state == AccountMediaRequestState::ready)
            picked->admitted = true;
        else if (result.state != AccountMediaRequestState::busy) {
            picked->failed = true;
            changed = true;
            continue;
        }
        picked->poll_at = deadline_after(now, 100);
    }
    return changed;
}

} // namespace profile_editor_detail
namespace {

void item_media(UiProfileDialogItem* item, const Utf8View& url, bool png) noexcept {
    assign(&item->url, url);
    item->key = media_asset_embed(hash64_bytes(kHash64Seed, url.data, url.bytes), 1, 0,
                                  MediaEmbedResource::image,
                                  png ? MediaAttachmentFormat::png : MediaAttachmentFormat::webp, false,
                                  MediaResolutionTier::px512);
    item->key.kind = MediaAssetKind::profile_image;
}
// The static decoration preset URL at the picker's 80 DIP tile.
u32 decoration_url(Utf8View asset, Slice<char> out) noexcept {
    ProtocolWriter w{{reinterpret_cast<u8*>(out.data), out.count}, 0};
    const bool ok = protocol_writer_append_literal(&w, "https://cdn.discordapp.com/avatar-decoration-presets/") &&
                    protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(asset.data), asset.bytes) &&
                    protocol_writer_append_literal(&w, ".png?size=96&passthrough=false");
    return ok ? w.used : 0;
}

} // namespace
namespace profile_editor_detail {

void build_items(Dialog kind) noexcept {
    auto& p = *owner;
    p.item_count = 0;
    p.item_selected = 0;
    // "None" leads ("Use Default" on a server tab with an account value).
    auto& none = p.items[p.item_count++];
    reset_value(&none);
    const auto type = kind == Dialog::decoration ? ProfileCollectibleType::avatar_decoration
                                                 : ProfileCollectibleType::profile_effect;
    const u64 current = current_sku(kind == Dialog::decoration);
    for (u32 i = 0; i < p.owned_count && p.item_count < kUiProfileDialogItemCapacity; ++i) {
        const auto& owned = p.owned[i];
        if (owned.type != type)
            continue;
        auto& item = p.items[p.item_count];
        reset_value(&item);
        item.sku = owned.sku;
        item.label = owned.label.view();
        item.owned = true;
        if (type == ProfileCollectibleType::avatar_decoration) {
            char url[256];
            const u32 bytes = decoration_url(owned.asset.view(), {url, sizeof(url)});
            if (!bytes)
                continue;
            item_media(&item, {url, bytes}, true);
        } else if (owned.asset.bytes)
            item_media(&item, owned.asset.view(), false);
        if (owned.sku == current)
            p.item_selected = p.item_count;
        ++p.item_count;
    }
}

} // namespace profile_editor_detail
namespace {

const ProfileCollectible* owned_item(u64 sku, ProfileCollectibleType type) noexcept {
    for (u32 i = 0; i < owner->owned_count; ++i)
        if (owner->owned[i].sku == sku && owner->owned[i].type == type)
            return &owner->owned[i];
    return nullptr;
}

void push(const UiSettingsControl& control,
          UiSettingsControlPresentation presentation = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault}) noexcept {
    if (owner->control_count >= kControlCapacity)
        return;
    owner->presentation[owner->control_count] = presentation;
    owner->controls[owner->control_count++] = control;
}
void push_error(u64 id, ProfileEditErrorField field) noexcept {
    const auto& text = owner->errors.fields[static_cast<u32>(field)];
    if (text.bytes)
        push({id, text.view(), {}, UiSettingsControlKind::error, true, false, {}, 0, {}, nullptr});
}
Utf8View footer_status() noexcept {
    auto& p = *owner;
    if (saving())
        return p.tab == ProfileEditorTab::main ? utf8_literal("Saving your profile\xE2\x80\xA6")
                                               : utf8_literal("Saving your per-server profile\xE2\x80\xA6");
    if (p.failed)
        return p.status.bytes ? p.status
               : p.tab == ProfileEditorTab::main
                   ? utf8_literal("There was a problem updating your profile")
                   : utf8_literal("There was a problem updating your per-server profile...");
    return utf8_literal("Careful \xE2\x80\x94 you have unsaved changes!");
}

} // namespace
namespace profile_editor_detail {

bool palette_resolve(void*, u64 control, u32 choice, BackgroundTheme* out) noexcept {
    if (!owner)
        return false;
    u32 colors[2]{};
    u32 rgb = 0;
    if (control == control_theme) {
        if (effective_theme(colors) == 2)
            rgb = colors[choice ? 1 : 0];
        else
            rgb = 0x41434A;
    } else if (control == control_accent)
        rgb = owner->draft.accent_set ? owner->draft.accent : owner->base.has_accent ? owner->base.accent : 0x41434A;
    else
        return false;
    reset_value(out);
    out->colors[0] = out->colors[1] = rgb;
    out->color_count = 2;
    return true;
}

} // namespace profile_editor_detail
namespace {

void project_controls(Bank& bank) noexcept {
    auto& p = *owner;
    p.control_count = 0;
    const auto g = gate();
    const bool edit = editable() && connected();
    const bool guild = p.tab == ProfileEditorTab::guild;
    const bool known = p.loaded && !p.load_failed;
    p.tab_choices[0] = utf8_literal("Main Profile");
    p.tab_choices[1] = utf8_literal("Per-server Profiles");
    push({control_tabs, utf8_literal("Profiles"), {}, UiSettingsControlKind::tabs, !saving(), false, {p.tab_choices, 2},
          static_cast<u32>(p.tab), {}, nullptr});
    if (guild) {
        u32 selected = kUiSettingsNoChoice;
        for (u32 i = 0; i < p.guild_choice_count; ++i)
            if (p.guild_ids[i] == p.guild_id)
                selected = i;
        if (!p.guild_choice_count) {
            push({control_guild, utf8_literal("You don't belong to a server yet"),
                  utf8_literal("Join servers to hang out with friends and explore new communities."),
                  UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr});
        } else
            push({control_guild, utf8_literal("Choose a Server"),
                  utf8_literal("Show who you are with different profiles for each of your servers."),
                  UiSettingsControlKind::choice, !saving() && !changes(), false, {p.guild_choices, p.guild_choice_count},
                  selected, {}, nullptr});
    }
    if (!known || (guild && !p.guild_id)) {
        push({control_status,
              p.load_failed ? (guild ? utf8_literal("Unable to load server profile")
                                     : utf8_literal("Unable to load your profile"))
                            : utf8_literal("Loading\xE2\x80\xA6"),
              {}, UiSettingsControlKind::notice, true, false, {}, 0, p.load_failed ? utf8_literal("Try again") : Utf8View{},
              nullptr},
             {p.load_failed ? UiSettingsVariant::critical : UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
        return;
    }
    // Display Name / Server Nickname.
    const bool name_allowed = (g.enabled & profile_editor_name) != 0;
    const auto& s = p.saved;
    const Utf8View account_name = s.global_name.bytes ? s.global_name.view() : s.username.view();
    push({control_name, guild ? utf8_literal("Server Nickname") : utf8_literal("Display Name"),
          profile_editor_gate_reason(p.tab, g, profile_editor_name), UiSettingsControlKind::edit, edit && name_allowed,
          false, {}, 0, {},
          Editors::view(editor_name, control_name, guild ? account_name : s.username.view(), edit && name_allowed)});
    push_error(control_name_error, guild ? ProfileEditErrorField::nick : ProfileEditErrorField::global_name);
    // Pronouns: open to everyone.
    const Utf8View account_pronouns = s.pronouns.view();
    push({control_pronouns, utf8_literal("Pronouns"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
          Editors::view(editor_pronouns, control_pronouns,
                        guild && account_pronouns.bytes ? account_pronouns : utf8_literal("Add your pronouns"), edit)});
    push_error(control_pronouns_error, ProfileEditErrorField::pronouns);
    // Per-server Nitro group explanation (no upsell).
    if (guild && !nitro())
        push({control_nitro_note, {},
              utf8_literal("Customize your profile for every server, from avatar decorations to bios. Only with Nitro."),
              UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr});
    // Avatar.
    const bool avatar_allowed = (g.enabled & profile_editor_avatar) != 0;
    const bool has_avatar = p.draft.avatar == ProfileImageEdit::replace ||
                            (p.draft.avatar == ProfileImageEdit::keep && p.base.avatar);
    p.avatar_buttons[0] = utf8_literal("Change Avatar");
    p.avatar_buttons[1] = guild ? utf8_literal("Reset Avatar") : utf8_literal("Remove Avatar");
    push({control_avatar, utf8_literal("Avatar"), profile_editor_gate_reason(p.tab, g, profile_editor_avatar),
          UiSettingsControlKind::buttons, edit && (avatar_allowed || has_avatar) && !p.picker.runtime, false,
          {p.avatar_buttons, has_avatar ? 2u : 1u}, kUiSettingsNoChoice, {}, nullptr});
    push_error(control_avatar_error, ProfileEditErrorField::avatar);
    // Avatar Decoration and Profile Effect: owned collectibles.
    const bool decoration_allowed = (g.enabled & profile_editor_decoration) != 0;
    const u64 decoration = current_sku(true);
    p.decoration_buttons[0] = utf8_literal("Change Decoration");
    p.decoration_buttons[1] = guild && p.saved.decoration_sku ? utf8_literal("Use Default") : utf8_literal("Remove Decoration");
    push({control_decoration, utf8_literal("Avatar Decoration"),
          profile_editor_gate_reason(p.tab, g, profile_editor_decoration), UiSettingsControlKind::buttons,
          edit && decoration_allowed, false, {p.decoration_buttons, decoration ? 2u : 1u}, kUiSettingsNoChoice, {},
          nullptr});
    const bool effect_allowed = (g.enabled & profile_editor_effect) != 0;
    const u64 effect = current_sku(false);
    p.effect_buttons[0] = utf8_literal("Change Effect");
    p.effect_buttons[1] = guild ? utf8_literal("Use Default") : utf8_literal("Remove Effect");
    push({control_effect, utf8_literal("Profile Effect"), profile_editor_gate_reason(p.tab, g, profile_editor_effect),
          UiSettingsControlKind::buttons, edit && effect_allowed, false, {p.effect_buttons, effect ? 2u : 1u},
          kUiSettingsNoChoice, {}, nullptr});
    // Main profile without Nitro: the free Banner Color.
    if (!guild && !nitro()) {
        p.accent_choices[0] = utf8_literal("Banner Color");
        push({control_accent, utf8_literal("Banner Color"), {}, UiSettingsControlKind::colors, edit, false,
              {p.accent_choices, 1}, kUiSettingsNoChoice, {}, nullptr});
    }
    // Profile Banner (Nitro).
    const bool banner_allowed = (g.enabled & profile_editor_banner) != 0;
    const bool has_banner = p.draft.banner == ProfileImageEdit::replace ||
                            (p.draft.banner == ProfileImageEdit::keep && p.base.banner);
    p.banner_buttons[0] = utf8_literal("Change Banner");
    p.banner_buttons[1] = guild ? utf8_literal("Reset Banner") : utf8_literal("Remove Banner");
    const Utf8View banner_reason = profile_editor_gate_reason(p.tab, g, profile_editor_banner);
    push({control_banner, utf8_literal("Profile Banner"),
          banner_reason.bytes ? banner_reason
                              : utf8_literal("Upload a PNG, JPG, or GIF under 10MB. Images should be at least 680x240."),
          UiSettingsControlKind::buttons, edit && (banner_allowed || (guild && has_banner)) && !p.picker.runtime, false,
          {p.banner_buttons, has_banner ? 2u : 1u}, kUiSettingsNoChoice, {}, nullptr});
    push_error(control_banner_error, ProfileEditErrorField::banner);
    // Profile Theme (Nitro): Primary and Accent; servers can reset theirs.
    const bool theme_allowed = (g.enabled & profile_editor_theme) != 0;
    p.theme_choices[0] = utf8_literal("Primary");
    p.theme_choices[1] = utf8_literal("Accent");
    u32 colors[2]{};
    const bool themed = effective_theme(colors) == 2;
    push({control_theme, utf8_literal("Profile Theme"), profile_editor_gate_reason(p.tab, g, profile_editor_theme),
          UiSettingsControlKind::colors, edit && theme_allowed, false, {p.theme_choices, 2}, kUiSettingsNoChoice,
          guild && themed ? utf8_literal("Reset Theme") : Utf8View{}, nullptr});
    // Bio: markdown, the remaining-characters counter inside the field.
    const bool bio_allowed = (g.enabled & profile_editor_bio) != 0;
    const auto bio = editor_text(editor_bio);
    const i32 remaining = profile_editor_bio_remaining(bio);
    const Utf8View counter = bank.number(remaining);
    const Utf8View bio_reason = profile_editor_gate_reason(p.tab, g, profile_editor_bio);
    push({control_bio, utf8_literal("Bio"),
          bio_reason.bytes ? bio_reason : utf8_literal("You can use markdown and links if you\xE2\x80\x99" "d like."),
          UiSettingsControlKind::multiline, edit && bio_allowed, false, {}, 0, counter,
          Editors::view(editor_bio, control_bio,
                        guild ? utf8_literal("Tell this server a bit about yourself") : Utf8View{}, edit && bio_allowed)},
         {remaining < 0 ? UiSettingsVariant::critical : UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
    push_error(control_bio_error, ProfileEditErrorField::bio);
    push_error(control_misc_error, ProfileEditErrorField::misc);
    if (p.status.bytes && !p.failed)
        push({control_status_note, p.status, {}, UiSettingsControlKind::error, true, false, {}, 0, {}, nullptr});
}

} // namespace
namespace profile_editor_detail {

bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    p.bank_index ^= 1;
    Bank bank{p.banks[p.bank_index], sizeof(p.banks[0]), 0};
    project_controls(bank);
    const u16 changed = changes();
    const bool valid = p.loaded && profile_editor_changes_allowed(p.tab, p.base, current_draft(), gate());
    p.footer = {footer_status(),
                utf8_literal("Save Changes"),
                utf8_literal("Reset"),
                changed != 0 || saving() || p.failed,
                changed && valid && connected() && !busy() && p.loaded && g_app.account.api.profile_edit,
                saving(),
                p.failed};
    p.content.controls = {p.controls, p.control_count};
    p.content.presentation = p.presentation;
    p.content.footer = &p.footer;
    p.content.sectioned = true;
    p.content.aside_row = 1;
    p.content.aside_label = preview_heading(bank);
    p.content.aside_width = kPreviewWidth;
    // The preview card publishes its height; lay it out where the aside sits.
    UiRect heading{}, card{}, clip{};
    const bool had_aside = ui_settings_scene_aside(p.scene, &heading, &card, &clip);
    const UiRect anchor = had_aside ? card : UiRect{0, 0, kPreviewWidth, 400};
    const bool preview = layout_preview(anchor, had_aside ? clip : UiRect{});
    p.content.aside_height = preview ? maximum(1.f, p.preview.preview_height) : 0;
    ++p.content.revision;
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    if (preview && ui_settings_scene_aside(p.scene, &heading, &card, &clip) &&
        (!ui_rect_equal(card, anchor) || !ui_rect_equal(clip, p.preview.viewport)))
        layout_preview(card, clip);
    g_app.renderer.settings_profile_preview =
        preview && ui_settings_scene_aside(p.scene, nullptr, nullptr, nullptr) ? &p.preview : nullptr;
    g_app.renderer.settings_profile_dialog = p.dialog.active ? &p.dialog : nullptr;
    return true;
}
bool repaint() noexcept {
    return settings_modal_present(project());
}

void queue(Op op) noexcept {
    owner->op = op;
    owner->queued = true;
    owner->retry_at = 0;
}
void queue_fetch() noexcept {
    owner->loaded = false;
    owner->load_failed = false;
    queue(Op::fetch);
}

bool dialog_update() noexcept {
    auto& p = *owner;
    if (p.dialog_kind == Dialog::none) {
        ui_profile_dialog_close(&p.dialog);
        g_app.renderer.settings_profile_dialog = nullptr;
        return true;
    }
    UiProfileDialogContent content{};
    const auto viewport = application_viewport_dips();
    if (p.dialog_kind == Dialog::crop) {
        const auto editor = ui_profile_dialog_crop_editor(viewport);
        image_crop_resize(&p.crop, ui_rect_width(editor), ui_rect_height(editor));
        content.kind = UiProfileDialogKind::crop;
        content.title = utf8_literal("Edit Image");
        content.apply_label = utf8_literal("Apply");
        content.image = p.staged.request.key;
        content.crop = p.crop;
        content.image_ready = p.staged.ready;
        content.failed = p.staged.failed;
        content.can_apply = !p.staged.failed && p.staged.data;
        content.can_reset = image_crop_edited(p.crop);
    } else {
        const bool decoration = p.dialog_kind == Dialog::decoration;
        content.kind = decoration ? UiProfileDialogKind::decoration : UiProfileDialogKind::effect;
        content.title = decoration ? utf8_literal("Change Avatar Decoration") : utf8_literal("Change Profile Effect");
        content.apply_label = utf8_literal("Apply");
        content.items = {p.items, p.item_count};
        content.selected = p.item_selected;
        const u64 current = current_sku(decoration);
        const u64 chosen = p.item_selected < p.item_count ? p.items[p.item_selected].sku : 0;
        content.can_apply = chosen != current;
        content.preview_label = p.item_selected < p.item_count && p.items[p.item_selected].sku
                                    ? p.items[p.item_selected].label
                                    : Utf8View{};
        content.status = p.collectibles_failed ? utf8_literal("Couldn't load your collectibles.")
                         : !p.collectibles_loaded ? utf8_literal("Loading\xE2\x80\xA6")
                         : p.item_count <= 1
                             ? (decoration ? utf8_literal("You don't own any avatar decorations yet.")
                                           : utf8_literal("You don't own any profile effects yet."))
                             : Utf8View{};
        content.failed = p.collectibles_failed;
        // The avatar the decoration previews on.
        const auto* self = user_store_find(g_app.account_ui->user_store, p.self_user_id);
        if (self) {
            const Utf8View hash = user_store_avatar_hash(*self);
            const u32 variant = user_avatar_variant(hash);
            if (variant)
                content.avatar.key = media_asset_user_avatar(p.self_user_id, variant, asset_hash_animated(hash));
        }
    }
    if (!ui_profile_dialog_update(&p.dialog, content, viewport))
        return false;
    g_app.renderer.settings_profile_dialog = &p.dialog;
    g_app.context_focus = &p.dialog.widgets;
    return true;
}
// Rebuilds the dialog from its state and repaints the page.
bool refresh_dialog() noexcept {
    dialog_update();
    return repaint();
}
void close_dialog() noexcept {
    auto& p = *owner;
    if (p.dialog_kind == Dialog::crop)
        release_picked(p.staged);
    p.dialog_kind = Dialog::none;
    p.crop_dragging = p.zoom_dragging = false;
    application_release_capture();
    ui_profile_dialog_close(&p.dialog);
    g_app.renderer.settings_profile_dialog = nullptr;
    g_app.context_focus = &p.scene.widgets;
}
void open_picker_dialog(Dialog kind) noexcept {
    auto& p = *owner;
    p.dialog_kind = kind;
    build_items(kind);
    if (!p.collectibles_loaded && !busy())
        queue(Op::collectibles);
    dialog_update();
}
void apply_dialog() noexcept {
    auto& p = *owner;
    if (p.dialog_kind == Dialog::crop) {
        // A GIF avatar needs Nitro; the pick is not applied.
        const bool gif = p.staged.info.kind == GuildImageKind::gif && p.staged.info.animated;
        if (p.staged_use == Pick::avatar && gif && !nitro()) {
            p.status = utf8_literal("Upload GIFs to animate your avatars! Only with Nitro.");
            close_dialog();
            return;
        }
        p.staged.crop = p.crop;
        Picked& target = p.staged_use == Pick::avatar ? p.avatar : p.banner;
        move_picked(target, p.staged);
        if (p.staged_use == Pick::avatar)
            p.draft.avatar = ProfileImageEdit::replace;
        else
            p.draft.banner = ProfileImageEdit::replace;
        reset_value(&p.status);
        close_dialog();
        return;
    }
    if (p.item_selected >= p.item_count)
        return;
    const auto& item = p.items[p.item_selected];
    if (p.dialog_kind == Dialog::decoration) {
        p.draft.decoration_set = true;
        p.draft.decoration_sku = item.sku;
        const auto* owned = owned_item(item.sku, ProfileCollectibleType::avatar_decoration);
        assign(&p.decoration_asset, owned ? owned->asset.view() : Utf8View{});
    } else {
        p.draft.effect_set = true;
        p.draft.effect_sku = item.sku;
    }
    close_dialog();
}

Utf8View image_error(ProfileImageError error) noexcept {
    switch (error) {
    case ProfileImageError::format: return utf8_literal("Unable to process image");
    case ProfileImageError::too_large: return utf8_literal("Max file size is 10.00 MB please.");
    case ProfileImageError::decode: return utf8_literal("Unable to process image");
    default: return utf8_literal("Oops, something went wrong...");
    }
}
// The file name (without extension) describes the upload, like the official
// client's avatar_description.
void describe(Picked& picked) noexcept {
    u32 start = 0, end = picked.path_units;
    for (u32 i = 0; i < picked.path_units; ++i)
        if (picked.path[i] == L'\\' || picked.path[i] == L'/')
            start = i + 1;
    for (u32 i = end; i > start; --i)
        if (picked.path[i - 1] == L'.') {
            end = i - 1;
            break;
        }
    const int bytes = utf16_to_utf8(picked.path + start, static_cast<int>(end - start),
                                    picked.description, sizeof(picked.description), false);
    picked.description_bytes = bytes > 0 ? static_cast<u32>(bytes) : 0;
    if (!utf8_validate({picked.description, picked.description_bytes}).valid)
        picked.description_bytes = 0;
}

} // namespace profile_editor_detail
} // namespace maicord::app_detail

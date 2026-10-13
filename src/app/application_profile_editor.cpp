#include "application_profile_editor.hpp"
#include "application_account_logout.hpp"
#include "application_account_presentation.hpp"
#include "application_attachments.hpp"
#include "application_cursor.hpp"
#include "application_channel_overview.hpp"
#include "application_composer_text_input.hpp"
#include "application_media_picker.hpp"
#include "application_message_interaction.hpp"
#include "application_profile.hpp"
#include "application_settings.hpp"
#include "application_settings_layer.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "settings_catalog.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/member_moderation.hpp"
#include "maicord/model/profile_editor.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/model/bounded_edit.hpp"
#include "maicord/ui/profile_dialog_scene.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/profile_scene.hpp"

#include <stddef.h>

namespace maicord::app_detail {
namespace {
constexpr UiPageKey kProfilesPage{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::profiles) + 1};
constexpr u32 kPageCapacity = 64;
constexpr u32 kControlCapacity = 40;
constexpr u32 kNameBytes = kProfileEditDisplayNameCodepoints * 4;
constexpr u32 kPronounsBytes = kProfileEditPronounsCodepoints * 4;
constexpr u32 kBioBytes = kProfileBioMaximumUtf8Bytes;
constexpr u32 kGuildChoices = 100;
constexpr u32 kOwnedCapacity = 96;
constexpr float kPreviewWidth = 300;
// Local previews of picked images use their own key space ("PR" ids) with
// the thumbnail bit, so composer and forum previews never collide with them.
constexpr u64 kPreviewIdBase = 0x5052000000000000ull;
// The official client's bio box is 136 DIP tall; the rest is the settings default.
constexpr UiSettingsStyle kEditorStyle{680, 12, 24, 40, 136, kUiEditCornerRadius, kUiControlCornerRadius, 64};

enum : u64 {
    control_tabs = 1,
    control_guild,
    control_name,
    control_name_error,
    control_pronouns,
    control_pronouns_error,
    control_nitro_note,
    control_avatar,
    control_avatar_error,
    control_decoration,
    control_effect,
    control_accent,
    control_banner,
    control_banner_error,
    control_theme,
    control_bio,
    control_bio_error,
    control_misc_error,
    control_status,
    control_status_note,
};
enum Editor : u32 { editor_name, editor_pronouns, editor_bio, editor_count };
enum class Op : u8 { none, fetch, collectibles, product, save_account, save_profile };
enum class Pick : u8 { none, avatar, banner };
enum class Dialog : u8 { none, crop, decoration, effect };

// A picked image: file bytes, what the module reported, its crop and its
// local preview (thumbnail in the preview atlas), and the data URI made
// when a save starts.
struct Picked {
    u8* data;
    u32 bytes;
    ProfileImageInfo info;
    ImageCropState crop;
    AccountMediaLocalPreviewRequest request;
    void* instance;
    u64 poll_at;
    bool admitted, ready, failed;
    char description[kProfileEditDescriptionBytes];
    u32 description_bytes;
    u8* uri;
    u32 uri_bytes;
    u32 path_units;
    // Last: moves copy only the used part of the path.
    wchar_t path[kNativeFilePickerMaximumPathUnits + 1];
};

struct Owner {
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
    UiPage pages[kPageCapacity];
    UiSettingsPageContent content, other;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editors[editor_count];
    UiSettingsFooter footer;
    MessageComposer models[editor_count];
    BoundedEdit bindings[editor_count];
    char name[kNameBytes], pronouns[kPronounsBytes], bio[kBioBytes];
    Utf8View tab_choices[2];
    Utf8View guild_choices[kGuildChoices];
    u64 guild_ids[kGuildChoices];
    u32 guild_choice_count;
    Utf8View avatar_buttons[2], decoration_buttons[2], effect_buttons[2], banner_buttons[2];
    Utf8View theme_choices[2], accent_choices[1];
    ProfileEditorTab tab;
    u64 guild_id;
    u16 guild_generation;
    // Saved values and the full profile of each tab (the preview's base).
    ProfileEditSaved saved;
    UserProfile base_profile, preview_profile;
    ProfileEditorDraft draft;
    ProfileEditorBase base;
    ProfileCollectibleRef base_collectibles[kProfileEditCollectibleMaximum];
    bool loaded, load_failed;
    // Pending images and the one being cropped.
    Picked avatar, banner, staged;
    Pick staged_use;
    NativeFilePicker picker;
    Pick picking;
    bool picker_consumed;
    // Decorations/effects owned (from collectibles-purchases) and the asset
    // of the drafted decoration.
    ProfileCollectible owned[kOwnedCapacity];
    u32 owned_count;
    bool collectibles_loaded, collectibles_failed;
    ProfileString<64> decoration_asset;
    // The preview card and its profile effect.
    ProfileScene preview;
    ProfileEffect effect;
    u64 effect_loaded, effect_wanted;
    bool preview_open;
    // The modal dialog.
    UiProfileDialogScene dialog;
    Dialog dialog_kind;
    UiProfileDialogItem items[kUiProfileDialogItemCapacity];
    u32 item_count, item_selected;
    ImageCropState crop;
    bool crop_dragging, zoom_dragging;
    float drag_x, drag_y, drag_offset_x, drag_offset_y;
    Utf8View dialog_status;
    // Requests: GET loads, then the two sequential PATCHes of a save.
    Op op;
    bool queued, failed, saving, account_failed;
    u64 request_id, retry_at, lifetime, account_scope, profile_id, self_user_id;
    UserPatch user_patch;
    MemberEditPatch member_patch;
    ProfilePatch profile_patch;
    ProfileEditErrors errors;
    Utf8View status;
    u32 preview_serial;
    u32 structurals[32 * 1024];
    char banks[2][4096];
    u32 bank_index;
};
Owner* owner;
u64 lifetime_serial;

// Scope -------------------------------------------------------------------------

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.account.status.running;
}
bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready;
}
ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
u64 next_request_id() noexcept {
    u64 id = ++g_app.account_ui->message_history_request_id;
    if (!id)
        id = ++g_app.account_ui->message_history_request_id;
    return id;
}
bool scope_current() noexcept {
    if (!owner || !available() || owner->account_scope != g_app.plugin_account_scope_serial ||
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
ProfileEditorGate gate() noexcept {
    return profile_editor_gate(owner->tab, owner->loaded ? owner->saved.premium_type : state().self_premium_type,
                               can_change_nickname());
}

struct Bank {
    char* data;
    u32 capacity, used;
    Utf8View add(Utf8View text) noexcept {
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
u32 copy_text(char* output, u32 capacity, Utf8View text) noexcept {
    u32 bytes = minimum(text.bytes, capacity);
    while (bytes && bytes < text.bytes && (static_cast<u8>(text.data[bytes]) & 0xC0) == 0x80)
        --bytes;
    copy_bytes(output, text.data, bytes);
    return bytes;
}
template <u32 N> void assign(ProfileString<N>* to, Utf8View from) noexcept {
    to->bytes = static_cast<u16>(copy_text(to->data, N, from));
}

// Editors -------------------------------------------------------------------------

bool binding_ready(void* model, u64 identity) noexcept {
    MessageComposer* focused = nullptr;
    return owner && owner->lifetime == identity && editable() &&
           settings_layer_editor(&owner->layer, nullptr, &focused, nullptr, nullptr) && focused == model;
}
char* editor_buffer(u32 index) noexcept {
    return index == editor_name ? owner->name : index == editor_pronouns ? owner->pronouns : owner->bio;
}
u32 editor_capacity(u32 index) noexcept {
    return index == editor_name ? kNameBytes : index == editor_pronouns ? kPronounsBytes : kBioBytes;
}
// The official client allows typing past the bio limit (the counter goes negative and
// Save disables); the name and pronouns inputs stop at their maxLength.
u32 editor_limit(u32 index) noexcept {
    return index == editor_name       ? (owner->tab == ProfileEditorTab::main ? kProfileEditDisplayNameCodepoints
                                                                              : kProfileEditNicknameCodepoints)
           : index == editor_pronouns ? kProfileEditPronounsCodepoints
                                      : kProfileBioMaximumCodepoints;
}
void load_editor(u32 index, Utf8View text) noexcept {
    auto& model = owner->models[index];
    const u32 bytes = copy_text(editor_buffer(index), editor_capacity(index), text);
    message_composer_refresh_borrowed(&model, bytes);
    message_composer_set_selection(&model, bytes, bytes);
    bounded_edit_bind(&owner->bindings[index], &model, editor_limit(index), index == editor_bio, owner->lifetime,
                      binding_ready, &model);
}
Utf8View editor_text(u32 index) noexcept {
    return message_composer_text(owner->models[index]);
}
u32 editor_of(u64 control) noexcept {
    return control == control_name       ? editor_name
           : control == control_pronouns ? editor_pronouns
           : control == control_bio      ? editor_bio
                                         : editor_count;
}

// Saved values and the draft --------------------------------------------------------

void release_uri(Picked& picked) noexcept {
    if (picked.uri) {
        SecureZeroMemory(picked.uri, picked.uri_bytes);
        VirtualFree(picked.uri, 0, MEM_RELEASE);
    }
    picked.uri = nullptr;
    picked.uri_bytes = 0;
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
void release_picked(Picked& picked) noexcept {
    retire_preview(picked);
    release_uri(picked);
    if (picked.data) {
        SecureZeroMemory(picked.data, picked.bytes);
        VirtualFree(picked.data, 0, MEM_RELEASE);
    }
    // Paths are long; clear only the header fields and the used path prefix.
    clear_bytes(picked.path, picked.path_units * static_cast<u32>(sizeof(wchar_t)));
    picked.data = nullptr;
    picked.bytes = picked.path_units = picked.description_bytes = 0;
    picked.info = {};
    picked.crop = {};
    picked.request = {};
    picked.instance = nullptr;
    picked.poll_at = 0;
    picked.admitted = picked.ready = picked.failed = false;
}
void move_picked(Picked& to, Picked& from) noexcept {
    release_picked(to);
    copy_bytes(&to, &from, static_cast<u32>(offsetof(Picked, path)));
    copy_bytes(to.path, from.path, from.path_units * static_cast<u32>(sizeof(wchar_t)));
    // The source keeps no ownership.
    from.data = nullptr;
    from.uri = nullptr;
    from.admitted = false;
    from.request = {};
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
    b = {};
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
ProfileEditorDraft current_draft() noexcept {
    auto d = owner->draft;
    d.name = editor_text(editor_name);
    d.pronouns = editor_text(editor_pronouns);
    d.bio = editor_text(editor_bio);
    d.avatar_uri = {reinterpret_cast<const char*>(owner->avatar.uri), owner->avatar.uri_bytes};
    d.avatar_description = {owner->avatar.description, owner->avatar.description_bytes};
    d.banner_uri = {reinterpret_cast<const char*>(owner->banner.uri), owner->banner.uri_bytes};
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
    p.draft = {};
    p.draft.theme[0] = p.base.theme[0];
    p.draft.theme[1] = p.base.theme[1];
    p.draft.accent = p.base.accent;
    p.draft.decoration_sku = p.base.decoration_sku;
    p.draft.effect_sku = p.base.effect_sku;
    const bool guild = p.tab == ProfileEditorTab::guild;
    assign(&p.decoration_asset, guild && p.saved.member_decoration_asset.bytes ? p.saved.member_decoration_asset.view()
                                                                               : p.saved.decoration_asset.view());
    p.errors = {};
    p.failed = false;
    p.status = {};
}

// Preview card ------------------------------------------------------------------------

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
        v.guild_bio = {};
        v.guild_pronouns = {};
        v.nickname = {};
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
            v.guild_appearance.avatar = {};
        else
            v.avatar = {};
    }
    if (d.banner == ProfileImageEdit::remove) {
        if (guild)
            v.guild_banner = {};
        else
            v.banner = {};
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
    const u64 effect = d.effect_set ? d.effect_sku : p.base.effect_sku;
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
bool layout_preview(UiRect anchor) noexcept {
    auto& p = *owner;
    if (!p.loaded)
        return false;
    compose_preview();
    auto& s = p.preview;
    s.presentation = ProfilePresentation::preview;
    s.pending_avatar = {};
    s.pending_banner = {};
    if (p.draft.avatar == ProfileImageEdit::replace && p.avatar.ready)
        s.pending_avatar = {p.avatar.request.key, p.avatar.crop, true};
    if (p.draft.banner == ProfileImageEdit::replace && p.banner.ready)
        s.pending_banner = {p.banner.request.key, p.banner.crop, true};
    s.measure = &g_app.account_ui->text_measure_cache;
    s.effect = p.effect.sku && p.effect.sku == p.effect_wanted ? &p.effect : nullptr;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    s.reduced_motion = !animations || g_app.preferences.reduced_motion;
    s.phase = ProfilePhase::ready;
    s.presence = UiPresence::online;
    const auto viewport = application_viewport_dips();
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

// Local previews of picked images ------------------------------------------------------

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
bool request_preview(Picked& picked, FileSnapshot snapshot) noexcept {
    const u32 generation = ++owner->preview_serial | kLocalDraftPreviewThumbnailVariant;
    const auto key = media_asset_local_draft_preview(kPreviewIdBase | owner->preview_serial, generation,
                                                     format_of(picked.info.kind));
    picked.request = {};
    picked.request.key = key;
    picked.request.snapshot = snapshot;
    picked.request.path = {picked.path, picked.path_units};
    picked.request.owner_scope = 0x5052000000000000ull | (owner->profile_id & 0xffffffffffffull);
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

// Owned collectibles ---------------------------------------------------------------------

u64 hash_url(Utf8View url) noexcept {
    return hash64_bytes(kHash64Seed, url.data, url.bytes);
}
void item_media(UiProfileDialogItem* item, Utf8View url, bool png) noexcept {
    assign(&item->url, url);
    item->key = media_asset_embed(hash_url(url), 1, 0, MediaEmbedResource::image,
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
void build_items(Dialog kind) noexcept {
    auto& p = *owner;
    p.item_count = 0;
    p.item_selected = 0;
    // "None" leads ("Use Default" on a server tab with an account value).
    auto& none = p.items[p.item_count++];
    none = {};
    const auto type = kind == Dialog::decoration ? ProfileCollectibleType::avatar_decoration
                                                 : ProfileCollectibleType::profile_effect;
    const u64 current = kind == Dialog::decoration ? (p.draft.decoration_set ? p.draft.decoration_sku : p.base.decoration_sku)
                                                   : (p.draft.effect_set ? p.draft.effect_sku : p.base.effect_sku);
    for (u32 i = 0; i < p.owned_count && p.item_count < kUiProfileDialogItemCapacity; ++i) {
        const auto& owned = p.owned[i];
        if (owned.type != type)
            continue;
        auto& item = p.items[p.item_count];
        item = {};
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
const ProfileCollectible* owned_item(u64 sku, ProfileCollectibleType type) noexcept {
    for (u32 i = 0; i < owner->owned_count; ++i)
        if (owner->owned[i].sku == sku && owner->owned[i].type == type)
            return &owner->owned[i];
    return nullptr;
}

// Projection -----------------------------------------------------------------------------

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
bool palette_resolve(void*, u64 control, u32 choice, BackgroundTheme* out) noexcept {
    if (!owner)
        return false;
    u32 colors[2]{};
    u32 rgb = 0;
    if (control == control_theme) {
        if (effective_theme(colors) == 2)
            rgb = colors[choice ? 1 : 0];
        else
            rgb = choice ? 0x41434A : 0x41434A;
    } else if (control == control_accent)
        rgb = owner->draft.accent_set ? owner->draft.accent : owner->base.has_accent ? owner->base.accent : 0x41434A;
    else
        return false;
    *out = {};
    out->colors[0] = out->colors[1] = rgb;
    out->color_count = 2;
    return true;
}
bool project() noexcept;
bool project_controls(Bank& bank) noexcept {
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
        return true;
    }
    // Display Name / Server Nickname.
    const bool name_allowed = (g.enabled & profile_editor_name) != 0;
    const auto& s = p.saved;
    const Utf8View account_name = s.global_name.bytes ? s.global_name.view() : s.username.view();
    p.editors[editor_name] = settings_layer_editor_view(&p.layer, control_name, p.models[editor_name],
                                                        guild ? account_name : s.username.view(), edit && name_allowed);
    push({control_name, guild ? utf8_literal("Server Nickname") : utf8_literal("Display Name"),
          profile_editor_gate_reason(p.tab, g, profile_editor_name), UiSettingsControlKind::edit, edit && name_allowed,
          false, {}, 0, {}, &p.editors[editor_name]});
    push_error(control_name_error, guild ? ProfileEditErrorField::nick : ProfileEditErrorField::global_name);
    // Pronouns: open to everyone.
    const Utf8View account_pronouns = s.pronouns.view();
    p.editors[editor_pronouns] = settings_layer_editor_view(
        &p.layer, control_pronouns, p.models[editor_pronouns],
        guild && account_pronouns.bytes ? account_pronouns : utf8_literal("Add your pronouns"), edit);
    push({control_pronouns, utf8_literal("Pronouns"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
          &p.editors[editor_pronouns]});
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
    const u64 decoration = p.draft.decoration_set ? p.draft.decoration_sku : p.base.decoration_sku;
    p.decoration_buttons[0] = utf8_literal("Change Decoration");
    p.decoration_buttons[1] = guild && p.saved.decoration_sku ? utf8_literal("Use Default") : utf8_literal("Remove Decoration");
    push({control_decoration, utf8_literal("Avatar Decoration"),
          profile_editor_gate_reason(p.tab, g, profile_editor_decoration), UiSettingsControlKind::buttons,
          edit && decoration_allowed, false, {p.decoration_buttons, decoration ? 2u : 1u}, kUiSettingsNoChoice, {},
          nullptr});
    const bool effect_allowed = (g.enabled & profile_editor_effect) != 0;
    const u64 effect = p.draft.effect_set ? p.draft.effect_sku : p.base.effect_sku;
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
    p.editors[editor_bio] = settings_layer_editor_view(
        &p.layer, control_bio, p.models[editor_bio],
        guild ? utf8_literal("Tell this server a bit about yourself") : Utf8View{}, edit && bio_allowed);
    const Utf8View counter = bank.number(remaining);
    const Utf8View bio_reason = profile_editor_gate_reason(p.tab, g, profile_editor_bio);
    push({control_bio, utf8_literal("Bio"),
          bio_reason.bytes ? bio_reason : utf8_literal("You can use markdown and links if you\xE2\x80\x99" "d like."),
          UiSettingsControlKind::multiline, edit && bio_allowed, false, {}, 0, counter, &p.editors[editor_bio]},
         {remaining < 0 ? UiSettingsVariant::critical : UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
    push_error(control_bio_error, ProfileEditErrorField::bio);
    push_error(control_misc_error, ProfileEditErrorField::misc);
    if (p.status.bytes && !p.failed)
        push({control_status_note, p.status, {}, UiSettingsControlKind::error, true, false, {}, 0, {}, nullptr});
    return true;
}
bool repaint() noexcept;
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
    const bool preview = layout_preview(anchor);
    p.content.aside_height = preview ? maximum(1.f, p.preview.preview_height) : 0;
    ++p.content.revision;
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    if (preview && ui_settings_scene_aside(p.scene, &heading, &card, &clip) && !ui_rect_equal(card, anchor))
        layout_preview(card);
    g_app.renderer.settings_profile_preview =
        preview && ui_settings_scene_aside(p.scene, nullptr, nullptr, nullptr) ? &p.preview : nullptr;
    g_app.renderer.settings_profile_dialog = p.dialog.active ? &p.dialog : nullptr;
    return true;
}
bool repaint() noexcept {
    if (!project())
        return false;
    application_request_frame();
    return !g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

// Requests ---------------------------------------------------------------------------------

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

// Dialogs ---------------------------------------------------------------------------------

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
        const u64 current = decoration ? (p.draft.decoration_set ? p.draft.decoration_sku : p.base.decoration_sku)
                                       : (p.draft.effect_set ? p.draft.effect_sku : p.base.effect_sku);
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
void close_dialog() noexcept {
    auto& p = *owner;
    if (p.dialog_kind == Dialog::crop)
        release_picked(p.staged);
    p.dialog_kind = Dialog::none;
    p.crop_dragging = p.zoom_dragging = false;
    if (GetCapture() == g_app.window)
        ReleaseCapture();
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
        p.status = {};
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

// Picking images ---------------------------------------------------------------------------

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
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, picked.path + start, static_cast<int>(end - start),
                                          picked.description, sizeof(picked.description), nullptr, nullptr);
    picked.description_bytes = bytes > 0 ? static_cast<u32>(bytes) : 0;
    if (!utf8_validate({picked.description, picked.description_bytes}).valid)
        picked.description_bytes = 0;
}
bool read_image(Pick pick, ConstSlice<wchar_t> path, FileSnapshot snapshot, Utf8View* error) noexcept {
    auto& p = *owner;
    *error = utf8_literal("Oops, something went wrong...");
    if (!path.data || !path.count || path.count > kNativeFilePickerMaximumPathUnits || !g_app.account.api.profile_image)
        return false;
    auto& staged = p.staged;
    release_picked(staged);
    copy_bytes(staged.path, path.data, path.count * static_cast<u32>(sizeof(wchar_t)));
    staged.path[path.count] = 0;
    staged.path_units = path.count;
    const HANDLE file = CreateFileW(staged.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        release_picked(staged);
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > kProfileImageSourceMaximumBytes) {
        CloseHandle(file);
        if (size.QuadPart > kProfileImageSourceMaximumBytes)
            *error = utf8_literal("Your files are too powerful. Max file size is 10.00 MB please.");
        release_picked(staged);
        return false;
    }
    auto* data = static_cast<u8*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u32 total = 0;
    while (data && total < size.QuadPart) {
        DWORD read = 0;
        if (!ReadFile(file, data + total, static_cast<DWORD>(size.QuadPart - total), &read, nullptr) || !read)
            break;
        total += read;
    }
    if (!snapshot.bytes)
        file_snapshot_read(file, &snapshot);
    CloseHandle(file);
    if (!data || total != size.QuadPart) {
        if (data)
            VirtualFree(data, 0, MEM_RELEASE);
        release_picked(staged);
        return false;
    }
    staged.data = data;
    staged.bytes = total;
    ProfileImageRequest request{};
    request.source = {data, total};
    request.info = &staged.info;
    if (!g_app.account.api.profile_image(g_app.account.instance, &request)) {
        *error = image_error(staged.info.error);
        release_picked(staged);
        return false;
    }
    describe(staged);
    if (!request_preview(staged, snapshot))
        staged.failed = true;
    p.staged_use = pick;
    const auto editor = ui_profile_dialog_crop_editor(application_viewport_dips());
    p.crop = image_crop_begin(pick == Pick::avatar ? ImageCropUse::avatar : ImageCropUse::banner, staged.info.width,
                              staged.info.height, ui_rect_width(editor), ui_rect_height(editor));
    p.dialog_kind = Dialog::crop;
    return true;
}
void poll_picker() noexcept {
    auto& p = *owner;
    if (!p.picker.runtime)
        return;
    const auto status = native_file_picker_status(&p.picker);
    if (status.phase != NativeFilePickerPhase::complete)
        return;
    if (!p.picker_consumed) {
        p.picker_consumed = true;
        Utf8View error{};
        if (status.ok() && status.count == 1 && p.scene.active && editable()) {
            FileSnapshot snapshot{};
            native_file_picker_snapshot(&p.picker, 0, &snapshot);
            if (read_image(p.picking, native_file_picker_path(&p.picker, 0), snapshot, &error)) {
                p.status = {};
                pump_previews(clock_monotonic_milliseconds());
                dialog_update();
            } else
                p.status = error;
        } else if (!status.ok() && status.error != NativeFilePickerError::cancelled && p.scene.active)
            p.status = utf8_literal("Couldn't open that file.");
    }
    native_file_picker_release(&p.picker, 0);
    p.picking = Pick::none;
}
void start_pick(Pick pick) noexcept {
    auto& p = *owner;
    if (p.picker.runtime || !connected() || !editable())
        return;
    p.picker_consumed = false;
    p.picking = pick;
    if (native_file_picker_start(&p.picker, g_app.window, 1,
                                 pick == Pick::avatar ? utf8_literal("Select an Image") : utf8_literal("Select an Image")) !=
        NativeFilePickerError::none) {
        p.picking = Pick::none;
        p.status = utf8_literal("Couldn't open the file picker.");
    }
}
void remove_image(Pick pick) noexcept {
    auto& p = *owner;
    Picked& picked = pick == Pick::avatar ? p.avatar : p.banner;
    ProfileImageEdit& edit = pick == Pick::avatar ? p.draft.avatar : p.draft.banner;
    const bool set = pick == Pick::avatar ? p.base.avatar : p.base.banner;
    release_picked(picked);
    edit = set ? ProfileImageEdit::remove : ProfileImageEdit::keep;
}
// Makes the data URI of a replaced image (once per save).
bool prepare_uri(Picked& picked) noexcept {
    if (picked.uri)
        return true;
    if (!picked.data || !g_app.account.api.profile_image)
        return false;
    const u32 capacity = account_profile_image_uri_capacity(picked.info);
    auto* uri = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!uri)
        return false;
    ProfileImageRequest request{};
    request.source = {picked.data, picked.bytes};
    request.crop = image_crop_output(picked.crop);
    ProfileImageInfo info{};
    request.info = &info;
    request.data_uri = {uri, capacity};
    if (!g_app.account.api.profile_image(g_app.account.instance, &request) || !request.data_uri_bytes) {
        VirtualFree(uri, 0, MEM_RELEASE);
        return false;
    }
    picked.uri = uri;
    picked.uri_bytes = request.data_uri_bytes;
    return true;
}

// Colours (Primary / Accent / Banner Color): a hex value, like the picker's input.
enum ColorSlot : u64 { color_primary = 1, color_accent, color_banner };
bool color_valid(TextFormOwnerKey key) noexcept {
    return owner && owner->lifetime == key.scope && owner->scene.active && editable();
}
bool color_save(TextFormOwnerKey key, Utf8View text) noexcept {
    u32 color = 0;
    if (!color_valid(key) || !profile_editor_parse_color(text, &color))
        return false;
    auto& p = *owner;
    if (key.collection == color_banner) {
        p.draft.accent_set = true;
        p.draft.accent = color;
    } else {
        u32 colors[2]{};
        if (effective_theme(colors) != 2)
            colors[0] = colors[1] = 0x41434A;
        colors[key.collection == color_primary ? 0 : 1] = color;
        p.draft.theme_set = true;
        p.draft.theme_reset = false;
        p.draft.theme[0] = colors[0];
        p.draft.theme[1] = colors[1];
    }
    application_text_form_close(false, false);
    repaint();
    return true;
}
void edit_color(ColorSlot slot) noexcept {
    auto& p = *owner;
    u32 colors[2]{};
    const u32 current = slot == color_banner ? (p.draft.accent_set ? p.draft.accent : p.base.accent)
                        : effective_theme(colors) == 2 ? colors[slot == color_primary ? 0 : 1]
                                                       : 0x41434A;
    static char initial[8];
    const u32 bytes = profile_editor_format_color(current, {initial, sizeof(initial)});
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Pick a Color");
    request.label = slot == color_primary ? utf8_literal("Primary") : slot == color_accent ? utf8_literal("Accent")
                                                                                          : utf8_literal("Banner Color");
    request.context = utf8_literal("Enter a hex colour such as #5865F2.");
    request.accept = utf8_literal("Apply");
    request.initial = {initial, bytes};
    request.owner = {p.lifetime, slot, 0};
    request.valid = color_valid;
    request.save = color_save;
    request.maximum_codepoints = 7;
    request.single_line = true;
    application_text_form_open(request);
}

// Navigation away from the page --------------------------------------------------------------

bool guard() noexcept {
    // Unsaved changes hold the page, as the official client's settings do.
    if (owner && changes() && !saving()) {
        settings_layer_shake(&owner->layer, clock_monotonic_milliseconds());
        repaint();
        return false;
    }
    return true;
}
void hand_off(UiSettingsTarget target) noexcept {
    if (!guard())
        return;
    // Focus leaves the closing editor scene, so Settings restores the shell.
    application_profile_editor_close(true);
    if (target.kind == UiSettingsTargetKind::action) {
        if (target.control_id == kSettingsLogOutAction)
            application_account_logout_open();
        return;
    }
    if (!application_settings_resume() || !g_app.settings)
        return;
    if (target.kind == UiSettingsTargetKind::page)
        application_settings_open_page(target.page);
    else if (target.kind == UiSettingsTargetKind::search)
        application_settings_focus_search();
}
void select_tab(ProfileEditorTab tab) noexcept {
    auto& p = *owner;
    if (p.tab == tab || !guard())
        return;
    p.tab = tab;
    if (tab == ProfileEditorTab::guild && !p.guild_id && p.guild_choice_count) {
        p.guild_id = p.guild_ids[0];
        if (const auto* guild = chat_state_store_find_guild(state(), p.guild_id))
            p.guild_generation = guild->generation;
    }
    p.errors = {};
    p.status = {};
    p.failed = false;
    queue_fetch();
}
void select_guild(u32 index) noexcept {
    auto& p = *owner;
    if (index >= p.guild_choice_count || p.guild_ids[index] == p.guild_id || !guard())
        return;
    p.guild_id = p.guild_ids[index];
    const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
    p.guild_generation = guild ? guild->generation : 0;
    p.errors = {};
    p.status = {};
    p.failed = false;
    queue_fetch();
}
void collect_guilds() noexcept {
    auto& p = *owner;
    p.guild_choice_count = 0;
    const auto& view = g_app.account_ui->navigation_view;
    for (u32 i = 0; i < view.guild_count && p.guild_choice_count < kGuildChoices; ++i) {
        const auto& row = view.guild_rows[i];
        if (!guild_view_row_is_guild(row) || row.unavailable)
            continue;
        const auto* guild = chat_state_store_find_guild(state(), row.id);
        if (!guild)
            continue;
        p.guild_ids[p.guild_choice_count] = row.id;
        p.guild_choices[p.guild_choice_count++] = chat_state_store_guild_name(state(), *guild);
    }
}

// Settings layer hooks -------------------------------------------------------------------------

void save() noexcept {
    auto& p = *owner;
    if (!p.footer.can_save)
        return;
    // Data URIs are made once, here, from the cropped picks.
    if ((p.draft.avatar == ProfileImageEdit::replace && !prepare_uri(p.avatar)) ||
        (p.draft.banner == ProfileImageEdit::replace && !prepare_uri(p.banner))) {
        p.failed = true;
        p.status = utf8_literal("Unable to process image");
        return;
    }
    const auto draft = current_draft();
    if (!profile_editor_patches(p.tab, p.base, draft, gate(), &p.user_patch, &p.member_patch, &p.profile_patch)) {
        p.failed = true;
        p.status = {};
        return;
    }
    p.errors = {};
    p.failed = false;
    p.status = {};
    p.account_failed = false;
    p.saving = true;
    const bool account = p.tab == ProfileEditorTab::main ? p.user_patch.fields != 0 : p.member_patch.fields != 0;
    queue(account ? Op::save_account : Op::save_profile);
}
void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!owner)
        return;
    auto& p = *owner;
    const auto target = activation.value.target;
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        if (guard())
            application_profile_editor_close();
        return;
    case UiSettingsTargetKind::reset:
        if (!saving())
            discard();
        repaint();
        return;
    case UiSettingsTargetKind::save:
        save();
        repaint();
        return;
    case UiSettingsTargetKind::page:
        if (!ui_page_key_equal(target.page, kProfilesPage))
            hand_off(target);
        return;
    case UiSettingsTargetKind::search:
    case UiSettingsTargetKind::action:
        hand_off(target);
        return;
    case UiSettingsTargetKind::control:
        break;
    default:
        return;
    }
    const u64 id = target.control_id;
    const u32 choice = activation.value.selected_choice;
    if (editor_of(id) < editor_count) {
        settings_layer_focus(&p.layer, target);
        return;
    }
    if (id == control_tabs) {
        select_tab(choice ? ProfileEditorTab::guild : ProfileEditorTab::main);
        repaint();
        return;
    }
    if (id == control_status) {
        queue_fetch();
        repaint();
        return;
    }
    if (id == control_guild) {
        if (p.guild_choice_count && !changes()) {
            PopupItem items[kGuildChoices]{};
            for (u32 i = 0; i < p.guild_choice_count; ++i)
                items[i] = {p.guild_choices[i], i + 1};
            settings_layer_open_choice(&p.layer, kProfilesPage.owner_id, {items, p.guild_choice_count},
                                       activation.anchor, maximum(240.f, ui_rect_width(activation.anchor)));
        }
        return;
    }
    if (!editable())
        return;
    switch (id) {
    case control_avatar:
        if (choice == 0)
            start_pick(Pick::avatar);
        else
            remove_image(Pick::avatar);
        break;
    case control_banner:
        if (choice == 0)
            start_pick(Pick::banner);
        else
            remove_image(Pick::banner);
        break;
    case control_decoration:
        if (choice == 0)
            open_picker_dialog(Dialog::decoration);
        else {
            p.draft.decoration_set = true;
            p.draft.decoration_sku = 0;
            p.decoration_asset = {};
        }
        break;
    case control_effect:
        if (choice == 0)
            open_picker_dialog(Dialog::effect);
        else {
            p.draft.effect_set = true;
            p.draft.effect_sku = 0;
        }
        break;
    case control_theme:
        if (choice < 2)
            edit_color(choice ? color_accent : color_primary);
        else {
            p.draft.theme_set = true;
            p.draft.theme_reset = true;
        }
        break;
    case control_accent:
        edit_color(color_banner);
        break;
    default:
        return;
    }
    repaint();
}
void choose(SettingsLayer*, u32 action) noexcept {
    if (owner && action)
        select_guild(action - 1);
    repaint();
}
bool edited(SettingsLayer*, u64 control) noexcept {
    if (!owner || editor_of(control) == editor_count)
        return false;
    owner->failed = false;
    owner->status = {};
    return true;
}
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** binding, bool* multiline) noexcept {
    const u32 index = owner ? editor_of(control) : editor_count;
    if (index == editor_count)
        return false;
    *model = &owner->models[index];
    *binding = &owner->bindings[index];
    *multiline = index == editor_bio;
    return true;
}
void close_hook(SettingsLayer*) noexcept {
    if (guard())
        application_profile_editor_close();
}
bool repaint_hook(SettingsLayer*) noexcept {
    return repaint();
}
bool editable_hook(SettingsLayer*) noexcept {
    return editable();
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose, close_hook, repaint_hook, nullptr,
                                    editable_hook};

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = static_cast<Owner*>(VirtualAlloc(nullptr, sizeof(Owner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return false;
    owner->layer.scene = &owner->scene;
    owner->layer.hooks = &kHooks;
    owner->layer.owner = owner;
    for (u32 i = 0; i < editor_count; ++i)
        message_composer_initialize(&owner->models[i], {editor_buffer(i), editor_capacity(i)});
    return true;
}
// The user settings pages (their sidebar), with this page as Profiles.
bool register_pages() noexcept {
    auto& p = *owner;
    if (!ui_page_registry_initialize(&p.registry, {p.pages, kPageCapacity}))
        return false;
    p.content = {utf8_literal("Profiles"), {}, {}, 1, kUiSettingsPageFormat, &kEditorStyle, nullptr};
    p.other = {utf8_literal("Settings"), {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
    const auto* settings = application_settings_pages();
    bool profiles = false;
    for (u32 i = 0; settings && i < settings->count && p.registry.count < kPageCapacity; ++i) {
        const auto& entry = settings->storage[i];
        const bool mine = ui_page_key_equal(entry.key, kProfilesPage);
        profiles = profiles || mine;
        if (!ui_page_registry_register(&p.registry, {entry.key, entry.label, mine ? &p.content : &p.other}).ok())
            return false;
    }
    if (!profiles) {
        // Settings not opened yet: the native pages from the catalog.
        for (u32 i = 0; i < kSettingsPanelCount; ++i) {
            const auto& panel = settings_catalog_panel(static_cast<SettingsPanel>(i));
            const UiPageKey key{kSettingsNativeOwner, i + 1ull};
            if (!ui_page_registry_register(&p.registry,
                                           {key, panel.title, ui_page_key_equal(key, kProfilesPage) ? &p.content : &p.other})
                     .ok())
                return false;
        }
    }
    return ui_page_registry_select(&p.registry, kProfilesPage).ok();
}

// Responses ------------------------------------------------------------------------------------------

Utf8View failure(const RestClientResponse& response, Utf8View body) noexcept {
    auto& p = *owner;
    if (!response.transport_ok())
        return utf8_literal("Couldn't reach Discord. Try again.");
    if (profile_edit_decode_errors(body, {p.structurals, sizeof(p.structurals) / sizeof(u32)}, &p.errors)) {
        if (p.errors.username)
            return utf8_literal("Your username is invalid or contains invalid characters. You must update your "
                                "username before you can change other user settings.");
        if (response.status_code == 429)
            return p.errors.message.bytes ? p.errors.message.view() : utf8_literal("You are being rate limited.");
    }
    return {};
}
void load_profile(Utf8View body) noexcept {
    auto& p = *owner;
    const u64 guild = p.tab == ProfileEditorTab::guild ? p.guild_id : 0;
    const Slice<u32> structurals{p.structurals, sizeof(p.structurals) / sizeof(u32)};
    if (!profile_edit_decode_profile(body, structurals, p.self_user_id, guild, &p.saved) ||
        !user_profile_decode(body, structurals, p.self_user_id, guild, &p.base_profile)) {
        p.load_failed = true;
        return;
    }
    p.loaded = true;
    p.load_failed = false;
    discard();
    // The saved effect's layers come from its product.
    const u64 effect = p.base.effect_sku;
    if (effect && p.effect.sku != effect)
        p.effect_wanted = effect;
}
} // namespace

// Public entry points ------------------------------------------------------------------------------------

bool application_profile_editor_open(ProfileEditorTab tab, u64 guild_id) noexcept {
    if (!available() || !g_app.renderer_ready || g_app.text_form || !g_app.account.api.profile_edit ||
        !g_app.account.api.profile_image)
        return false;
    if (application_profile_editor_active()) {
        if (owner->tab != tab || (guild_id && owner->guild_id != guild_id)) {
            owner->tab = tab;
            if (guild_id)
                owner->guild_id = guild_id;
            queue_fetch();
        }
        return repaint();
    }
    if (application_channel_overview_active())
        return false;
    const auto& s = state();
    if (!s.self_user_id || !ensure_owner() || busy())
        return false;
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
    auto& p = *owner;
    p.lifetime = ++lifetime_serial;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.self_user_id = s.self_user_id;
    p.tab = tab;
    p.queued = p.failed = p.saving = false;
    p.op = Op::none;
    p.status = {};
    p.errors = {};
    p.loaded = p.load_failed = false;
    p.dialog_kind = Dialog::none;
    p.collectibles_loaded = p.collectibles_failed = false;
    p.owned_count = 0;
    p.effect = {};
    p.effect_loaded = p.effect_wanted = 0;
    collect_guilds();
    p.guild_id = guild_id ? guild_id : s.selected_guild_id;
    bool listed = false;
    for (u32 i = 0; i < p.guild_choice_count; ++i)
        listed = listed || p.guild_ids[i] == p.guild_id;
    if (!listed)
        p.guild_id = p.guild_choice_count ? p.guild_ids[0] : 0;
    const auto* guild = p.guild_id ? chat_state_store_find_guild(s, p.guild_id) : nullptr;
    p.guild_generation = guild ? guild->generation : 0;
    release_picked(p.avatar);
    release_picked(p.banner);
    release_picked(p.staged);
    p.draft = {};
    settings_layer_reset(&p.layer);
    if (!register_pages())
        return false;
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::modal);
    ui_settings_scene_bind_navigation(&p.scene, settings_catalog_navigation(application_account_logout_available()), 1);
    ui_settings_scene_bind_palette(&p.scene, {owner, palette_resolve, p.lifetime});
    for (u32 i = 0; i < editor_count; ++i)
        load_editor(i, {});
    p.scene.focus = {UiSettingsTargetKind::page, kProfilesPage, 0};
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()) || !project()) {
        application_profile_editor_close(false);
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &p.scene);
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    g_app.context_focus = &p.scene.widgets;
    application_sync_attachment_drop_target();
    queue_fetch();
    if (repaint())
        return true;
    application_profile_editor_close(false);
    return false;
}

bool application_profile_editor_active() noexcept {
    return owner && owner->scene.active;
}

void application_profile_editor_close(bool restore_focus) noexcept {
    if (!owner || !owner->scene.active)
        return;
    auto& p = *owner;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&p.layer);
    settings_layer_cancel_pointer(&p.layer);
    application_text_form_cancel_owner(color_valid);
    if (p.picker.runtime) {
        native_file_picker_cancel(&p.picker);
        native_file_picker_release(&p.picker, 0);
    }
    p.picking = Pick::none;
    close_dialog();
    // A queued request is dropped; one in flight finishes and is discarded.
    p.queued = false;
    p.saving = false;
    release_picked(p.avatar);
    release_picked(p.banner);
    release_picked(p.staged);
    g_app.renderer.settings_profile_preview = nullptr;
    g_app.renderer.settings_profile_dialog = nullptr;
    if (p.preview_open) {
        profile_scene_release(&p.preview);
        p.preview_open = false;
    }
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&p.scene);
    if (restore_focus && available())
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

void application_profile_editor_release() noexcept {
    application_profile_editor_close(false);
    if (!owner)
        return;
    if (owner->request_id && g_app.account.instance && g_app.account.api.profile_edit)
        g_app.account.api.profile_edit(g_app.account.instance, nullptr, owner->request_id);
    if (owner->picker.runtime) {
        native_file_picker_cancel(&owner->picker);
        native_file_picker_release(&owner->picker, 5'000);
    }
    release_picked(owner->avatar);
    release_picked(owner->banner);
    release_picked(owner->staged);
    ui_settings_scene_release(&owner->scene);
    for (auto& model : owner->models)
        message_composer_release(&model);
    SecureZeroMemory(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}

bool application_profile_editor_synchronize() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    if (!scope_current()) {
        application_profile_editor_close(false);
        return true;
    }
    bool changed = false;
    if (p.picker.runtime) {
        poll_picker();
        changed = !p.picker.runtime;
    }
    if (pump_previews(clock_monotonic_milliseconds()))
        changed = true;
    if (p.layer.choice && !settings_layer_choice_current(p.layer)) {
        p.layer.choice = false;
        changed = true;
    }
    if (changed) {
        dialog_update();
        return repaint();
    }
    if (!project())
        return false;
    if (p.dialog_kind != Dialog::none)
        dialog_update();
    return g_app.renderer.interaction_text_settings_revision == p.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

// Dialog input ----------------------------------------------------------------------------------------

namespace {
float client_x(LPARAM lparam) noexcept {
    return static_cast<i16>(LOWORD(lparam)) * 96.f / maximum(g_app.renderer.dpi, 1u);
}
float client_y(LPARAM lparam) noexcept {
    return static_cast<i16>(HIWORD(lparam)) * 96.f / maximum(g_app.renderer.dpi, 1u);
}
void set_zoom(float x) noexcept {
    auto& p = *owner;
    const float t = ui_profile_dialog_zoom_at(p.dialog, x);
    image_crop_set_zoom(&p.crop, kImageCropMinimumZoom + t * (kImageCropMaximumZoom - kImageCropMinimumZoom));
}
void dialog_action(UiProfileDialogAction action, u32 source) noexcept {
    auto& p = *owner;
    switch (action) {
    case UiProfileDialogAction::close:
    case UiProfileDialogAction::cancel:
        close_dialog();
        break;
    case UiProfileDialogAction::apply:
        apply_dialog();
        break;
    case UiProfileDialogAction::reset:
        image_crop_reset(&p.crop);
        break;
    case UiProfileDialogAction::rotate:
        image_crop_rotate(&p.crop);
        break;
    case UiProfileDialogAction::item:
        if (source < p.item_count) {
            p.item_selected = source;
            if (p.dialog_kind == Dialog::effect)
                p.effect_wanted = p.items[source].sku;
        }
        break;
    default:
        return;
    }
    dialog_update();
    repaint();
}
bool dialog_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto& p = *owner;
    auto& d = p.dialog;
    if (message == WM_MOUSEWHEEL) {
        const float delta = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 48.f;
        if (ui_profile_dialog_scroll(&d, delta))
            repaint();
        return true;
    }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(g_app.window, &point);
        const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
        const auto hit = ui_profile_dialog_hit_test(d, point.x * scale, point.y * scale);
        const auto action = ui_profile_dialog_action(d, hit);
        application_set_cursor(p.crop_dragging || action == UiProfileDialogAction::pan ? UiCursor::move
                               : action != UiProfileDialogAction::none                 ? UiCursor::hand
                                                                                      : UiCursor::arrow);
        return true;
    }
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
        p.crop_dragging = p.zoom_dragging = false;
        ui_button_pointer(&d.widgets, UiButtonPointerEvent::cancel, {});
        return false;
    }
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP &&
        message != WM_LBUTTONDBLCLK)
        return message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MOUSELEAVE;
    if (message == WM_LBUTTONDBLCLK)
        message = WM_LBUTTONDOWN;
    const float x = client_x(lparam), y = client_y(lparam);
    if (p.crop_dragging || p.zoom_dragging) {
        if (message == WM_MOUSEMOVE) {
            if (p.crop_dragging) {
                p.crop.offset_x = p.drag_offset_x + (x - p.drag_x);
                p.crop.offset_y = p.drag_offset_y + (y - p.drag_y);
                image_crop_clamp(&p.crop);
            } else
                set_zoom(x);
            dialog_update();
            repaint();
        } else if (message == WM_LBUTTONUP) {
            p.crop_dragging = p.zoom_dragging = false;
            if (GetCapture() == g_app.window)
                ReleaseCapture();
            dialog_update();
            repaint();
        }
        return true;
    }
    const auto hit = ui_profile_dialog_hit_test(d, x, y);
    u32 source = 0;
    const auto action = ui_profile_dialog_action(d, hit, &source);
    if (message == WM_LBUTTONDOWN && !ui_rect_contains(d.rectangle, x, y)) {
        // A click on the scrim closes the dialog like Cancel.
        close_dialog();
        repaint();
        return true;
    }
    if (message == WM_LBUTTONDOWN && (action == UiProfileDialogAction::pan || action == UiProfileDialogAction::zoom)) {
        SetCapture(g_app.window);
        if (action == UiProfileDialogAction::pan) {
            p.crop_dragging = true;
            p.drag_x = x;
            p.drag_y = y;
            p.drag_offset_x = p.crop.offset_x;
            p.drag_offset_y = p.crop.offset_y;
        } else {
            p.zoom_dragging = true;
            set_zoom(x);
            dialog_update();
            repaint();
        }
        return true;
    }
    const auto result = ui_button_pointer_target(&d.widgets,
                                                 message == WM_LBUTTONDOWN ? UiButtonPointerEvent::press
                                                 : message == WM_LBUTTONUP ? UiButtonPointerEvent::release
                                                                           : UiButtonPointerEvent::move,
                                                 hit);
    if (message == WM_LBUTTONDOWN && result.handled)
        SetCapture(g_app.window);
    if (message == WM_LBUTTONUP && GetCapture() == g_app.window)
        ReleaseCapture();
    if (result.changed) {
        ++d.revision;
        repaint();
    }
    if (result.activate) {
        u32 activated = 0;
        const auto act = ui_profile_dialog_action(d, result.target, &activated);
        dialog_action(act, activated);
    }
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, g_app.window, 0};
        TrackMouseEvent(&track);
    }
    (void)action;
    (void)source;
    return true;
}
bool dialog_key(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto& p = *owner;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return true;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_ESCAPE && !repeated) {
        close_dialog();
        repaint();
        return true;
    }
    if (key == VK_RETURN && !repeated) {
        apply_dialog();
        dialog_update();
        repaint();
        return true;
    }
    if (p.dialog_kind == Dialog::crop && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
        // As in the official client: 4 DIP per press, 40 with Shift.
        const float step = shift ? kImageCropFastKeyStep : kImageCropKeyStep;
        image_crop_pan(&p.crop, key == VK_LEFT ? -step : key == VK_RIGHT ? step : 0,
                       key == VK_UP ? -step : key == VK_DOWN ? step : 0);
        dialog_update();
        repaint();
        return true;
    }
    if (p.dialog_kind != Dialog::crop && p.item_count && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
        const i32 move = key == VK_LEFT ? -1 : key == VK_RIGHT ? 1 : key == VK_UP ? -3 : 3;
        const i32 next = static_cast<i32>(p.item_selected) + move;
        if (next >= 0 && next < static_cast<i32>(p.item_count))
            dialog_action(UiProfileDialogAction::item, static_cast<u32>(next));
        return true;
    }
    return true;
}
} // namespace

bool application_profile_editor_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (!owner || !owner->scene.active)
        return false;
    if (owner->dialog_kind != Dialog::none)
        return dialog_key(kind, key, repeated);
    return settings_layer_input(&owner->layer, kind, key, repeated);
}

SettingsLayer* application_profile_editor_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

bool application_profile_editor_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!application_profile_editor_active())
        return false;
    if (owner->dialog_kind != Dialog::none && owner->dialog.active)
        return dialog_message(message, wparam, lparam);
    return settings_layer_window_message(&owner->layer, message, wparam, lparam);
}

u64 application_profile_editor_next_wake() noexcept {
    if (!owner || !owner->scene.active)
        return 0;
    // The preview's effect product loads after the profile.
    const bool product = owner->loaded && owner->effect_wanted && owner->effect_wanted != owner->effect_loaded &&
                         !owner->queued && !owner->request_id;
    if (product)
        return 1;
    if (!owner->queued || owner->request_id)
        return 0;
    return owner->retry_at ? owner->retry_at : 1;
}

bool application_profile_editor_submit(u64 now) noexcept {
    if (!owner || owner->request_id || (owner->retry_at && owner->retry_at > now) || !g_app.account_ui ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none || !g_app.account.instance ||
        !g_app.account.api.profile_edit)
        return false;
    auto& p = *owner;
    if (!p.queued && p.loaded && p.effect_wanted && p.effect_wanted != p.effect_loaded)
        queue(Op::product);
    if (!p.queued)
        return false;
    if (!scope_current() || !connected()) {
        p.queued = false;
        if (p.op == Op::fetch)
            p.load_failed = true;
        else if (p.op == Op::save_account || p.op == Op::save_profile) {
            p.saving = false;
            p.failed = true;
            p.status = utf8_literal("Couldn't reach Discord. Try again.");
        }
        repaint();
        return false;
    }
    ProfileEditRequest request{};
    switch (p.op) {
    case Op::fetch:
        request.operation = ProfileEditOperation::profile_fetch;
        request.id = p.self_user_id;
        request.guild_id = p.tab == ProfileEditorTab::guild ? p.guild_id : 0;
        if (p.tab == ProfileEditorTab::guild && !p.guild_id) {
            p.queued = false;
            return false;
        }
        break;
    case Op::collectibles:
        request.operation = ProfileEditOperation::collectibles;
        break;
    case Op::product:
        request.operation = ProfileEditOperation::collectible_product;
        request.id = p.effect_wanted;
        break;
    case Op::save_account:
        if (p.tab == ProfileEditorTab::main) {
            request.operation = ProfileEditOperation::user_patch;
            request.user = &p.user_patch;
        } else {
            request.operation = ProfileEditOperation::member_patch;
            request.guild_id = p.guild_id;
            request.member = &p.member_patch;
        }
        break;
    case Op::save_profile:
        request.operation = p.tab == ProfileEditorTab::main ? ProfileEditOperation::user_profile_patch
                                                            : ProfileEditOperation::member_profile_patch;
        request.guild_id = p.tab == ProfileEditorTab::main ? 0 : p.guild_id;
        request.profile = &p.profile_patch;
        break;
    default:
        p.queued = false;
        return false;
    }
    const u64 id = next_request_id();
    const auto submitted = g_app.account.api.profile_edit(g_app.account.instance, &request, id);
    if (submitted.error == RestClientSubmitError::busy) {
        p.retry_at = deadline_after(now, 50);
        return true;
    }
    p.queued = false;
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "profile_editor", "submit_error", static_cast<u64>(submitted.error));
        if (p.op == Op::fetch)
            p.load_failed = true;
        else if (p.op == Op::collectibles)
            p.collectibles_failed = true;
        else if (p.op == Op::product)
            p.effect_loaded = p.effect_wanted;
        else {
            p.saving = false;
            p.failed = true;
            p.status = submitted.error == RestClientSubmitError::body_capacity
                           ? utf8_literal("This image is too large to upload.")
                           : Utf8View{};
        }
        dialog_update();
        repaint();
        return false;
    }
    p.request_id = id;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    repaint();
    return true;
}

bool application_profile_editor_owns_response(u64 request_id) noexcept {
    return request_id && owner && owner->request_id == request_id;
}

bool application_profile_editor_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!application_profile_editor_owns_response(response.request_id))
        return false;
    auto& p = *owner;
    p.request_id = 0;
    if (!p.scene.active || !scope_current())
        return true;
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    const Slice<u32> structurals{p.structurals, sizeof(p.structurals) / sizeof(u32)};
    switch (p.op) {
    case Op::fetch:
        if (ok)
            load_profile(body);
        else {
            p.load_failed = true;
            log_u64(LogLevel::warning, "profile_editor", "fetch_status", response.status_code);
        }
        break;
    case Op::collectibles: {
        const u32 count = ok ? profile_edit_decode_collectibles(body, structurals, {p.owned, kOwnedCapacity}) : ~0u;
        p.collectibles_failed = count == ~0u;
        p.collectibles_loaded = true;
        p.owned_count = count == ~0u ? 0 : count;
        if (p.dialog_kind == Dialog::decoration || p.dialog_kind == Dialog::effect)
            build_items(p.dialog_kind);
        break;
    }
    case Op::product:
        if (ok)
            profile_effect_decode(body, structurals, p.effect_wanted, &p.effect);
        p.effect_loaded = p.effect_wanted;
        p.preview.effect_started_ms = 0;
        break;
    case Op::save_account:
    case Op::save_profile: {
        if (!ok) {
            p.failed = true;
            p.status = failure(response, body);
            if (p.op == Op::save_account)
                p.account_failed = true;
        }
        // The official client sends the profile PATCH even when the account PATCH fails.
        if (p.op == Op::save_account && p.profile_patch.fields) {
            queue(Op::save_profile);
            break;
        }
        p.saving = false;
        if (!p.failed) {
            // Saved: reload the profile (and let the popout refetch).
            if (g_app.profile && g_app.profile->data.user_id == p.self_user_id)
                g_app.profile->loaded_at = 0;
            release_picked(p.avatar);
            release_picked(p.banner);
            queue_fetch();
        }
        break;
    }
    default:
        break;
    }
    if (!p.queued)
        p.op = Op::none;
    dialog_update();
    return repaint();
}

u64 application_profile_editor_caret_wake() noexcept {
    if (!application_profile_editor_active())
        return 0;
    u64 wake = settings_layer_next_wake(owner->layer);
    const bool previews = (owner->staged.data && !owner->staged.ready && !owner->staged.failed) ||
                          (owner->avatar.data && !owner->avatar.ready && !owner->avatar.failed) ||
                          (owner->banner.data && !owner->banner.ready && !owner->banner.failed);
    if (owner->picker.runtime || previews) {
        const u64 poll = deadline_after(clock_monotonic_milliseconds(), 50);
        wake = wake ? minimum(wake, poll) : poll;
    }
    if (owner->preview_open && profile_scene_animating(&owner->preview, clock_monotonic_milliseconds())) {
        const u64 frame = deadline_after(clock_monotonic_milliseconds(), 42);
        wake = wake ? minimum(wake, frame) : frame;
    }
    return wake;
}

void application_profile_editor_tick(u64 now) noexcept {
    if (application_profile_editor_active())
        settings_layer_tick(&owner->layer, now, g_app.preferences.reduced_motion);
}

bool application_profile_editor_edit_focused() noexcept {
    return application_profile_editor_active() && owner->dialog_kind == Dialog::none &&
           settings_layer_editor(&owner->layer, nullptr, nullptr, nullptr, nullptr);
}

MessageComposer* application_profile_editor_edit_model() noexcept {
    MessageComposer* model = nullptr;
    return application_profile_editor_edit_focused() &&
                   settings_layer_editor(&owner->layer, nullptr, &model, nullptr, nullptr)
               ? model
               : nullptr;
}

TextInputTarget application_profile_editor_edit_target() noexcept {
    BoundedEdit* binding = nullptr;
    return application_profile_editor_edit_focused() &&
                   settings_layer_editor(&owner->layer, nullptr, nullptr, &binding, nullptr)
               ? bounded_edit_input_target(binding)
               : TextInputTarget{};
}

void application_profile_editor_edit_cancel_native_group(TextInputTarget target) noexcept {
    if (owner)
        settings_layer_cancel_native_group(&owner->layer, target);
}

UiRect application_profile_editor_edit_bounds() noexcept {
    return application_profile_editor_active() ? settings_layer_edit_bounds(&owner->layer) : UiRect{};
}

bool application_profile_editor_edit_changed(bool content) noexcept {
    return application_profile_editor_active() && settings_layer_edit_changed(&owner->layer, content);
}

u32 application_profile_editor_owner_bytes() noexcept {
    return owner ? sizeof(*owner) : 0;
}

bool application_profile_editor_frame_current(MediaAssetKey key) noexcept {
    const auto* picked = picked_for(key);
    return picked && !picked->failed;
}

void application_profile_editor_frame_uploaded(MediaAssetKey key) noexcept {
    if (auto* picked = picked_for(key)) {
        picked->ready = true;
        picked->poll_at = 0;
        dialog_update();
        repaint();
    }
}

void application_profile_editor_frame_rejected(MediaAssetKey key) noexcept {
    if (auto* picked = picked_for(key)) {
        picked->ready = false;
        picked->failed = true;
        dialog_update();
        repaint();
    }
}

ProfileScene* application_profile_editor_preview() noexcept {
    return application_profile_editor_active() && owner->preview_open ? &owner->preview : nullptr;
}

const UiProfileDialogScene* application_profile_editor_dialog() noexcept {
    return application_profile_editor_active() && owner->dialog.active ? &owner->dialog : nullptr;
}

} // namespace maicord::app_detail

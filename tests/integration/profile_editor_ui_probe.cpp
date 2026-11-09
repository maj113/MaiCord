#include "profile_editor_ui_probe.hpp"
#include "../support/ui_scene_fixtures.hpp"
#include "probe_present.hpp"
#include "../../src/app/account/profile_editor.hpp"
#include "maicord/app/media/local_draft_preview.hpp"
#include "maicord/media/image/dominant_color.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/user/profile.hpp"
#include "maicord/ui/guild/card_scene.hpp"
#include "maicord/ui/widgets/tooltip.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/account/profile_dialog_scene.hpp"
#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/account/user_context_menu.hpp"
#include "maicord/ui/widgets/presence.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

// The probe links the application media pump without the editor owner: the
// editor scenes are bound to the renderer directly below.
namespace maicord::app_detail {
bool application_profile_editor_frame_current(const MediaAssetKey&) noexcept {
    return false;
}
void application_profile_editor_frame_uploaded(MediaAssetKey) noexcept {
}
void application_profile_editor_frame_rejected(MediaAssetKey) noexcept {
}
ProfileScene* application_profile_editor_preview() noexcept {
    return nullptr;
}
const UiProfileDialogScene* application_profile_editor_dialog() noexcept {
    return nullptr;
}
} // namespace maicord::app_detail

namespace maicord {
namespace {
bool require(bool value, u32 stage) noexcept {
    if (!value)
        log_u64(LogLevel::error, "probe", "profile_editor_failure_stage", stage);
    return value;
}
bool palette(void*, u64 control, u32 choice, BackgroundTheme* out) noexcept {
    *out = {};
    out->colors[0] = out->colors[1] = control == 9 ? 0x2B6E9Bu : choice ? 0x6B2FA8u : 0x1D3A6Eu;
    out->color_count = 2;
    return true;
}
struct Page {
    UiPage pages[3]{};
    UiPageRegistry registry{};
    Utf8View tabs[2]{utf8_literal("Main Profile"), utf8_literal("Per-server Profiles")};
    Utf8View avatar[2]{utf8_literal("Change Avatar"), utf8_literal("Remove Avatar")};
    Utf8View decoration[2]{utf8_literal("Change Decoration"), utf8_literal("Remove Decoration")};
    Utf8View effect[1]{utf8_literal("Change Effect")};
    Utf8View banner[2]{utf8_literal("Change Banner"), utf8_literal("Remove Banner")};
    Utf8View theme[2]{utf8_literal("Primary"), utf8_literal("Accent")};
    Utf8View accent[1]{utf8_literal("Banner Color")};
    Utf8View guilds[3]{utf8_literal("Native test server"), utf8_literal("Second server"), utf8_literal("Third")};
    MessageComposer models[3]{};
    char name[128]{}, pronouns[160]{}, bio[1200]{};
    UiSettingsEditor editors[3]{};
    UiSettingsControl controls[20]{};
    UiSettingsControlPresentation presentation[20]{};
    UiSettingsFooter footer{};
    UiSettingsNavEntry navigation[8]{};
    UiSettingsPageContent content{}, other{};
};
void load(MessageComposer* model, char* buffer, u32 capacity, Utf8View text) noexcept {
    message_composer_initialize(model, {buffer, capacity});
    copy_bytes(buffer, text.data, text.bytes);
    message_composer_refresh_borrowed(model, text.bytes);
}
// The Main Profile tab as application_profile_editor projects it.
u32 build(Page* p, bool nitro, bool guild) noexcept {
    load(&p->models[0], p->name, sizeof(p->name), guild ? utf8_literal("Robin-san") : utf8_literal("Native Profile"));
    load(&p->models[1], p->pronouns, sizeof(p->pronouns), utf8_literal("they/them"));
    load(&p->models[2], p->bio, sizeof(p->bio),
         utf8_literal("A profile card with a **wrapped** biography and _markdown_. Links like https://example.com work."));
    for (u32 i = 0; i < 3; ++i)
        p->editors[i] = {{message_composer_text(p->models[i]), 0, 0, false, false, true}, nullptr,
                         i == 0 ? utf8_literal("native.test") : i == 1 ? utf8_literal("Add your pronouns") : Utf8View{}};
    using K = UiSettingsControlKind;
    u32 n = 0;
    p->controls[n++] = {1, utf8_literal("Profiles"), {}, K::tabs, true, false, {p->tabs, 2}, guild ? 1u : 0u};
    if (guild)
        p->controls[n++] = {2, utf8_literal("Choose a Server"),
                            utf8_literal("Show who you are with different profiles for each of your servers."), K::choice,
                            true, false, {p->guilds, 3}, 0};
    p->controls[n++] = {3, guild ? utf8_literal("Server Nickname") : utf8_literal("Display Name"), {}, K::edit, true,
                        false, {}, 0, {}, &p->editors[0]};
    p->controls[n++] = {4, utf8_literal("Pronouns"), {}, K::edit, true, false, {}, 0, {}, &p->editors[1]};
    if (guild && !nitro)
        p->controls[n++] = {5, {},
                            utf8_literal("Customize your profile for every server, from avatar decorations to bios. "
                                         "Only with Nitro."),
                            K::notice, true, false};
    const bool gated = guild && !nitro;
    p->controls[n++] = {6, utf8_literal("Avatar"), gated ? utf8_literal("Only with Nitro.") : Utf8View{}, K::buttons,
                        !gated, false, {p->avatar, 2}, kUiSettingsNoChoice};
    p->controls[n++] = {7, utf8_literal("Avatar Decoration"), gated ? utf8_literal("Only with Nitro.") : Utf8View{},
                        K::buttons, !gated, false, {p->decoration, 2}, kUiSettingsNoChoice};
    p->controls[n++] = {8, utf8_literal("Profile Effect"), gated ? utf8_literal("Only with Nitro.") : Utf8View{},
                        K::buttons, !gated, false, {p->effect, 1}, kUiSettingsNoChoice};
    if (!guild && !nitro)
        p->controls[n++] = {9, utf8_literal("Banner Color"), {}, K::colors, true, false, {p->accent, 1},
                            kUiSettingsNoChoice};
    p->controls[n++] = {10, utf8_literal("Profile Banner"),
                        nitro ? utf8_literal("Upload a PNG, JPG, or GIF under 10MB. Images should be at least 680x240.")
                              : utf8_literal("Only with Nitro."),
                        K::buttons, nitro, false, {p->banner, 2}, kUiSettingsNoChoice};
    p->controls[n++] = {11, utf8_literal("Profile Theme"), nitro ? Utf8View{} : utf8_literal("Only with Nitro."),
                        K::colors, nitro, false, {p->theme, 2}, kUiSettingsNoChoice,
                        guild ? utf8_literal("Reset Theme") : Utf8View{}};
    p->controls[n++] = {12, utf8_literal("Bio"), utf8_literal("You can use markdown and links if you\xE2\x80\x99" "d like."),
                        K::multiline, !gated, false, {}, 0, utf8_literal("92"), &p->editors[2]};
    p->controls[n++] = {13, utf8_literal("Must be 190 or fewer in length."), {}, K::error, true, false};
    for (auto& pr : p->presentation)
        pr = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    p->footer = {utf8_literal("Careful \xE2\x80\x94 you have unsaved changes!"), utf8_literal("Save Changes"),
                 utf8_literal("Reset"), true, true, false, false};
    p->content = {utf8_literal("Profiles"), {}, {p->controls, n}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle,
                  &p->footer};
    p->content.presentation = p->presentation;
    p->content.sectioned = true;
    p->content.aside_width = 300;
    p->content.aside_row = 1;
    p->content.aside_label = guild ? utf8_literal("Preview For Native test server") : utf8_literal("Preview");
    p->other = {utf8_literal("Account"), {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle};
    p->navigation[0] = {UiSettingsNavKind::header, false, UiIcon::count, utf8_literal("USER SETTINGS"), {}, 0};
    p->navigation[1] = {UiSettingsNavKind::page, false, UiIcon::user, utf8_literal("My Account"), {1, 1}, 0};
    p->navigation[2] = {UiSettingsNavKind::page, false, UiIcon::user_edit, utf8_literal("Profiles"), {1, 2}, 0};
    p->navigation[3] = {UiSettingsNavKind::page, false, UiIcon::smartphone, utf8_literal("Devices"), {1, 3}, 0};
    return n;
}
void release(Page* p) noexcept {
    for (auto& model : p->models)
        message_composer_release(&model);
}
} // namespace

bool member_overlay_capacity_probe(D3D11Renderer& renderer, UiTextMeasureCache* measure, const ChatStateStore* state,
                                   const UserStore* users) noexcept;
bool guild_card_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache* measure,
                         bool (*capture)(const wchar_t*) noexcept) noexcept;

namespace {
// The Profiles settings page; its Restore unbinds the settings scenes on return.
bool profile_editor_pages(D3D11Renderer& renderer, UiTextMeasureCache* measure, const ChatStateStore* state,
                             const UserStore* users, bool (*capture)(const wchar_t*) noexcept) noexcept {
    const auto* previous_settings = renderer.settings_scene;
    const auto* previous_preview = renderer.settings_profile_preview;
    const auto* previous_dialog = renderer.settings_profile_dialog;
    static Page page{};
    static UserProfile profile{};
    static ProfileScene preview{};
    static UiProfileDialogScene dialog{};
    static UiSettingsScene scene{};
    struct Restore {
        D3D11Renderer& renderer;
        const UiSettingsScene* settings;
        const ProfileScene* old_preview;
        const UiProfileDialogScene* old_dialog;
        ~Restore() noexcept {
            renderer.settings_profile_preview = old_preview;
            renderer.settings_profile_dialog = old_dialog;
            renderer_bind_settings_scene(&renderer, settings);
            profile_scene_release(&preview);
            ui_settings_scene_release(&scene);
            release(&page);
        }
    } restore{renderer, previous_settings, previous_preview, previous_dialog};
    u32 scratch[2048];
    const auto json = utf8_literal(
        R"({"user":{"id":"4242","username":"native.test","global_name":"Native Profile","avatar":"abc123"},)"
        R"("user_profile":{"bio":"A profile card with a **wrapped** biography and _markdown_. Links like https://example.com work.",)"
        R"("pronouns":"they/them","theme_colors":[1915502,7024552],"banner":"a_testbanner"},)"
        R"("premium_type":2,"badges":[{"id":"premium","icon":"abcdef","description":"Nitro subscriber"}]})");
    const float scale = 96.0f / renderer.dpi;
    const UiRect viewport{0, 0, renderer.width * scale, renderer.height * scale};
    const auto show = [&](bool nitro, bool guild, const wchar_t* name, UiRect view, Utf8View bio = {},
                          bool reverse = false) noexcept {
        release(&page);
        page = {};
        build(&page, nitro, guild);
        // Ctrl+A in the focused Bio field: the whole text selected, as the settings layer projects it.
        if (bio.bytes) {
            message_composer_release(&page.models[2]);
            load(&page.models[2], page.bio, sizeof(page.bio), bio);
            page.editors[2] = {{message_composer_text(page.models[2]), reverse ? bio.bytes : 0u,
                                reverse ? 0u : bio.bytes, true, true, true},
                               &renderer.media_picker_edit_geometry,
                               {}};
        }
        // A new content revision per frame, as the editor bumps it on every
        // projection; an unchanged revision keeps the scene's previous layout.
        static u32 revision = 0;
        page.content.revision = ++revision;
        bool ok = require(user_profile_decode(json, {scratch, 2048}, 4242, guild ? 100 : 0, &profile), 1);
        if (!nitro)
            profile.has_theme = false;
        // Preview first at a nominal anchor to learn its height.
        preview.presentation = ProfilePresentation::preview;
        preview.measure = measure;
        preview.phase = ProfilePhase::ready;
        preview.presence = UiPresence::online;
        ok = ok && require(profile_scene_open(&preview, &profile, state, users, 1, {0, 0, 300, 400}, view), 2);
        page.content.aside_height = preview.preview_height > 0 ? preview.preview_height : 400;
        int context = 0;
        ok = ok && require(ui_page_registry_initialize(&page.registry, {page.pages, 3}) &&
                               ui_page_registry_register(&page.registry, {{1, 2}, utf8_literal("Profiles"), &page.content})
                                   .ok() &&
                               ui_page_registry_register(&page.registry, {{1, 1}, utf8_literal("My Account"), &page.other})
                                   .ok() &&
                               ui_page_registry_register(&page.registry, {{1, 3}, utf8_literal("Devices"), &page.other})
                                   .ok() &&
                               ui_page_registry_select(&page.registry, {1, 2}).ok(),
                           3);
        // These report whether anything changed; the scene is reused across frames.
        ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal);
        ui_settings_scene_bind_navigation(&scene, {page.navigation, 4}, 1);
        ui_settings_scene_bind_palette(&scene, {&context, palette, 1});
        ok = ok && require(ui_settings_scene_update(&scene, &page.registry, view), 4);
        if (bio.bytes)
            ok = ok && require(ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, {1, 2}, 12}), 11);
        UiRect heading{}, card{}, clip{};
        if (ok && ui_settings_scene_aside(scene, &heading, &card, &clip)) {
            preview.anchor = card;
            ok = require(profile_scene_layout(&preview, view, true), 5);
            renderer.settings_profile_preview = &preview;
        } else
            renderer.settings_profile_preview = nullptr;
        renderer_bind_settings_scene(&renderer, &scene);
        ok = ok && require(renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                               probe_present(&renderer, kDefaultTheme) && (!name || capture(name)),
                           6);
        // The next projection (a blink, the next key) takes the selection geometry that frame published.
        if (bio.bytes) {
            page.content.revision = ++revision;
            ok = ok && require(ui_settings_scene_update(&scene, &page.registry, view) &&
                                   renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                   probe_present(&renderer, kDefaultTheme),
                               12);
        }
        return ok;
    };
    bool ok = show(true, false, L"profile-editor-main.bmp", viewport) &&
              show(false, false, L"profile-editor-main-free.bmp", viewport) &&
              show(false, true, L"profile-editor-guild.bmp", viewport) &&
              show(true, false, L"profile-editor-narrow.bmp", {0, 0, viewport.right * .62f, viewport.bottom});
    const Utf8View bios[]{
        utf8_literal("First line\n\nThird line after a blank one\n"),
        utf8_literal("\n\n\n\n\n\n\n\n\n\n"),
        utf8_literal("A long first line that wraps across the width of the bio field more than once, twice even.\n\n"
                     "Hello \xF0\x9F\x98\x80 world \xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD\n"
                     "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D and back\n\n"),
        utf8_literal("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n"),
        utf8_literal("Windows\r\nline\r\n\r\nends\r\n"),
    };
    for (u32 i = 0; ok && i < sizeof(bios) / sizeof(bios[0]) * 2; ++i) {
        log_u64(LogLevel::info, "probe", "profile_editor_bio_select_all", i);
        ok = show(true, false, i ? nullptr : L"profile-editor-bio-selected.bmp", viewport, bios[i / 2], i & 1);
    }
    // A picked avatar shown in the preview and the cropper.
    constexpr u32 w = 240, h = 180; // fits the probe's 256 px atlases
    static u8 rgba[w * h * 4];
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* px = rgba + (y * w + x) * 4;
            px[0] = static_cast<u8>(x * 255 / w);
            px[1] = static_cast<u8>(y * 255 / h);
            px[2] = static_cast<u8>(((x / 40 + y / 40) & 1) ? 220 : 60);
            px[3] = 255;
        }
    const auto key = media_asset_local_draft_preview(0x5052000000000001ull, 1u | kLocalDraftPreviewThumbnailVariant,
                                                     MediaAttachmentFormat::png);
    ok = ok && require(renderer_upload_media_straight_rgba8(&renderer, key, w, h, w * 4, {rgba, sizeof(rgba)}).ok(), 7);
    const auto editor = ui_profile_dialog_crop_editor(viewport);
    auto crop = image_crop_begin(ImageCropUse::avatar, w, h, ui_rect_width(editor), ui_rect_height(editor));
    image_crop_set_zoom(&crop, 1.4f);
    image_crop_pan(&crop, 30, -10);
    if (ok) {
        ok = show(true, false, L"profile-editor-pending-base.bmp", viewport);
        preview.pending_avatar = {key, crop, true};
        ok = ok && require(profile_scene_layout(&preview, viewport, true) && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                               probe_present(&renderer, kDefaultTheme) && capture(L"profile-editor-pending.bmp"),
                           8);
    }
    // A Nitro preview without theme colours themes from the picked avatar as cropped: the crop palette of a picture
    // uploaded at half its source size (a 240x180 source; green left half, red right half) reads only the cropped
    // half, sampled at the drawn size, turned or not.
    {
        constexpr u32 hw = 120, hh = 90;
        static u8 halves[hw * hh * 4];
        for (u32 i = 0; i < hw * hh; ++i) {
            const bool left = i % hw < hw / 2;
            halves[i * 4] = left ? 0x08 : 0xC8;
            halves[i * 4 + 1] = left ? 0x89 : 0x28;
            halves[i * 4 + 2] = left ? 0x59 : 0x1E;
            halves[i * 4 + 3] = 255;
        }
        const auto picked = media_asset_local_draft_preview(0x5052000000000002ull, 1u | kLocalDraftPreviewThumbnailVariant,
                                                            MediaAttachmentFormat::png);
        u32 left[kImagePaletteColors]{}, right[kImagePaletteColors]{};
        ok = ok && require(renderer_upload_media_straight_rgba8(&renderer, picked, hw, hh, hw * 4,
                                                                {halves, sizeof(halves)})
                                   .ok() &&
                               renderer_media_crop_palette(&renderer, picked, {10, 10, 80, 80, 240, 80, 0}, left) &&
                               left[0] == 0x0C8C5C &&
                               renderer_media_crop_palette(&renderer, picked, {150, 90, 80, 80, 240, 80, 90}, right) &&
                               right[0] == 0xCC2C1C,
                           97);
    }
    const auto crop_frame = [&](ImageCropState state, const wchar_t* name) noexcept {
        UiProfileDialogContent content{};
        content.kind = UiProfileDialogKind::crop;
        content.title = utf8_literal("Edit Image");
        content.apply_label = utf8_literal("Apply");
        content.image = key;
        content.crop = state;
        content.image_ready = true;
        content.can_apply = true;
        content.can_reset = image_crop_edited(state);
        renderer.settings_profile_dialog = &dialog;
        return require(ui_profile_dialog_update(&dialog, content, viewport) &&
                           renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                           probe_present(&renderer, kDefaultTheme) && capture(name),
                       9);
    };
    ok = ok && crop_frame(crop, L"profile-editor-crop-avatar.bmp");
    auto banner = image_crop_begin(ImageCropUse::banner, w, h, ui_rect_width(editor), ui_rect_height(editor));
    ok = ok && crop_frame(banner, L"profile-editor-crop-banner.bmp");
    image_crop_rotate(&crop);
    ok = ok && crop_frame(crop, L"profile-editor-crop-rotated.bmp");
    static UiProfileDialogItem items[7]{};
    const Utf8View labels[] = {{}, utf8_literal("Cat Ears"), utf8_literal("Halo"), utf8_literal("Sakura"),
                               utf8_literal("Flames"), utf8_literal("Snow"), utf8_literal("Glitch")};
    for (u32 i = 0; i < 7; ++i) {
        items[i] = {};
        items[i].sku = i ? 100 + i : 0;
        items[i].label = labels[i];
    }
    UiProfileDialogContent picker{};
    picker.kind = UiProfileDialogKind::decoration;
    picker.title = utf8_literal("Change Avatar Decoration");
    picker.apply_label = utf8_literal("Apply");
    picker.items = {items, 7};
    picker.selected = 2;
    picker.preview_label = labels[2];
    picker.can_apply = true;
    ok = ok && require(ui_profile_dialog_update(&dialog, picker, viewport) &&
                           renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                           probe_present(&renderer, kDefaultTheme) && capture(L"profile-editor-decorations.bmp"),
                       10);
    ui_profile_dialog_close(&dialog);
    return ok;
}
} // namespace

bool profile_editor_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache* measure, const ChatStateStore* state,
                             const UserStore* users, bool (*capture)(const wchar_t*) noexcept) noexcept {
    // The shell overlays run once Settings is closed again.
    const bool ok = profile_editor_pages(renderer, measure, state, users, capture) &&
                    member_overlay_capacity_probe(renderer, measure, state, users) &&
                    guild_card_ui_probe(renderer, measure, capture);
    log_write(ok ? LogLevel::info : LogLevel::error, "probe",
              ok ? "profile_editor_native_ui_passed" : "profile_editor_native_ui_failed");
    return ok;
}

// Member list → right click → Profile / Roles over the full shell: a profile
// popout of a member with many roles and badges, the 48-row Roles menu, and
// both together, with the member row keeping keyboard focus. Every frame must
// present; an overflowing paint stream used to reject them all, freezing the
// window until the overlay closed.
bool member_overlay_capacity_probe(D3D11Renderer& renderer, UiTextMeasureCache* measure, const ChatStateStore* state,
                                   const UserStore* users) noexcept {
    auto* chat = const_cast<ChatStateStore*>(state);
    // The fixture shell may show no member list; a bound one keeps row focus.
    auto* members = renderer.member_list_scene && renderer.member_list_scene->active
                        ? const_cast<MemberListScene*>(renderer.member_list_scene)
                        : nullptr;
    if (!require(chat != nullptr, 20))
        return false;
    constexpr u64 guild = 100;
    constexpr u32 role_count = kUserContextRoleItems;
    char names[role_count][16]{};
    for (u32 i = 0; i < role_count; ++i)
        wsprintfA(names[i], "Role %u", i + 1);
    // As many server roles as the fixture store holds (the popout clamps its
    // pills to two lines; the Roles menu lists all 48 by name).
    u32 stored = 0;
    while (stored < 24 && chat_state_store_upsert_role(chat, {9000 + stored, guild, 0,
                                                              {names[stored], static_cast<u32>(lstrlenA(names[stored]))},
                                                              200 - stored, 0x203040u + stored * 0x030507u}))
        ++stored;
    if (!require(stored >= 4, 21))
        return false;
    char json[4096]{};
    u32 used = static_cast<u32>(wsprintfA(json, "%s",
                                          R"({"user":{"id":"4343","username":"many.roles","global_name":"Many Roles",)"
                                          R"("avatar":"abc123"},"user_profile":{"bio":"Owner of a server with a long )"
                                          R"(list of roles and badges.","pronouns":"he/him"},"guild_member":{"roles":[)"));
    for (u32 i = 0; i < 24; ++i)
        used += static_cast<u32>(wsprintfA(json + used, i ? ",\"%u\"" : "\"%u\"", 9000 + i % stored));
    used += static_cast<u32>(wsprintfA(json + used, "%s", R"(],"joined_at":"2021-03-14T12:00:00.000Z"},"badges":[)"));
    for (u32 i = 0; i < 12; ++i)
        used += static_cast<u32>(wsprintfA(json + used, R"(%s{"id":"badge%u","icon":"abcdef%u","description":"Badge %u"})",
                                           i ? "," : "", i, i, i));
    used += static_cast<u32>(wsprintfA(json + used, "%s", "]}"));
    static u32 scratch[4096];
    static UserProfile profile{};
    static ProfileScene popout{};
    static PopupItem items[role_count]{};
    static PopupRowWidget rows[role_count]{};
    static UiWidget widgets[1024]{};
    static UiLayoutNode nodes[1024]{};
    static PopupScene menu{};
    struct Restore {
        D3D11Renderer& renderer;
        const ProfileScene* profile;
        const PopupScene* popup;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.profile_scene = const_cast<ProfileScene*>(profile);
            renderer_bind_popup_scene(&renderer, popup);
            renderer_bind_focus_owner(&renderer, focus);
            profile_scene_release(&popout);
            popup_scene_release(&menu);
        }
    } restore{renderer, renderer.profile_scene, renderer.popup_scene, renderer.focus_owner};
    const float scale = 96.0f / renderer.dpi;
    const UiRect viewport{0, 0, renderer.width * scale, renderer.height * scale};
    u64 focused = 0;
    for (u32 i = 0; members && i < members->count && !focused; ++i)
        if (members->rows[i].kind == MemberListRowKind::member && members->rows[i].user_id)
            focused = members->rows[i].user_id;
    if (!require((!focused || member_list_scene_focus_user(members, focused)) &&
                     user_profile_decode({json, used}, {scratch, 4096}, 4343, guild, &profile) &&
                     profile.role_count == 24 && profile.badge_count == 12,
                 22))
        return false;
    if (focused)
        renderer_bind_focus_owner(&renderer, &members->widgets);
    if (!focused)
        focused = 4343;
    u32 peak = 0;
    const auto present = [&](u32 stage) noexcept {
        const bool presented = renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                               probe_present(&renderer, kDefaultTheme);
        log_u64(LogLevel::info, "probe", "member_overlay_paint_batches", renderer.paint_stream.batch_count);
        log_u64(LogLevel::info, "probe", "member_overlay_paint_clips", renderer.paint_stream.clip_count);
        peak = maximum(peak, maximum(renderer.paint_stream.batch_count, renderer.paint_stream.clip_count));
        return require(presented && !renderer.paint_stream.rejected, stage);
    };
    // Profile from the context menu: a popout beside the pointer.
    popout.measure = measure;
    popout.phase = ProfilePhase::ready;
    popout.presence = UiPresence::online;
    const UiRect pointer{viewport.right - 140, 160, viewport.right - 139, 161};
    if (!require(profile_scene_open(&popout, &profile, state, users, 1, pointer, viewport), 23))
        return false;
    renderer.profile_scene = &popout;
    if (!present(24))
        return false;
    // Roles, from the member menu: every editable role, half of them held.
    UserContextRole roles[role_count]{};
    for (u32 i = 0; i < role_count; ++i)
        roles[i] = {{names[i], static_cast<u32>(lstrlenA(names[i]))}, 0x203040u + i * 0x030507u, (i & 1) == 0, true};
    const u32 count = user_context_roles_build({roles, role_count}, {items, role_count});
    if (!require(count == role_count && popup_scene_initialize(&menu, {items, role_count}, {rows, role_count},
                                                               {widgets, 1024}, {nodes, 1024}),
                 25))
        return false;
    PopupItem copy[role_count];
    copy_bytes(copy, items, sizeof(copy));
    if (!require(popup_scene_open(&menu, PopupKind::context_menu, focused, {copy, count}, pointer, viewport, 240.f)
                         .ok() &&
                     renderer_bind_popup_scene(&renderer, &menu),
                 26))
        return false;
    renderer.profile_scene = nullptr;
    if (!present(27))
        return false;
    // The profile's own Add Role menu over the open popout.
    renderer.profile_scene = &popout;
    // These frames need more than the former 64 clips or batches per frame.
    return present(28) && require(peak > 64, 29);
}

// The server tag popout over the shell: loading, a ready profile whose icon
// and tag badge are resident (drawn over their plates) with Join, a member's
// tag card (boost badge and tag chip on the name line, its banner in the
// icon's colour read back from the atlas, two lines of traits then "+ 2 more"),
// and the private card. Every frame presents without paint stream rejections.
bool guild_card_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache* measure,
                         bool (*capture)(const wchar_t*) noexcept) noexcept {
    static UiGuildCardScene card{};
    struct Restore {
        D3D11Renderer& renderer;
        const UiGuildCardScene* previous;
        ~Restore() noexcept {
            renderer.guild_card_scene = previous;
            ui_guild_card_close(&card);
        }
    } restore{renderer, renderer.guild_card_scene};
    const float scale = 96.0f / renderer.dpi;
    const UiRect viewport{0, 0, renderer.width * scale, renderer.height * scale};
    const UiRect chip{viewport.right * .35f, 160, viewport.right * .35f + 56, 180};
    UiGuildCardContent content{};
    content.guild_id = 1234567890123456789ull;
    content.phase = UiGuildCardPhase::loading;
    const auto present = [&](u32 stage, const wchar_t* name) noexcept {
        return require(ui_guild_card_show(&card, content.guild_id, content, chip, viewport, 1000, true, measure) &&
                           (renderer.guild_card_scene = &card, true) &&
                           renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                           probe_present(&renderer, kDefaultTheme) && !renderer.paint_stream.rejected &&
                           (!name || capture(name)),
                       stage);
    };
    if (!present(30, nullptr))
        return false;
    constexpr u32 w = 64, h = 64;
    static u8 rgba[w * h * 4];
    for (u32 i = 0; i < w * h; ++i) {
        rgba[i * 4] = static_cast<u8>(40 + (i % w) * 3);
        rgba[i * 4 + 1] = 90;
        rgba[i * 4 + 2] = static_cast<u8>(200 - (i / w) * 2);
        rgba[i * 4 + 3] = 255;
    }
    auto icon = media_asset_embed(0x4743000000000001ull, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::png,
                                  false, MediaResolutionTier::px512);
    icon.kind = MediaAssetKind::profile_image;
    auto badge = icon;
    badge.id = 0x4743000000000002ull;
    if (!require(renderer_upload_media_straight_rgba8(&renderer, icon, w, h, w * 4, {rgba, sizeof(rgba)}).ok() &&
                     renderer_upload_media_straight_rgba8(&renderer, badge, w, h, w * 4, {rgba, sizeof(rgba)}).ok(),
                 31))
        return false;
    content.phase = UiGuildCardPhase::ready;
    content.name = utf8_literal("Native Server");
    content.description = utf8_literal("A friendly place for native client testing, with a description that wraps "
                                       "onto a second line.");
    content.tag = utf8_literal("GEAR");
    content.traits[0] = utf8_literal("Gaming");
    content.traits[1] = utf8_literal("Art");
    content.traits[2] = utf8_literal("Chill");
    content.trait_count = 3;
    content.online_count = 1234;
    content.member_count = 56789;
    content.banner_color = 0x5865F2;
    content.has_banner_color = true;
    content.action = UiGuildCardAction::join;
    if (!present(32, nullptr))
        return false;
    // The resident icon and tag badge are drawn over their plates.
    const u32 media_without = renderer.media_quad_count;
    content.icon = icon;
    content.badge = badge;
    if (!present(33, L"guild-card-ready.bmp") || !require(renderer.media_quad_count == media_without + 2, 33))
        return false;
    // A member's tag card without a brand colour: the banner glows in the icon's dominant colour, read back from the
    // atlas (the quantizer's cell centre: 161,108,171 reads 164,108,172).
    for (u32 i = 0; i < w * h; ++i) {
        rgba[i * 4] = 161;
        rgba[i * 4 + 1] = 108;
        rgba[i * 4 + 2] = 171;
    }
    auto flat_icon = icon;
    flat_icon.id = 0x4743000000000003ull;
    u32 icon_color = 0;
    if (!require(renderer_upload_media_straight_rgba8(&renderer, flat_icon, w, h, w * 4, {rgba, sizeof(rgba)}).ok() &&
                     renderer_media_dominant_color(&renderer, flat_icon, &icon_color) && icon_color == 0xA46CAC,
                 35))
        return false;
    content.name = utf8_literal("YURI!!!!");
    content.description = {};
    content.tag = utf8_literal("YURI");
    content.icon = flat_icon;
    content.server = {false, false, false, false, false, 2, 14};
    content.traits[0] = utf8_literal("YURI!!!!!!!!!!!!!!!!!!");
    content.traits[1] = utf8_literal("!!!!!!!!!!!!!!!!!!!!!!");
    content.traits[2] = utf8_literal("!!!!!!!!!!!!!!!!!!!!!!");
    content.traits[3] = utf8_literal("!!!!!!!!!!!!!!!!!!!!!!");
    content.traits[4] = utf8_literal("YURI!!!!!!!!!!!!!!!!!!");
    content.trait_count = 5;
    content.online_count = 4414;
    content.member_count = 12518;
    content.banner_color = icon_color;
    content.action = UiGuildCardAction::none;
    if (!present(36, L"guild-card-tag.bmp"))
        return false;
    u32 glows = 0;
    for (u32 i = 0; i < renderer.paint_stream.count; ++i)
        glows += renderer.paint_stream.quads[i].border_width == kPaintBannerGlowMode;
    if (!require(glows == 1 && card.server_badge_kind == UiGuildCardBadge::boost && card.visible_traits < 5 &&
                     card.overflow_bytes && card.overflow.top <= card.traits[0].bottom + 4 + 1,
                 37))
        return false;
    // The "+ N more" tooltip shows the folded traits as chips; the boost badge's names its level and boosts.
    static UiTooltipScene tip{};
    static UiWidgetManager overlay{};
    static UiWidget overlay_root[1];
    static UiLayoutNode overlay_nodes[1];
    struct TooltipRestore {
        D3D11Renderer& renderer;
        const UiTooltipScene* previous;
        ~TooltipRestore() noexcept {
            ui_tooltip_clear(&tip);
            renderer.tooltip_scene = previous;
        }
    } tip_restore{renderer, renderer.tooltip_scene};
    auto root_style = ui_layout_style();
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    if (!require(ui_widget_manager_initialize(&overlay, {overlay_root, 1}, {overlay_nodes, 1}), 38))
        return false;
    const auto root = ui_widget_manager_add(
        &overlay, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Tooltip Overlay"), root_style}});
    if (!require(root.ok() && ui_widget_manager_layout_rectangle(&overlay, viewport).ok() &&
                     ui_tooltip_bind_text_owner(&tip, {&overlay, root.handle}),
                 38))
        return false;
    renderer.tooltip_scene = &tip;
    const auto show_hint = [&](const UiRect& source, u32 stage, const wchar_t* name) noexcept {
        UiGuildCardHint hint{};
        if (!ui_guild_card_hint(card, card.panel.left + (source.left + source.right) * .5f,
                                card.panel.top + (source.top + source.bottom) * .5f, &hint))
            return require(false, stage);
        UiTooltipTarget target{
            {card.owner, 1, card.identity, card.revision, 0x4743'0000u | static_cast<u32>(hint.kind) << 8},
            hint.anchor,
            hint.text};
        target.layout = hint.kind == UiGuildCardHintKind::traits         ? UiTooltipLayout::chips
                        : hint.kind == UiGuildCardHintKind::server_badge ? UiTooltipLayout::card
                        : hint.kind == UiGuildCardHintKind::games        ? UiTooltipLayout::rows
                                                                         : UiTooltipLayout::label;
        target.row_media = hint.media;
        target.placement =
            hint.kind == UiGuildCardHintKind::traits ? UiTooltipPlacement::above : UiTooltipPlacement::below;
        UiTooltipStyle style{};
        style.maximum_height = 240;
        renderer.retained_presentation.valid = false;
        return require(ui_tooltip_offer(&tip, target, viewport, UiTooltipTrigger::pointer, 2000, style) &&
                           ui_tooltip_tick(&tip, 3000, measure) && ui_tooltip_visible(tip) &&
                           renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                           probe_present(&renderer, kDefaultTheme) && !renderer.paint_stream.rejected && capture(name),
                       stage);
    };
    if (!show_hint(card.overflow, 39, L"guild-card-traits-tooltip.bmp") ||
        !require(tip.layout == UiTooltipLayout::chips && tip.chip_count == 2, 40) ||
        !show_hint(card.server_badge, 41, L"guild-card-badge-tooltip.bmp") ||
        !require(tip.layout == UiTooltipLayout::card && tip.detail_size.height > 0, 41))
        return false;
    ui_tooltip_clear(&tip);
    // A server with games and trait emoji: five game pictures, the sixth's dimmed under "+2", a Unicode emoji
    // leading a trait.
    MediaAssetKey games[kUiGuildCardGames + 1];
    for (u32 g = 0; g <= kUiGuildCardGames; ++g) {
        for (u32 i = 0; i < w * h; ++i) {
            rgba[i * 4] = static_cast<u8>(60 + g * 30);
            rgba[i * 4 + 1] = static_cast<u8>(200 - g * 25);
            rgba[i * 4 + 2] = static_cast<u8>(90 + (i % w));
        }
        games[g] = icon;
        games[g].id = 0x4743000000000010ull + g;
        if (!require(renderer_upload_media_straight_rgba8(&renderer, games[g], w, h, w * 4, {rgba, sizeof(rgba)}).ok(),
                     42))
            return false;
    }
    constexpr Utf8View names[]{utf8_literal("Doki Doki Literature Club"), utf8_literal("Minecraft"),
                               utf8_literal("Terraria"), utf8_literal("Stardew Valley"), utf8_literal("Celeste")};
    for (u32 g = 0; g < kUiGuildCardGames; ++g) {
        content.games[g] = games[g];
        content.game_names[g] = names[g];
    }
    content.games_more_icons[0] = games[kUiGuildCardGames];
    content.games_more_icons[1] = games[0];
    content.games_more_names = utf8_literal("Hollow Knight\nPortal 2");
    content.game_count = 7;
    content.game_high[1] = true;
    content.trait_emoji[0] = utf8_literal("\xF0\x9F\x92\x9C");
    content.description = utf8_literal("A cozy server for yuri fans.");
    const u32 media_before_games = renderer.media_quad_count;
    if (!present(43, L"guild-card-games.bmp") ||
        !require(card.shown_games == 5 && card.games_more_bytes && renderer.media_quad_count >= media_before_games + 6,
                 43))
        return false;
    // "+N"'s tooltip: each other game beside its picture.
    const u32 media_before_rows = renderer.media_quad_count;
    if (!show_hint(card.games_more, 44, L"guild-card-games-tooltip.bmp") ||
        !require(tip.layout == UiTooltipLayout::rows && tip.row_count == 2 &&
                     renderer.media_quad_count >= media_before_rows + 2,
                 44))
        return false;
    ui_tooltip_clear(&tip);
    renderer.tooltip_scene = tip_restore.previous;
    content.games[0] = {};
    content.game_count = 0;
    content.trait_emoji[0] = {};
    content.description = {};
    content.phase = UiGuildCardPhase::private_profile;
    return present(34, L"guild-card-private.bmp");
}
} // namespace maicord

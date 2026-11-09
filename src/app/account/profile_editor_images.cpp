#include "profile_editor_internal.hpp"
#include "../media/file_over_limit.hpp"
#include "maicord/os/file.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "profile_editor.hpp"
#include "application_account_logout.hpp"
#include "../settings/settings.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/widgets/color_picker.hpp"
#include "maicord/media/image/dominant_color.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/ui/account/profile_media.hpp"

namespace maicord::app_detail {
namespace {

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
    const HANDLE file = file_open(staged.path, {FileAccess::read, FileCreate::existing, kFileShareRead, kFileSequential});
    if (file == INVALID_HANDLE_VALUE) {
        release_picked(staged);
        return false;
    }
    u64 size = 0;
    if (!file_size(file, &size) || size == 0 || size > kProfileImageSourceMaximumBytes) {
        file_close(file);
        if (size > kProfileImageSourceMaximumBytes)
            *error = application_file_over_limit_text(path, size, kProfileImageSourceMaximumBytes,
                                                      {p.size_error, sizeof(p.size_error)});
        release_picked(staged);
        return false;
    }
    auto* data = static_cast<u8*>(
        commit_pages(static_cast<SIZE_T>(size)));
    u32 total = 0;
    while (data && total < size) {
        u32 read = 0;
        if (!file_read(file, data + total, static_cast<u32>(size - total), &read) || !read)
            break;
        total += read;
    }
    if (!snapshot.bytes)
        file_snapshot_read(file, &snapshot);
    file_close(file);
    if (!data || total != size) {
        if (data)
            pages_release(data);
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

} // namespace
namespace profile_editor_detail {

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
                reset_value(&p.status);
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

} // namespace profile_editor_detail
namespace {

void start_pick(Pick pick) noexcept {
    auto& p = *owner;
    if (p.picker.runtime || !connected() || !editable())
        return;
    p.picker_consumed = false;
    p.picking = pick;
    if (native_file_picker_start(&p.picker, g_app.window, 1, utf8_literal("Select an Image")) !=
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
    auto* uri = static_cast<u8*>(
        commit_pages(kAccountProfileImageUriCapacity));
    if (!uri)
        return false;
    ProfileImageRequest request{};
    request.source = {picked.data, picked.bytes};
    request.crop = image_crop_output(picked.crop);
    ProfileImageInfo info{};
    request.info = &info;
    request.data_uri = {uri, kAccountProfileImageUriCapacity};
    if (!g_app.account.api.profile_image(g_app.account.instance, &request) || !request.data_uri_bytes) {
        pages_release(uri);
        return false;
    }
    picked.uri = uri;
    picked.uri_bytes = request.data_uri_bytes;
    picked.has_md5 = request.has_original_md5;
    if (picked.has_md5)
        copy_bytes(picked.md5, request.original_md5, 32);
    return true;
}

// Colours (Primary / Accent / Banner Color) through the colour picker. Every change recolours the preview live;
// closing without Apply restores the colours the draft had when the picker opened. The suggestions are the saved
// avatar's palette, as the official client suggests them.
// Name colours are color_name + their index (up to kProfileNameStyleColors).
enum ColorSlot : u64 { color_primary = 1, color_accent, color_banner, color_name };
struct ColorPreview {
    bool active, applied, accent_set, theme_set, theme_reset, name_style_set;
    u32 accent, theme[2];
    ProfileNameStyle name_style;
    u32 suggested[kImagePaletteColors];
} g_color_preview;

} // namespace
namespace profile_editor_detail {

bool color_valid(TextFormOwnerKey key) noexcept {
    return owner && owner->lifetime == key.scope && owner->scene.active && editable();
}

} // namespace profile_editor_detail
namespace {

void color_apply(u64 slot, u32 color) noexcept {
    auto& p = *owner;
    if (slot == color_banner) {
        p.draft.accent_set = true;
        p.draft.accent = color;
    } else if (slot >= color_name && slot < color_name + kProfileNameStyleColors) {
        p.draft.name_style_set = true;
        p.draft.name_style.colors[slot - color_name] = color;
    } else {
        u32 colors[2]{};
        if (effective_theme(colors) != 2)
            colors[0] = colors[1] = 0x41434A;
        colors[slot == color_primary ? 0 : 1] = color;
        p.draft.theme_set = true;
        p.draft.theme_reset = false;
        p.draft.theme[0] = colors[0];
        p.draft.theme[1] = colors[1];
    }
    repaint();
}
void color_preview(TextFormOwnerKey key, u32 rgb) noexcept {
    if (g_color_preview.active && color_valid(key))
        color_apply(key.collection, rgb);
}
bool color_save(TextFormOwnerKey key, u32 rgb) noexcept {
    if (!color_valid(key))
        return false;
    color_apply(key.collection, rgb);
    g_color_preview.applied = true;
    return true;
}
// The avatar's palette, once the preview card has its picture resident (0 colours before): a picked, unsaved avatar as
// it is cropped, as the preview's theme reads it, else the saved one.
u32 avatar_palette(u32* colors) noexcept {
    ProfileMedia still;
    const auto& card = owner->preview;
    if (!owner->preview_open)
        return 0;
    if (card.pending_avatar.active) {
        const auto out = image_crop_output(card.pending_avatar.crop);
        const u32 dpi = card.media_dpi ? card.media_dpi : 96u;
        return renderer_media_crop_palette(&g_app.renderer, card.pending_avatar.key,
                                           {out.x, out.y, out.width, out.height, card.pending_avatar.crop.image_width,
                                            (static_cast<u32>(kProfileAvatarThemeDips) * dpi + 95) / 96, out.rotation},
                                           colors);
    }
    return profile_avatar_still_media(card, kProfileAvatarThemeDips, &still) ||
                   profile_avatar_still_media(card, ui_rect_width(card.avatar), &still)
               ? renderer_media_palette(&g_app.renderer, still.key, colors)
               : 0;
}
void color_closed(TextFormOwnerKey key) noexcept {
    const auto preview = g_color_preview;
    reset_value(&g_color_preview);
    if (!preview.active || preview.applied || !color_valid(key))
        return;
    auto& d = owner->draft;
    d.accent_set = preview.accent_set;
    d.accent = preview.accent;
    d.theme_set = preview.theme_set;
    d.theme_reset = preview.theme_reset;
    d.theme[0] = preview.theme[0];
    d.theme[1] = preview.theme[1];
    d.name_style_set = preview.name_style_set;
    d.name_style = preview.name_style;
    repaint();
}
void edit_color(ColorSlot slot) noexcept {
    auto& p = *owner;
    const auto& d = p.draft;
    g_color_preview = {true,        false,        d.accent_set, d.theme_set, d.theme_reset, d.name_style_set,
                       d.accent,    {d.theme[0], d.theme[1]},   d.name_style, {}};
    const u32 suggested = avatar_palette(g_color_preview.suggested);
    u32 colors[2]{};
    const u32 fallback = suggested ? g_color_preview.suggested[0] : 0x41434A;
    const bool name = slot >= color_name;
    const u32 current = name ? d.name_style.colors[slot - color_name]
                        : slot == color_banner ? (d.accent_set ? d.accent : p.base.has_accent ? p.base.accent : fallback)
                        : effective_theme(colors) == 2 ? colors[slot == color_primary ? 0 : 1]
                                                       : fallback;
    ApplicationColorPickerRequest request{};
    request.title = slot == color_primary  ? utf8_literal("Primary Color")
                    : slot == color_accent ? utf8_literal("Accent Color")
                    : slot == color_banner ? utf8_literal("Banner Color")
                    : d.name_style.color_count > 2 ? utf8_literal("Name Palette Color")
                    : d.name_style.color_count > 1
                        ? (slot == color_name ? utf8_literal("Name Start Color") : utf8_literal("Name End Color"))
                        : utf8_literal("Name Color");
    request.label = request.title;
    request.owner = {p.lifetime, slot, 0};
    request.valid = color_valid;
    request.rgb = current;
    // Name colours suggest the official client's name swatches; profile colours the saved avatar's palette.
    request.suggested = name ? ConstSlice<u32>{kProfileNameColorSuggestions, sizeof(kProfileNameColorSuggestions) / sizeof(u32)}
                             : ConstSlice<u32>{g_color_preview.suggested, suggested};
    request.preview = color_preview;
    request.save = color_save;
    request.closed = color_closed;
    if (!application_color_picker_open(request))
        reset_value(&g_color_preview);
}

} // namespace
namespace profile_editor_detail {

bool guard() noexcept {
    // Unsaved changes hold the page, as the official client's settings do.
    if (owner && changes() && !saving()) {
        settings_layer_shake(&owner->layer);
        repaint();
        return false;
    }
    return true;
}

} // namespace profile_editor_detail
namespace {

// A dropdown row: enabled (the popup ignores clicks on rows without the flag), checked when it is the current value.
u16 dropdown_flags(bool current) noexcept {
    return static_cast<u16>(popup_item_enabled | (current ? popup_item_checked : 0));
}
// Opens a dropdown under its select field, left-aligned and as wide as the field, as the official client's select;
// the pointer's position plays no part.
void open_dropdown(const UiSettingsTarget& target, ConstSlice<PopupItem> items) noexcept {
    auto& p = *owner;
    UiSettingsControlGeometry geometry{};
    if (!ui_settings_scene_control_geometry(p.scene, target, &geometry))
        return;
    settings_layer_open_choice(&p.layer, kProfilesPage.owner_id, items, geometry.field,
                               maximum(96.f, ui_rect_width(geometry.field)));
}
// Try It Out's Shuffle, as the official client's: a random theme and a random name style to preview.
void shuffle() noexcept {
    auto& p = *owner;
    u64 state = clock_monotonic_milliseconds() * 0x9E3779B97F4A7C15ull ^ p.lifetime;
    const auto next = [&state](u32 bound) noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<u32>(state % bound);
    };
    const float hue = next(360) / 360.f;
    p.draft.theme_set = true;
    p.draft.theme_reset = false;
    p.draft.theme[0] = color_picker_rgb({hue, .45f + next(40) / 100.f, .55f + next(40) / 100.f});
    p.draft.theme[1] = color_picker_rgb({hue < .5f ? hue + .5f : hue - .5f, .45f + next(40) / 100.f, .3f + next(40) / 100.f});
    p.draft.name_style_set = true;
    p.draft.name_style = profile_name_style_with_effect({kProfileNameFonts[next(kProfileNameFontCount)].id, 0, 0, {}},
                                                        next(kProfileNameEffectCount));
}
// Leaving Try It Out returns every field the account can't save to its saved value; other edits stay.
void stop_trying() noexcept {
    auto& p = *owner;
    p.trying = false;
    const u16 gated = static_cast<u16>(~gate().enabled);
    auto& d = p.draft;
    if (gated & profile_editor_avatar) {
        release_picked(p.avatar);
        d.avatar = ProfileImageEdit::keep;
    }
    if (gated & profile_editor_banner) {
        release_picked(p.banner);
        d.banner = ProfileImageEdit::keep;
    }
    if (gated & profile_editor_theme)
        d.theme_set = d.theme_reset = false;
    if (gated & profile_editor_name_style) {
        d.name_style_set = false;
        d.name_style = p.base.name_style;
    }
    if (gated & profile_editor_effect) {
        d.effect_set = false;
        d.effect_sku = p.base.effect_sku;
    }
    const bool guild = p.tab == ProfileEditorTab::guild;
    if (gated & profile_editor_decoration) {
        d.decoration_set = false;
        d.decoration_sku = p.base.decoration_sku;
        assign(&p.decoration_asset, guild && p.saved.member_decoration_asset.bytes
                                        ? p.saved.member_decoration_asset.view()
                                        : p.saved.decoration_asset.view());
    }
    if (gated & profile_editor_nameplate) {
        d.nameplate_set = false;
        d.nameplate_sku = p.base.nameplate_sku;
        assign(&p.nameplate_asset, guild ? p.saved.member_nameplate_asset.view() : p.saved.nameplate_asset.view());
    }
    if (gated & profile_editor_bio)
        load_editor(editor_bio, p.base.bio);
}
void hand_off(const UiSettingsTarget& target) noexcept {
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
    reset_value(&p.errors);
    reset_value(&p.status);
    p.failed = false;
    queue_fetch();
}

} // namespace
namespace profile_editor_detail {

void select_guild(u32 index) noexcept {
    auto& p = *owner;
    if (index >= p.guild_choice_count || p.guild_ids[index] == p.guild_id || !guard())
        return;
    p.guild_id = p.guild_ids[index];
    const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
    p.guild_generation = guild ? guild->generation : 0;
    reset_value(&p.errors);
    reset_value(&p.status);
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

} // namespace profile_editor_detail
namespace {

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
        reset_value(&p.status);
        return;
    }
    reset_value(&p.errors);
    p.failed = false;
    reset_value(&p.status);
    p.saving = true;
    const bool account = p.tab == ProfileEditorTab::main ? p.user_patch.fields != 0 : p.member_patch.fields != 0;
    queue(account ? Op::save_account : Op::save_profile);
}

} // namespace
namespace profile_editor_detail {

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
                items[i] = {p.guild_choices[i], popup_guild + i + 1, dropdown_flags(p.guild_ids[i] == p.guild_id)};
            open_dropdown(target, {items, p.guild_choice_count});
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
            reset_value(&p.decoration_asset);
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
    case control_try:
        // Not trying: the notice's Try It Out. Trying: Shuffle, then Stop Trying.
        if (!p.trying)
            p.trying = true;
        else if (choice == 0)
            shuffle();
        else
            stop_trying();
        break;
    case control_name_style:
    case control_name_effect: {
        // A dropdown of the fonts or the effects, the current one checked.
        const bool fonts = id == control_name_style;
        const auto& style = p.draft.name_style;
        const u32 current = !profile_name_style_set(style) ? ~0u
                            : fonts                          ? profile_name_font_index(style.font)
                                                             : profile_name_effect_index(style.effect);
        PopupItem items[kProfileNameFontCount]{};
        const u32 count = fonts ? kProfileNameFontCount : kProfileNameEffectCount;
        for (u32 i = 0; i < count; ++i)
            items[i] = {fonts ? kProfileNameFonts[i].name : kProfileNameEffects[i].name,
                        (fonts ? popup_font : popup_effect) + i + 1, dropdown_flags(i == current)};
        open_dropdown(target, {items, count});
        return;
    }
    case control_name_colors:
        if (choice < p.draft.name_style.color_count)
            edit_color(static_cast<ColorSlot>(color_name + choice));
        else {
            // Remove Style (Use Default on a server profile).
            p.draft.name_style_set = true;
            reset_value(&p.draft.name_style);
        }
        break;
    case control_nameplate:
        if (choice == 0)
            open_picker_dialog(Dialog::nameplate);
        else {
            p.draft.nameplate_set = true;
            p.draft.nameplate_sku = 0;
            reset_value(&p.nameplate_asset);
        }
        break;
    default:
        return;
    }
    repaint();
}

// A choice from one of the page's dropdowns (its action: the popup's base + the index + 1).
void choose_option(u32 action) noexcept {
    auto& p = *owner;
    if (action > popup_effect) {
        if (!editable())
            return;
        p.draft.name_style_set = true;
        p.draft.name_style = profile_name_style_with_effect(p.draft.name_style, action - popup_effect - 1);
    } else if (action > popup_font) {
        if (!editable() || action - popup_font - 1 >= kProfileNameFontCount)
            return;
        auto& style = p.draft.name_style;
        if (!profile_name_style_set(style))
            style = profile_name_style_with_effect(style, 0);
        style.font = kProfileNameFonts[action - popup_font - 1].id;
        p.draft.name_style_set = true;
    } else if (action)
        select_guild(action - 1);
}

} // namespace profile_editor_detail
} // namespace maicord::app_detail

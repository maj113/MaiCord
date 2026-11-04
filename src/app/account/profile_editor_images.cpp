#include "profile_editor_internal.hpp"
#include "../media/file_over_limit.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "profile_editor.hpp"
#include "application_account_logout.hpp"
#include "../settings/settings.hpp"
#include "account_ui_runtime.hpp"

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
            *error = application_file_over_limit_text(path, static_cast<u64>(size.QuadPart), kProfileImageSourceMaximumBytes,
                                                      {p.size_error, sizeof(p.size_error)});
        release_picked(staged);
        return false;
    }
    auto* data = static_cast<u8*>(
        commit_pages(static_cast<SIZE_T>(size.QuadPart)));
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

// Colours (Primary / Accent / Banner Color): a hex value, like the picker's input.
// Valid text recolours the preview live; closing without Apply restores the
// colours the draft had when the form opened.
enum ColorSlot : u64 { color_primary = 1, color_accent, color_banner };
struct ColorPreview {
    bool active, applied, accent_set, theme_set, theme_reset;
    u32 accent, theme[2];
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
bool color_check(TextFormOwnerKey key, Utf8View text, u32* rgba) noexcept {
    if (!application_text_form_color_check(key, text, rgba))
        return false;
    if (g_color_preview.active && color_valid(key))
        color_apply(key.collection, *rgba >> 8);
    return true;
}
bool color_save(TextFormOwnerKey key, Utf8View text) noexcept {
    u32 color = 0;
    if (!color_valid(key) || !color_hex_parse_rgb24(text, &color))
        return false;
    color_apply(key.collection, color);
    g_color_preview.applied = true;
    application_text_form_close(false, false);
    return true;
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
    repaint();
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
    request.closed = color_closed;
    request.maximum_codepoints = 7;
    request.single_line = true;
    request.placeholder = utf8_literal("#RRGGBB");
    request.check = color_check;
    request.rejected = utf8_literal("That isn't a hex colour. Use #RGB or #RRGGBB.");
    request.swatch = true;
    const auto& d = p.draft;
    g_color_preview = {true, false, d.accent_set, d.theme_set, d.theme_reset, d.accent, {d.theme[0], d.theme[1]}};
    if (!application_text_form_open(request))
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
    default:
        return;
    }
    repaint();
}

} // namespace profile_editor_detail
} // namespace maicord::app_detail

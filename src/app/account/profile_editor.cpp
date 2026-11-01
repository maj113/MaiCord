#include "profile_editor_internal.hpp"

namespace maicord::app_detail {
namespace {

void choose(SettingsLayer*, u32 action) noexcept {
    if (owner && action)
        select_guild(action - 1);
    repaint();
}
bool edited(SettingsLayer*, u64 control) noexcept {
    if (!owner || editor_of(control) == editor_count)
        return false;
    owner->failed = false;
    reset_value(&owner->status);
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
constexpr SettingsLayerHooks kHooks{editor_hook, edited, activate, choose, close_hook,
                                    settings_hook<repaint>, nullptr, settings_hook<editable>};

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = settings_modal_allocate<Owner>(kHooks);
    if (!owner)
        return false;
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

Utf8View failure(const RestClientResponse& response, const Utf8View& body) noexcept {
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
void load_profile(const Utf8View& body) noexcept {
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

bool application_profile_editor_open(ProfileEditorTab tab, u64 guild_id) noexcept {
    if (!application_account_available() || !g_app.renderer_ready || g_app.text_form || !g_app.account.api.profile_edit ||
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
    settings_modal_yield();
    auto& p = *owner;
    p.lifetime = ++lifetime_serial;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.self_user_id = s.self_user_id;
    p.tab = tab;
    p.queued = p.failed = p.saving = false;
    p.op = Op::none;
    reset_value(&p.status);
    reset_value(&p.errors);
    p.loaded = p.load_failed = false;
    p.dialog_kind = Dialog::none;
    p.collectibles_loaded = p.collectibles_failed = false;
    p.owned_count = 0;
    reset_value(&p.effect);
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
    reset_value(&p.draft);
    settings_layer_reset(&p.layer);
    if (!register_pages())
        return false;
    settings_modal_take_input();
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
    settings_modal_suspend(p);
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
    settings_modal_leave(p, restore_focus && application_account_available());
}

void application_profile_editor_release() noexcept {
    application_profile_editor_close(false);
    if (!owner)
        return;
    application_cancel_request(&AccountModuleApi::profile_edit, owner->request_id);
    if (owner->picker.runtime) {
        native_file_picker_cancel(&owner->picker);
        native_file_picker_release(&owner->picker, 5'000);
    }
    release_picked(owner->avatar);
    release_picked(owner->banner);
    release_picked(owner->staged);
    for (auto& model : owner->models)
        message_composer_release(&model);
    settings_modal_free(owner);
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
    if (changed)
        return refresh_dialog();
    if (!project())
        return false;
    if (p.dialog_kind != Dialog::none)
        dialog_update();
    return settings_modal_sync_text(p);
}

namespace {

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
    refresh_dialog();
}
bool dialog_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto& p = *owner;
    auto& d = p.dialog;
    if (message == WM_MOUSEWHEEL) {
        const float delta = -application_wheel_notches(wparam) * 48.f;
        if (ui_profile_dialog_scroll(&d, delta))
            repaint();
        return true;
    }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        UiPoint point{};
        application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point);
        const auto hit = ui_profile_dialog_hit_test(d, point.x, point.y);
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
    const float x = client_dip_x(lparam), y = client_dip_y(lparam);
    if (p.crop_dragging || p.zoom_dragging) {
        if (message == WM_MOUSEMOVE) {
            if (p.crop_dragging) {
                p.crop.offset_x = p.drag_offset_x + (x - p.drag_x);
                p.crop.offset_y = p.drag_offset_y + (y - p.drag_y);
                image_crop_clamp(&p.crop);
            } else
                set_zoom(x);
            refresh_dialog();
        } else if (message == WM_LBUTTONUP) {
            p.crop_dragging = p.zoom_dragging = false;
            application_release_capture();
            refresh_dialog();
        }
        return true;
    }
    const auto hit = ui_profile_dialog_hit_test(d, x, y);
    const auto action = ui_profile_dialog_action(d, hit);
    if (message == WM_LBUTTONDOWN && !ui_rect_contains(d.rectangle, x, y)) {
        // A click on the scrim closes the dialog like Cancel.
        close_dialog();
        repaint();
        return true;
    }
    if (message == WM_LBUTTONDOWN && (action == UiProfileDialogAction::pan || action == UiProfileDialogAction::zoom)) {
        application_capture_pointer();
        if (action == UiProfileDialogAction::pan) {
            p.crop_dragging = true;
            p.drag_x = x;
            p.drag_y = y;
            p.drag_offset_x = p.crop.offset_x;
            p.drag_offset_y = p.crop.offset_y;
        } else {
            p.zoom_dragging = true;
            set_zoom(x);
            refresh_dialog();
        }
        return true;
    }
    const auto result = ui_button_pointer_target(&d.widgets,
                                                 message == WM_LBUTTONDOWN ? UiButtonPointerEvent::press
                                                 : message == WM_LBUTTONUP ? UiButtonPointerEvent::release
                                                                           : UiButtonPointerEvent::move,
                                                 hit);
    if (message == WM_LBUTTONDOWN && result.handled)
        application_capture_pointer();
    if (message == WM_LBUTTONUP)
        application_release_capture();
    if (result.changed) {
        ++d.revision;
        repaint();
    }
    if (result.activate) {
        u32 activated = 0;
        const auto act = ui_profile_dialog_action(d, result.target, &activated);
        dialog_action(act, activated);
    }
    return true;
}
bool dialog_key(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto& p = *owner;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return true;
    const bool shift = application_shift_down();
    if (key == VK_ESCAPE && !repeated) {
        close_dialog();
        repaint();
        return true;
    }
    if (key == VK_RETURN && !repeated) {
        apply_dialog();
        refresh_dialog();
        return true;
    }
    if (p.dialog_kind == Dialog::crop && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
        // As in the official client: 4 DIP per press, 40 with Shift.
        const float step = shift ? kImageCropFastKeyStep : kImageCropKeyStep;
        image_crop_pan(&p.crop, key == VK_LEFT ? -step : key == VK_RIGHT ? step : 0,
                       key == VK_UP ? -step : key == VK_DOWN ? step : 0);
        refresh_dialog();
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
    const u64 id = application_next_request_id();
    const auto submitted = g_app.account.api.profile_edit(g_app.account.instance, &request, id);
    if (submit_busy(submitted, p.retry_at, now, 50))
        return true;
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
        refresh_dialog();
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
    const bool ok = response.succeeded();
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
    return refresh_dialog();
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

// The layer text editing goes to; none while a dialog is over the editor.
SettingsLayer* application_profile_editor_edit_layer() noexcept {
    return application_profile_editor_active() && owner->dialog_kind == Dialog::none ? &owner->layer : nullptr;
}

bool application_profile_editor_frame_current(MediaAssetKey key) noexcept {
    const auto* picked = picked_for(key);
    return picked && !picked->failed;
}

void application_profile_editor_frame_uploaded(MediaAssetKey key) noexcept {
    if (auto* picked = picked_for(key)) {
        picked->ready = true;
        picked->poll_at = 0;
        refresh_dialog();
    }
}

void application_profile_editor_frame_rejected(MediaAssetKey key) noexcept {
    if (auto* picked = picked_for(key)) {
        picked->ready = false;
        picked->failed = true;
        refresh_dialog();
    }
}

ProfileScene* application_profile_editor_preview() noexcept {
    return application_profile_editor_active() && owner->preview_open ? &owner->preview : nullptr;
}

const UiProfileDialogScene* application_profile_editor_dialog() noexcept {
    return application_profile_editor_active() && owner->dialog.active ? &owner->dialog : nullptr;
}

} // namespace maicord::app_detail

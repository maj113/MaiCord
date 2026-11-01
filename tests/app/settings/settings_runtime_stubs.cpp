#include "settings_runtime_internal.hpp"
#include "shell/state.hpp"
#include "settings/settings.hpp"
#include "account/profile.hpp"
#include "account/profile_editor.hpp"
#include "shell/content_input.hpp"
#include "voice/voice.hpp"
#include "settings_runtime_keys.hpp"
#include "../../support/test_output.hpp"

namespace maicord {

// Deterministic picker boundary: actual plugin controller code is exercised,
// while every picker has owned VM and a real completion handle but no dialog,
// worker thread, Shell work, or account interaction.
NativeFilePickerError native_file_picker_start(NativeFilePicker* picker, HWND owner, u32 maximum_files,
                                               Utf8View) noexcept {
    using namespace settings_runtime_test;
    if (!picker || picker->runtime || owner || maximum_files != 1 || !picker_boundary.next_path.data)
        return NativeFilePickerError::invalid_input;
    auto* runtime = static_cast<PickerRuntime*>(
        VirtualAlloc(nullptr, sizeof(PickerRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime)
        return NativeFilePickerError::memory;
    const HANDLE notification = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!notification) {
        VirtualFree(runtime, 0, MEM_RELEASE);
        return NativeFilePickerError::memory;
    }
    runtime->path = picker_boundary.next_path;
    runtime->result = {1, 0, NativeFilePickerError::none, NativeFilePickerPhase::complete};
    picker->runtime = runtime;
    picker->notification = notification;
    picker->phase = static_cast<LONG>(NativeFilePickerPhase::complete);
    ++picker_boundary.starts;
    return NativeFilePickerError::none;
}
NativeFilePickerResult native_file_picker_status(const NativeFilePicker* picker) noexcept {
    using namespace settings_runtime_test;
    return picker && picker->runtime
               ? static_cast<const PickerRuntime*>(picker->runtime)->result
               : NativeFilePickerResult{0, 0, NativeFilePickerError::none, NativeFilePickerPhase::empty};
}
ConstSlice<wchar_t> native_file_picker_path(const NativeFilePicker* picker, u32 index) noexcept {
    using namespace settings_runtime_test;
    if (!picker || !picker->runtime || index)
        return {};
    ++picker_boundary.path_reads;
    return static_cast<const PickerRuntime*>(picker->runtime)->path;
}
void native_file_picker_cancel(NativeFilePicker* picker) noexcept {
    using namespace settings_runtime_test;
    if (!picker || !picker->runtime)
        return;
    ++picker_boundary.cancellations;
    // Completion was published before cancellation. Like the real worker,
    // retain that immutable success so the controller must abandon it itself.
}
bool native_file_picker_release(NativeFilePicker* picker, u32) noexcept {
    using namespace settings_runtime_test;
    if (!picker)
        return false;
    ++picker_boundary.releases;
    if (!picker->runtime)
        return true;
    if (picker_boundary.release_timeouts) {
        --picker_boundary.release_timeouts;
        SetLastError(WAIT_TIMEOUT);
        return false;
    }
    CloseHandle(picker->notification);
    VirtualFree(picker->runtime, 0, MEM_RELEASE);
    *picker = {};
    return true;
}

// Rendering and surrounding account interactions are deliberate test seams.
// The controller, state, scene, storage, preference writer and DLL host remain
// production implementations.
void renderer_bind_theme_editor_scene(D3D11Renderer* renderer, const UiThemeEditorScene* current) noexcept {
    renderer->theme_editor_scene = current;
    renderer->retained_presentation.valid = false;
}
// Plugin background effects need a device; settings only list and save them.
HRESULT renderer_select_plugin_effect(D3D11Renderer*, u64, u64, u64, ConstSlice<u8>, const float*) noexcept {
    return E_NOTIMPL;
}
void renderer_clear_plugin_effect(D3D11Renderer*) noexcept {
}
// The sign-in backdrop is a GPU image; settings only freeze and release it.
bool renderer_freeze_login_backdrop(D3D11Renderer* renderer, const Theme&) noexcept {
    renderer->settings_backdrop_frozen = renderer->login_visible;
    return renderer->login_visible;
}
void renderer_release_settings_backdrop(D3D11Renderer* renderer) noexcept {
    renderer->settings_backdrop_frozen = false;
}
void renderer_bind_settings_scene(D3D11Renderer* renderer, const UiSettingsScene* current) noexcept {
    using namespace settings_runtime_test;
    if (!current && renderer->settings_scene && observed.detaching_module && observed.audit) {
        ++observed.audit->host_detaches;
        observed.detached_active_scene = renderer->settings_scene->active;
        observed.detached_live_code =
            observed.detaching_module->module && GetModuleHandleW(L"MaiCordPluginFixture.dll");
        // The plugin's menu items are still registered while its UI detaches.
        const auto* host = application_settings_plugin_host_current();
        NativePluginSdkMenuItem items[4]{};
        observed.detached_live_menu_labels =
            host && host->sdk && native_plugin_sdk_menu_items(*host->sdk, MC_MENU_USER, {items, 4}) == 1 &&
            items[0].owner_id == fixture_owner &&
            utf8_equal(items[0].label, utf8_literal("Open native fixture for friend"));
        MEMORY_BASIC_INFORMATION region{};
        observed.detached_live_state =
            observed.detaching_module->state &&
            VirtualQuery(observed.detaching_module->state, &region, sizeof(region)) == sizeof(region) &&
            region.State == MEM_COMMIT;
    }
    renderer->settings_scene = current;
    renderer->interaction_text_settings_revision = ~(current ? current->revision : 0u);
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}
bool renderer_set_settings_modal_progress(D3D11Renderer* renderer, float progress) noexcept {
    renderer->settings_modal_progress = progress;
    return true;
}
// No text is measured here; pointer placement keeps the keyboard caret.
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32*) noexcept {
    return false;
}
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme&) noexcept {
    using namespace settings_runtime_test;
    ++observed.text_builds;
    if (observed.fail_text_remaining) {
        --observed.fail_text_remaining;
        return false;
    }
    if (observed.fail_text)
        return false;
    renderer->interaction_text_settings_revision =
        renderer->settings_scene ? renderer->settings_scene->revision : 0;
    const auto& popup = app_detail::g_app.popup_scene;
    renderer->interaction_text_popup_revision =
        renderer->settings_scene && renderer->settings_scene->active
            ? (popup.active && popup.kind == PopupKind::settings_choice ? popup.revision : 0)
            : popup.revision;
    return true;
}
MessageTextHit renderer_content_hit_test(const D3D11Renderer&, i32, i32) noexcept {
    ++settings_runtime_test::observed.content_hits;
    return {};
}
RendererMessageTextPositionHit renderer_message_text_position_hit_test(D3D11Renderer&, i32, i32) noexcept {
    ++settings_runtime_test::observed.text_hits;
    return {};
}
// Settings own the surface: the message view's emoji are never consulted.
MessageEmojiHit renderer_message_emoji_hit_test(D3D11Renderer&, float, float) noexcept {
    return {};
}
void renderer_set_live_resize(D3D11Renderer* renderer, bool live_resize) noexcept {
    renderer->live_resize = live_resize;
}
bool renderer_finish_resize(D3D11Renderer* renderer, u32 width, u32 height) noexcept {
    auto& observed = settings_runtime_test::observed;
    ++observed.resize_finishes;
    if (observed.resize_finish_failed)
        return false;
    renderer->width = width;
    renderer->height = height;
    renderer->live_resize = false;
    return true;
}
namespace app_detail {
// The Profiles page hands the modal to the profile editor, which is not
// linked here.
bool application_profile_editor_open(ProfileEditorTab, u64) noexcept {
    return false;
}
// This existing fixture exercises legacy built-in preferences and background
// editing. The separate included-owner fixture below covers custom snapshots
// and typed picker/IO receipts without loading a filesystem worker here.
ApplicationCustomThemeView application_custom_theme_view() noexcept {
    return settings_runtime_test::custom_theme_projection;
}
const ClientThemeDocument* application_custom_theme_document() noexcept {
    return nullptr;
}
bool application_custom_theme_preferences_changed(bool, bool) noexcept {
    return true;
}
bool application_custom_theme_snapshot(ClientThemeStorageSnapshot*) noexcept {
    return false;
}
bool application_custom_theme_restore(const ClientThemeStorageSnapshot&) noexcept {
    return true;
}
bool application_custom_theme_synchronize() noexcept {
    return true;
}
void application_custom_theme_cancel_interaction() noexcept {
}
ConstSlice<Utf8View> application_custom_theme_catalog() noexcept {
    return {};
}
bool application_custom_theme_select_installed(u32) noexcept {
    return false;
}
bool application_custom_theme_action(ApplicationCustomThemeAction action) noexcept {
    settings_runtime_test::custom_theme_last_action = action;
    ++settings_runtime_test::custom_theme_actions;
    if (settings_runtime_test::custom_theme_pointer_transition &&
        action == ApplicationCustomThemeAction::preview) {
        settings_runtime_test::custom_theme_projection.previewing = true;
        ++settings_runtime_test::custom_theme_projection.revision;
    }
    return true;
}
// The isolated Settings host borrows the real shared author geometry instead
// of linking the entire content activation controller. Count the boundary so
// the opaque Settings cursor shield fails if it consults underlying profiles.
// Server tag chips share the author line and the same shield.
bool application_message_guild_tag_hit(float, float) noexcept {
    ++settings_runtime_test::observed.profile_hits;
    return false;
}
bool application_message_command_chip_hit(float, float) noexcept {
    ++settings_runtime_test::observed.profile_hits;
    return false;
}
MessageProfileHit application_message_profile_hit_test(float x, float y) noexcept {
    ++settings_runtime_test::observed.profile_hits;
    const auto& renderer = g_app.renderer;
    if (g_app.login.visible || !renderer.message_text || !renderer.chat_state || !renderer.user_store)
        return {};
    return message_author_hit_test(
        g_app.message_scene, *renderer.message_text, *renderer.chat_state, *renderer.user_store,
        renderer.member_list_scene ? renderer.member_list_scene->measure : nullptr, application_theme(), x,
        y);
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
void application_plugins_actions_install(NativePluginSdkRuntime& runtime) noexcept { runtime.actions = {}; }
// Account settings open the service's web pages through the link flow.
namespace {
u32 settings_link_opens{};
Utf8View settings_link_url{};
bool settings_link_trusted{};

} // namespace

bool application_link_open(Utf8View url, bool trusted) noexcept {
    ++settings_link_opens;
    settings_link_url = url;
    settings_link_trusted = trusted;
    return true;
}
u32 settings_runtime_link_opens(Utf8View* url, bool* trusted) noexcept {
    if (url) *url = settings_link_url;
    if (trusted) *trusted = settings_link_trusted;
    return settings_link_opens;
}
void application_tooltip_clear() noexcept {
}
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    auto& form = settings_runtime_test::gradient_form;
    if (form.active || !request.valid || !request.save || !request.valid(request.owner) ||
        request.initial.bytes > sizeof(form.initial))
        return false;
    form.request = request;
    copy_bytes(form.initial, request.initial.data, request.initial.bytes);
    form.request.initial = {form.initial, request.initial.bytes};
    form.active = true;
    ++form.opens;
    return true;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    auto& form = settings_runtime_test::gradient_form;
    if (form.active && form.request.valid == validator)
        form.active = false;
}
void application_text_form_close(bool, bool) noexcept {
    settings_runtime_test::gradient_form.active = false;
}
const ApplicationTextFormRequest* settings_runtime_text_form() noexcept {
    const auto& form = settings_runtime_test::gradient_form;
    return form.active ? &form.request : nullptr;
}
bool application_present_frame() noexcept {
    ++settings_runtime_test::observed.resize_sync_presents;
    return true;
}
void application_record_presentation_failure(const char*) noexcept {
    ++settings_runtime_test::observed.resize_finish_failures;
}
// This target exercises ordinary/plugin Settings with Overview absent. The
// separate Overview runtime target owns its production controller and page.
// Settings before sign-in borrows the account UI runtime; these tests open
// it signed in.
bool application_initialize_account_ui(Application&) noexcept {
    return false;
}
void application_release_account_ui(Application&) noexcept {}
bool application_login_view_suspend(bool) noexcept {
    return true;
}
bool application_channel_overview_active() noexcept {
    return false;
}
bool application_account_logout_available() noexcept {
    return false;
}
bool application_account_logout_open() noexcept {
    return false;
}
void application_channel_overview_close(bool) noexcept {
}
void application_channel_overview_release() noexcept {
}
bool application_channel_overview_synchronize() noexcept {
    return true;
}
bool application_channel_overview_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_channel_overview_window_message(UINT, WPARAM, LPARAM) noexcept {
    return false;
}
void application_notifications_settings_refresh(Application*) noexcept {
    ++settings_runtime_test::observed.notifications_refreshes;
}
bool application_notifications_setup(Application*) noexcept {
    ++settings_runtime_test::observed.notifications_setups;
    return true;
}
Utf8View application_notifications_status(const Application* app) noexcept {
    return !app->preferences.desktop_enabled ? utf8_literal("Desktop notifications are disabled.")
           : settings_runtime_test::observed.notifications_setups
               ? utf8_literal("Notification setup requested.")
               : utf8_literal("Set up desktop notifications.");
}
bool application_friends_active() noexcept {
    return settings_runtime_test::observed.friends_active;
}
bool application_friends_user_relation(u64 user_id, u8* relation) noexcept {
    const auto& row = settings_runtime_test::menu_fixture.row;
    if (!relation || user_id != row.user_id)
        return false;
    *relation = static_cast<u8>(row.kind);
    return true;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
}
UiThreadBrowserScene* application_thread_browser_scene() noexcept {
    return nullptr;
}
bool application_thread_browser_toggle() noexcept {
    return false;
}
bool application_friends_requests_active() noexcept {
    return settings_runtime_test::observed.requests_active;
}
void application_friends_close_requests() noexcept {
    auto& observed = settings_runtime_test::observed;
    if (!observed.requests_active)
        return;
    observed.requests_active = false;
    g_app.renderer.friends_requests_scene = nullptr;
    ui_friends_scene_release(&settings_runtime_test::menu_fixture.requests);
    g_app.context_focus = &settings_runtime_test::menu_fixture.friends.widgets;
}
bool application_friends_show() noexcept {
    return false;
}
bool application_private_channel_picker(u64) noexcept {
    return false;
}
bool application_private_channel_call(u64) noexcept {
    return false;
}
bool application_private_channel_rename(u64) noexcept {
    return false;
}
bool application_private_channel_title_hit(float, float) noexcept {
    return false;
}
UiRect application_viewport_dips() noexcept {
    return settings_runtime_test::observed.viewport;
}
bool synchronize_shell_layout() noexcept {
    ++settings_runtime_test::observed.layout_updates;
    return true;
}
bool synchronize_member_subscription(u64) noexcept {
    ++settings_runtime_test::observed.member_updates;
    return true;
}
bool refresh_account_text(bool force) noexcept {
    ++settings_runtime_test::observed.account_text_updates;
    if (force)
        ++settings_runtime_test::observed.forced_account_text_updates;
    return true;
}
void application_composer_text_input_suspend() noexcept {
    ++settings_runtime_test::observed.suspends;
}
bool focus_message_composer_control(MessageComposerControl control) noexcept {
    g_app.message_composer.focused = control == MessageComposerControl::edit;
    return true;
}
void reset_message_composer_caret_timing(bool) noexcept {
}
// Popover glue (app/plugins/surfaces.cpp is linked for real).
bool refresh_popup_text() noexcept {
    return true;
}
void set_popup_owner(PopupOwnerKind owner) noexcept {
    g_app.popup_owner_kind = owner;
}
bool application_open_content_url(Utf8View url) noexcept {
    ++settings_runtime_test::observed.opened_urls;
    return url.bytes != 0;
}
bool application_message_action_bar_modifiers_changed() noexcept {
    return true;
}
bool dismiss_popup() noexcept {
    using namespace settings_runtime_test;
    if (g_app.popup_scene.active) {
        if (observed.detaching_module && observed.audit) {
            ++observed.audit->host_detaches;
            observed.detached_active_popup = true;
            observed.detached_popup_labels = g_app.popup_scene.item_count >= 2 &&
                                             utf8_equal(g_app.popup_scene.items[1].label,
                                                        utf8_literal("Open native fixture for friend"));
            observed.detached_live_code =
                observed.detaching_module->module && GetModuleHandleW(L"MaiCordPluginFixture.dll");
            MEMORY_BASIC_INFORMATION region{};
            observed.detached_live_state =
                observed.detaching_module->state &&
                VirtualQuery(observed.detaching_module->state, &region, sizeof(region)) == sizeof(region) &&
                region.State == MEM_COMMIT;
        }
        popup_scene_close(&g_app.popup_scene);
    }
    application_plugins_menu_dismissed();
    g_app.popup_owner_kind = PopupOwnerKind::none;
    return true;
}
void application_image_viewer_close() noexcept {
}
bool application_profile_close() noexcept {
    return true;
}
u16 application_message_action_bar_capabilities(u64) noexcept {
    return 0;
}
bool application_profile_open(u64, const UiRect&, ProfileAnchor) noexcept {
    ++settings_runtime_test::observed.profile_opens;
    return true;
}
bool application_account_menu_open(UiWidgetHandle source, UiRect anchor) noexcept {
    auto& seen = settings_runtime_test::observed;
    ++seen.account_menu_opens;
    seen.account_menu_source = source;
    seen.account_menu_anchor = anchor;
    return true;
}
// Voice: the settings glue is real; the client voice module is not linked here.
VoicePreferences& application_voice_preferences() noexcept {
    static VoicePreferences preferences = voice_preferences_defaults();
    return preferences;
}
void application_voice_settings_changed(bool) noexcept {
}
void application_voice_set_monitor(bool) noexcept {
}
VoiceAudioStatus application_voice_status() noexcept {
    return {};
}
bool application_voice_shell_click(float, float) noexcept {
    return false;
}
bool application_voice_shell_hover(float, float) noexcept {
    return false;
}
void application_channel_prefetch_hover(u64) noexcept {}
bool application_voice_hand_cursor(float, float) noexcept {
    return false;
}
bool application_message_content_cursor(float, float, UiCursor*) noexcept {
    return false;
}
bool application_video_viewer_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
void end_scrollbar_drag() noexcept {
}
void application_sync_attachment_drop_target() noexcept {
    ++settings_runtime_test::observed.drop_updates;
}

} // namespace app_detail
} // namespace maicord

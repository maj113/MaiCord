#include "settings_runtime_internal.hpp"
#include "shell/state.hpp"
#include "settings/settings.hpp"
#include "account/profile.hpp"
#include "account/profile_editor.hpp"
#include "shell/content_input.hpp"
#include "voice/voice.hpp"
#include "settings_runtime_keys.hpp"
#include "../../support/test_output.hpp"

#include "../../../src/app/store/store_host.hpp"

namespace maicord {

// Rendering and surrounding account interactions are deliberate test seams.
// The controller, state, scene, storage, preference writer and plugin menus remain
// production implementations.
void renderer_bind_theme_editor_scene(D3D11Renderer* renderer, const UiThemeEditorScene* current) noexcept {
    renderer->theme_editor_scene = current;
    renderer->retained_presentation.valid = false;
}
// Package background effects need a device: the owner keeps only what the host reads back (the effect selected).
EffectSelectResult renderer_select_effect(D3D11Renderer* renderer, const BackgroundEffectSpec& spec, const EffectSettings&) noexcept {
    const bool changed = !renderer->effect.selected || renderer->effect.identity != spec.identity;
    renderer->effect.selected = true;
    renderer->effect.identity = spec.identity;
    return {S_OK, changed ? EffectSelect::changed : EffectSelect::unchanged};
}
void renderer_clear_effect(D3D11Renderer* renderer) noexcept {
    renderer->effect.selected = false;
    renderer->effect.identity = 0;
}
void d3d11_effect_set_marker(D3D11EffectOwner*, volatile u64*) noexcept {
}
void d3d11_effect_set_pointer(D3D11EffectOwner*, float, float) noexcept {
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
// The magnifier follows the stored preferences; this fixture has no lens.
void application_image_zoom_configure() noexcept {}
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
    ++settings_runtime_test::theme_preference_changes;
    return true;
}
bool application_custom_theme_edit_background(const BackgroundTheme&) noexcept {
    ++settings_runtime_test::theme_background_edits;
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
ConstSlice<ClientThemeCatalogEntry> application_custom_theme_installed() noexcept {
    return settings_runtime_test::installed_themes;
}
u32 application_custom_theme_installed_active() noexcept {
    return settings_runtime_test::installed_selection.active;
}
bool application_custom_theme_apply_installed(u32 index, bool preview) noexcept {
    using namespace settings_runtime_test;
    if (index >= installed_themes.count)
        return false;
    ++installed_selection.calls;
    installed_selection.last = index;
    installed_selection.preview = preview;
    if (!preview) {
        installed_selection.active = index;
        custom_theme_projection.current = {installed_themes[index].name, installed_themes[index].name_bytes};
        custom_theme_projection.custom_active = true;
    }
    ++custom_theme_projection.revision;
    return true;
}
bool application_custom_theme_remove_installed(u32 index) noexcept {
    return index < settings_runtime_test::installed_themes.count;
}
bool application_custom_theme_use_builtin() noexcept {
    using namespace settings_runtime_test;
    ++installed_selection.builtin;
    installed_selection.active = ~0u;
    custom_theme_projection.custom_active = false;
    ++custom_theme_projection.revision;
    return true;
}
void application_custom_theme_rescan() noexcept {
}
bool application_custom_theme_action(ApplicationCustomThemeAction action) noexcept {
    settings_runtime_test::custom_theme_last_action = action;
    ++settings_runtime_test::custom_theme_actions;
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
const RelationshipStore* application_friends_relationships() noexcept {
    return nullptr;
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
bool synchronize_navigation_view() noexcept {
    ++settings_runtime_test::observed.navigation_updates;
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
// Popup glue the plugin menus and surfaces (app/plugins/menus.cpp, surfaces.cpp) are linked against.
bool refresh_popup_text() noexcept {
    return true;
}
void set_popup_owner(PopupOwnerKind owner) noexcept {
    g_app.popup_owner_kind = owner;
}
bool application_open_content_url(const Utf8View& url) noexcept {
    ++settings_runtime_test::observed.opened_urls;
    return url.bytes != 0;
}
bool application_message_action_bar_modifiers_changed() noexcept {
    return true;
}
bool dismiss_popup() noexcept {
    using namespace settings_runtime_test;
    if (g_app.popup_scene.active)
        popup_scene_close(&g_app.popup_scene);
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

// The signed-out settings measure through the renderer's text backend; the runtime test has none, so every
// measurement fails and layout keeps its per-character estimates.
bool renderer_text_measure_backend(void*, const UiTextMeasureRequest&, UiTextMeasureValue*) noexcept {
    return false;
}
u32 renderer_typography_revision(const D3D11Renderer&) noexcept {
    return 0;
}
} // namespace maicord

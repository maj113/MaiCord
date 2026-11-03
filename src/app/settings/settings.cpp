#include "settings_internal.hpp"
#include "../guild/channel_overview.hpp"
#include "../account/login/ui.hpp"
#include "../account/account_presentation.hpp"
#include "../account/application_account_module.hpp"
#include "../account/profile.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/messages/interaction.hpp"
#include "../shell/scrolling.hpp"
#include "../shell/overlay_input.hpp"
#include "maicord/ui/layout/splitter.hpp"
#include "../media/image_zoom.hpp"
#include "../chat/composer/attachments.hpp"
#include "../account/friends.hpp"
#include "../notifications/notifications.hpp"
#include "theme_editor.hpp"
#include "../store/store_host.hpp"
#include "../chat/composer/text_edit_keys.hpp"
#include "user_settings.hpp"
#include "account_settings.hpp"
#include "maicord/app/account/login/oauth_account.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/ui/text/clipboard.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {
namespace settings_detail {

bool choice_action(u32 action, const ApplicationSettingsChoice* captured) noexcept {
    const auto choice = captured ? *captured : g_app.settings ? g_app.settings->choice : ApplicationSettingsChoice{};
    const bool current = captured ? choice_control(choice) && !g_app.popup_scene.active &&
                                        g_app.popup_owner_kind == PopupOwnerKind::none &&
                                        g_app.popup_scene.opens == choice.popup_opens &&
                                        g_app.popup_scene.widgets.generation == choice.popup_generation
                                  : choice_popup_current(choice);
    if (!current) {
        // A replacement popup belongs to the nested caller; leave it alone.
        if (!captured)
            close_choice();
        return refresh();
    }
    if (action == kUiSettingsChoicePrevious || action == kUiSettingsChoiceNext) {
        auto next = choice;
        return ui_settings_choice_page(choice.choice_count, choice.first, action == kUiSettingsChoicePrevious,
                                       &next.first) &&
               project_choice(next);
    }
    u32 selected = 0;
    const auto* control = choice_control(choice);
    if (!control || !ui_settings_choice_selected(*control, choice.first, action, &selected))
        return false;
    close_choice();
    if (!choice_control(choice))
        return refresh();
    const bool result =
        activate({UiSettingsTargetKind::control, choice.token.page, choice.control_id}, false, false, selected);
    if (!plugin_ui_scope_current(g_app.plugin_account_scope_serial, choice.account_serial) ||
        g_app.account.status.profile_id != choice.profile_id)
        return false;
    return refresh() && result;
}

// The search field's model, with its undo history enabled on first use.
static MessageComposer& search_model(ApplicationSettings& settings) noexcept {
    auto& model = settings.search_edit;
    if (!model.history)
        message_composer_enable_history(&model, 2048);
    return model;
}
// Focus moves to the field before the edit is published, so the stale scene
// (the results page has not been laid out yet) cannot drop it.
static bool search_changed(ApplicationSettings& settings) noexcept {
    settings.scene.focus = {UiSettingsTargetKind::search, {}, 0};
    apply_search(settings);
    const bool refreshed = refresh();
    ui_settings_scene_focus(&settings.scene, {UiSettingsTargetKind::search, {}, 0});
    return refreshed;
}
bool search_character(ApplicationSettings& settings, u32 code) noexcept {
    auto& model = search_model(settings);
    message_composer_insert_utf16(&model, static_cast<u16>(code), clock_monotonic_milliseconds());
    return search_changed(settings);
}
// Editing keys of the focused search field; false when the key is not one.
bool search_key(ApplicationSettings& settings, u32 key, bool control, bool shift) noexcept {
    auto& model = search_model(settings);
    const u32 first = minimum(model.anchor, model.caret), last = maximum(model.anchor, model.caret);
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && (key == 'C' || key == 'X')) {
        if (last > first && clipboard_write_utf8(g_app.window, {model.text + first, last - first}) && key == 'X')
            result = message_composer_replace_range(&model, first, last, {});
    } else if (control && key == 'V') {
        char text[kSettingsSearchBytes];
        const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        u32 kept = 0;
        // One line: stop at the first control character, like a pasted query.
        while (read.ok() && kept < read.bytes) {
            const auto scalar = utf8_decode_one(text + kept, read.bytes - kept);
            if (!scalar.valid || scalar.codepoint < 0x20 || scalar.codepoint == 0x7f)
                break;
            kept += scalar.width;
        }
        if (kept)
            result = message_composer_insert_utf8(&model, {text, kept});
    } else if (!message_composer_edit_key(&model, key, control, shift, &result))
        return false;
    if (result == MessageComposerResult::changed)
        search_changed(settings);
    return true;
}
// Places the caret under the pointer in the measured search field.
bool search_point(ApplicationSettings& settings, float x, float y, bool extend) noexcept {
    auto& model = settings.search_edit;
    u32 position = 0, byte = 0;
    if (!text_edit_layout_hit_test(g_app.renderer.media_picker_layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(model), position, &byte))
        return false;
    if (message_composer_set_selection(&model, extend ? model.anchor : byte, byte) == MessageComposerResult::changed)
        return refresh();
    return true;
}

} // namespace settings_detail

void application_preferences_initialize() noexcept {
    g_app.preferences = client_preferences_defaults();
    const auto loaded = client_preferences_load(&g_app.preferences);
    if (!loaded.ok() && loaded.error != ClientPreferencesError::missing &&
        loaded.error != ClientPreferencesError::version)
        log_u64(LogLevel::warning, "preferences", "load_failed_using_defaults", static_cast<u64>(loaded.error));
    g_app.member_pane_visible = g_app.preferences.member_pane_visible;
    // Splitter widths persist in whole DIPs; zero keeps the startup default.
    const auto pane_limits = ui_splitter_limits(kUiSplitterMaximumPaneDips);
    g_app.channel_pane_width =
        ui_splitter_restored_width(g_app.preferences.channel_pane_width, g_app.channel_pane_width, pane_limits);
    g_app.member_pane_width =
        ui_splitter_restored_width(g_app.preferences.member_pane_width, g_app.member_pane_width, pane_limits);
    application_image_zoom_configure();
}
bool application_preferences_persist_pane_widths() noexcept {
    if (!g_app.pane_widths_persist_pending || g_app.pane_resize != PaneResizeKind::none)
        return false;
    g_app.pane_widths_persist_pending = false;
    const u16 channels = ui_splitter_persisted_width(g_app.channel_pane_width);
    const u16 members = ui_splitter_persisted_width(g_app.member_pane_width);
    if (channels == g_app.preferences.channel_pane_width && members == g_app.preferences.member_pane_width)
        return false;
    g_app.preferences.channel_pane_width = channels;
    g_app.preferences.member_pane_width = members;
    return persist();
}

// Set by WM_ACTIVATE(WA_INACTIVE), cleared on activation. A window shown
// without activation (offline probes) counts as active.
static bool g_window_inactive = false;

void application_animation_activity_synchronize() noexcept {
    const auto now = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
    // The official client holds avatar, server icon, emoji, GIF
    // and sticker animations while its window is inactive. The Settings
    // modal keeps the shell beneath it live, animations included.
    if (g_app.preferences.reduced_motion || g_app.renderer.live_resize || g_window_inactive ||
        application_window_minimized())
        animation_visual_runtime_suspend(&g_app.animations, now);
    else
        animation_visual_runtime_resume(&g_app.animations, now);
}
bool application_window_inactive() noexcept {
    return g_window_inactive;
}
void application_animation_window_active(bool active) noexcept {
    if (g_window_inactive == !active)
        return;
    g_window_inactive = !active;
    application_animation_activity_synchronize();
}
void application_preferences_apply() noexcept {
    g_app.member_pane_visible = g_app.preferences.member_pane_visible;
    application_user_settings_apply_display();
    const bool immediate = !g_app.preferences.smooth_scroll || g_app.preferences.reduced_motion;
    if (g_app.scroll_host.immediate != immediate)
        ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.scroll_host.immediate = immediate;
    if (g_app.renderer.reduced_motion != g_app.preferences.reduced_motion)
        content_text_retention_invalidate(&g_app.renderer.content_text_retention);
    g_app.renderer.reduced_motion = g_app.preferences.reduced_motion;
    application_animation_activity_synchronize();
    g_app.renderer.retained_presentation.valid = false;
    application_image_zoom_configure();
}
bool application_preferences_member_visibility(bool visible) noexcept {
    g_app.preferences.member_pane_visible = visible;
    g_app.member_pane_visible = visible;
    persist();
    if (g_app.settings)
        project(*g_app.settings);
    return synchronize_shell_layout() && refresh_account_text(true) && application_request_frame();
}
bool application_preferences_dm_profile_visibility(bool visible) noexcept {
    g_app.preferences.dm_profile_visible = visible;
    persist();
    return synchronize_shell_layout() && refresh_account_text(true) && application_request_frame();
}
bool application_preferences_image_zoom(u8 factor, u8 size) noexcept {
    auto& preferences = g_app.preferences;
    if (preferences.image_zoom_factor == factor && preferences.image_zoom_size == size)
        return true;
    preferences.image_zoom_factor = factor;
    preferences.image_zoom_size = size;
    const bool saved = persist();
    if (g_app.settings)
        project(*g_app.settings);
    return saved;
}
bool application_settings_commit_preferences() noexcept {
    const bool accepted = persist();
    if (g_app.settings) {
        project(*g_app.settings);
        if (g_app.settings->scene.active && !update_scene(*g_app.settings, g_app.settings->scene.viewport))
            return false;
    }
    application_request_frame();
    return accepted;
}
void application_settings_cancel_pointer() noexcept {
    auto* settings = g_app.settings;
    if (!settings)
        return;
    const bool restore = settings->slider_preview &&
                         settings->pressed_account_serial == g_app.plugin_account_scope_serial &&
                         settings->pressed_profile == g_app.account.status.profile_id;
    settings->slider_preview = false;
    settings->preference_key = 0;
    if (restore) {
        SettingBinding binding{};
        control_node(settings->slider_target.control_id, &binding);
        if (theme_binding(binding)) {
            g_app.preferences.theme = settings->slider_baseline.theme;
            g_app.preferences.gradient = settings->slider_baseline.gradient;
            copy_bytes(g_app.preferences.effect_profiles, settings->slider_baseline.effect_profiles,
                       sizeof(g_app.preferences.effect_profiles));
            application_custom_theme_preferences_changed(false, false);
        } else if (binding == SettingBinding::output_volume)
            g_app.preferences.output_volume = settings->slider_baseline.output_volume;
        application_preferences_apply();
        project(*settings);
        refresh();
    }
    close_choice();
    reset_value(&settings->pressed);
    settings->slider_dragging = false;
    settings->search_selecting = false;
    settings->backdrop_pressed = false;
    ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {});
    application_release_capture();
}
UiPageRegistry* application_settings_pages() noexcept {
    auto* settings = ensure();
    return settings ? &settings->registry : nullptr;
}
u64 application_settings_lifetime() noexcept {
    return g_app.settings_lifetime_serial;
}
bool application_settings_open_key(Utf8View key) noexcept {
    UiSettingsSchemaLocation location{};
    if (!ui_settings_schema_find(settings_catalog(), key, &location) || !application_settings_open() || !g_app.settings)
        return false;
    return navigate(*g_app.settings, {kNativeSettingsOwner, location.panel + 1},
                    location.node == kUiSettingsNoChoice ? 0 : settings_control_id(location.panel, location.node));
}
bool application_settings_open_page(UiPageKey page) noexcept {
    if (page.owner_id != kNativeSettingsOwner)
        return false;
    const auto* pages = application_settings_pages();
    if (!pages || !ui_page_registry_find(*pages, page) || !application_settings_open() || !g_app.settings)
        return false;
    if (!page_allowed(page))
        return true;
    return navigate(*g_app.settings, page, 0);
}
bool application_settings_focus_search() noexcept {
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return false;
    return ui_settings_scene_focus(&settings->scene, {UiSettingsTargetKind::search, {}, 0}) && refresh();
}
void application_settings_refresh_pages() noexcept {
    if (!g_app.settings)
        return;
    project(*g_app.settings);
    if (g_app.settings->scene.active && !refresh())
        log_write(LogLevel::warning, "plugins", "settings_refresh_failed");
}
bool application_settings_active() noexcept {
    return application_channel_overview_active() || (g_app.settings && g_app.settings->scene.active);
}
bool application_settings_appearance_selected() noexcept {
    const auto* selected = g_app.settings ? ui_page_registry_selected(g_app.settings->registry) : nullptr;
    return selected && (theme_panel(selected->key) ||
                        ui_page_key_equal(selected->key, {kNativeSettingsOwner, kSettingsSearchPage}));
}
HANDLE application_preferences_notification() noexcept {
    return client_preferences_writer_completion_event(g_app.preferences_writer);
}
HANDLE application_media_auth_notification() noexcept { return oauth_sign_in_notification(&g_spotify_account.sign_in); }
void application_media_auth_poll() noexcept {
    auto& account = g_spotify_account;
    if (!account.sign_in.runtime) return;
    if (account.stopping) {
        media_close(account);
        if (!account.stopping && g_app.settings) {
            project(*g_app.settings);
            if (g_app.settings->scene.active) refresh();
        }
        return;
    }
    const auto status = oauth_sign_in_status(&account.sign_in);
    const auto previous = account.message;
    using Phase = OAuthSignInPhase;
    if (status.phase == Phase::waiting && !account.launched) {
        SecureBuffer url{}, wide{};
        bool opened = false;
        if (oauth_sign_in_url(&account.sign_in, account.generation, &url) &&
            secure_buffer_initialize(&wide, (url.bytes + 1) * sizeof(wchar_t)).ok()) {
            auto* text = reinterpret_cast<wchar_t*>(wide.data);
            for (u32 i = 0; i < url.bytes; ++i) text[i] = static_cast<wchar_t>(url.data[i]);
            text[url.bytes] = 0;
            opened = reinterpret_cast<usize>(ShellExecuteW(g_app.window, L"open", text, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        }
        secure_buffer_release(&wide); secure_buffer_release(&url);
        account.launched = true;
        account.message = opened ? utf8_literal("Finish signing in in your browser.")
                                   : utf8_literal("Could not open your browser. Try signing in again.");
        if (!opened) media_close(account);
    } else if (status.phase == Phase::exchanging) {
        account.message = utf8_literal("Completing sign-in...");
    } else if (status.phase == Phase::ready) {
        SecureBuffer credentials{};
        const bool taken = oauth_sign_in_take(&account.sign_in, account.generation, &credentials);
        if (taken) {
            const auto& tokens = *reinterpret_cast<const OAuthTokens*>(credentials.data);
            account.saved = oauth_account_save(account.client_id(), {tokens.refresh, tokens.refresh_bytes}, provider);
            account.message = account.saved ? saved_account_message()
                : utf8_literal("Could not save sign-in on this device. Try again.");
        } else account.message = utf8_literal("Could not complete sign-in. Try again.");
        secure_buffer_release(&credentials);
        media_close(account);
    } else if (status.phase == Phase::failed || status.phase == Phase::expired || status.phase == Phase::cancelled) {
        account.message = status.phase == Phase::expired ? utf8_literal("Sign-in expired. Try again.")
            : status.phase == Phase::cancelled || status.error == OAuthError::denied ? utf8_literal("Sign-in cancelled.")
            : status.error == OAuthError::invalid_client ? utf8_literal("The provider rejected this app's registration.")
            : status.error == OAuthError::invalid_grant ? utf8_literal("Authorization expired or was already used. Sign in again.")
            : status.error == OAuthError::invalid_request ? utf8_literal("The provider rejected the sign-in request.")
            : status.error == OAuthError::scope ? utf8_literal("The required account permission was not granted. Sign in again.")
            : status.error == OAuthError::malformed ? utf8_literal("The provider returned an unexpected sign-in response.")
            : utf8_literal("Sign-in failed. Try again.");
        media_close(account);
    }
    if (!utf8_equal(previous, account.message) && g_app.settings) {
        project(*g_app.settings);
        if (g_app.settings->scene.active) refresh();
    }
}
void application_media_auth_shutdown() noexcept {
    auto& account = g_spotify_account;
    if (oauth_sign_in_stop(&account.sign_in, 1000)) {
        account.checked = false; account.saved = false; reset_value(&account.message);
        account.stopping = false;
    }
}
void application_preferences_poll() noexcept {
    if (!g_app.preferences_writer)
        return;
    u64 revision = 0;
    ClientPreferencesResult result{};
    bool ready = false;
    if (!client_preferences_writer_poll(g_app.preferences_writer, &revision, &result, &ready) || !ready ||
        revision != g_app.preferences_request_revision)
        return;
    g_app.preferences_save_pending = false;
    g_app.preferences_save_failed = !result.ok();
    if (!result.ok()) {
        log_u64(LogLevel::warning, "preferences", "save_failed", static_cast<u64>(result.error));
        log_u64(LogLevel::warning, "preferences", "save_native_error", result.native_error);
    }
    if (g_app.settings) {
        // Only the retry row reads the save flags, and it exists while a save has failed.
        const bool retry_visible = g_app.settings->save_failed || !result.ok();
        g_app.settings->save_failed = !result.ok();
        if (retry_visible) {
            project(*g_app.settings);
            if (g_app.settings->scene.active)
                refresh();
        }
    }
}
void application_preferences_shutdown() noexcept {
    if (!client_preferences_writer_stop(&g_app.preferences_writer, kAccountStopTimeoutMs))
        log_write(LogLevel::warning, "preferences", "shutdown_write_still_pending");
}
bool application_settings_open() noexcept {
    if (application_channel_overview_active())
        return false;
    if (!g_app.renderer_ready || g_app.text_form || !begin_signed_out())
        return false;
    auto* settings = ensure();
    if (!settings) {
        end_signed_out();
        return false;
    }
    if (settings->scene.active)
        return true;
    application_friends_close_requests();
    settings->previous_focus = g_app.context_focus;
    settings->previous_composer_focus = g_app.message_composer.focused;
    application_composer_text_input_suspend();
    dismiss_popup();
    application_image_viewer_close();
    if (!application_profile_close())
        return false;
    end_scrollbar_drag();
    g_app.pane_resize = PaneResizeKind::none;
    ui_scroll_host_cancel(&g_app.scroll_host);
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    ui_button_pointer(&g_app.message_composer_scene.widgets, UiButtonPointerEvent::cancel, {});
    ui_button_pointer(&g_app.shell_scene.widgets, UiButtonPointerEvent::cancel, {});
    application_release_capture();
    g_app.button_pointer_owner = nullptr;
    g_app.message_composer_pointer_selection = false;
    g_app.message_text_pointer_selection = false;
    g_app.message_composer.focused = false;
    reset_message_composer_caret_timing(false);
    g_app.context_focus = nullptr;
    message_composer_clear(&settings->search_edit);
    settings->search_revision = settings->search_edit.text_revision;
    settings->search_bytes = 0;
    const auto* selected = ui_page_registry_selected(settings->registry);
    if (searching(*settings) || !selected || !page_allowed(selected->key))
        ui_page_registry_select(&settings->registry, {kNativeSettingsOwner, static_cast<u64>(SettingsPanel::appearance) + 1});
    // Before sign-in Settings fills the window: there is no shell beneath it.
    ui_settings_scene_set_presentation(&settings->scene, g_signed_out.active
                                                              ? UiSettingsPresentation::layer
                                                              : UiSettingsPresentation::modal);
    settings->scene.focus = {UiSettingsTargetKind::search, {}, 0};
    project(*settings);
    ui_settings_scene_set_focus_visible(&settings->scene, g_app.keyboard_focus_visible);
    if (!update_scene(*settings, application_viewport_dips())) {
        g_app.context_focus = settings->previous_focus;
        if (settings->previous_composer_focus)
            focus_message_composer_control(MessageComposerControl::edit);
        end_signed_out();
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &settings->scene);
    synchronize_member_subscription(clock_monotonic_milliseconds());
    application_preferences_apply();
    application_sync_attachment_drop_target();
    if (refresh())
        return true;
    application_settings_close();
    return false;
}
bool application_settings_resume() noexcept {
    if (!application_settings_open() || !g_app.settings)
        return false;
    // The reopened scene counts as an open; mark its entrance as played.
    auto& settings = *g_app.settings;
    settings.entrance_opens = settings.scene.opens;
    settings.entrance_started_ms = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    return true;
}
UiSettingsScene* application_settings_scene() noexcept {
    return g_app.settings && g_app.settings->scene.active ? &g_app.settings->scene : nullptr;
}
bool application_settings_select_page(const UiPageKey& page) noexcept {
    return g_app.settings && ui_page_registry_select(&g_app.settings->registry, page).ok() && refresh();
}
void application_settings_close(bool restore_focus) noexcept {
    application_custom_theme_cancel_interaction();
    if (application_channel_overview_active()) {
        application_channel_overview_close(restore_focus);
        return;
    }
    application_theme_editor_close(true);
    if (application_theme_editor_active())
        return;
    close_choice();
    application_store_visible(false, CatalogKind::plugin);
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return;
    ui_scroll_host_cancel(&g_app.scroll_host);
    application_voice_settings_visible(false);
    application_settings_cancel_pointer();
    const auto* previous = settings->previous_focus;
    const bool composer = settings->previous_composer_focus;
    application_release_capture();
    reset_value(&settings->pressed);
    settings->notifications_active = false;
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&settings->scene);
    application_preferences_apply();
    application_sync_attachment_drop_target();
    if (restore_focus && g_app.account_ui && !g_app.login.visible) {
        g_app.context_focus = const_cast<UiWidgetManager*>(previous);
        if (composer)
            focus_message_composer_control(MessageComposerControl::edit);
        refresh_account_text(true);
    }
    synchronize_member_subscription(clock_monotonic_milliseconds());
    end_signed_out();
    application_request_frame();
}
UiScrollBinding application_settings_scroll_binding(bool sidebar) noexcept {
    // Look up the lazily created holder on every sample, rather than keeping a pointer
    // that could outlive closing Settings or unloading a contributed page.
    return {nullptr, sidebar ? 1u : 0u,
            [](void*, u64 key, UiScrollRange* range) noexcept {
                auto* settings = g_app.settings;
                if (!settings || !settings->scene.active || settings->choice.active || settings->slider_dragging ||
                    settings->scrollbar_dragging || g_app.theme_editor || g_app.text_form ||
                    !settings_allowed())
                    return false;
                const auto& scene = settings->scene;
                const bool left = key != 0;
                UiRect viewport{};
                if (!ui_settings_scene_scroll_viewport(scene, left, &viewport))
                    return false;
                const auto geometry = ui_settings_scene_scrollbar(scene, left);
                *range = {viewport,
                          left ? scene.sidebar_offset : scene.content_offset,
                          geometry.maximum_scroll,
                          32,
                          1,
                          0,
                          left ? g_app.settings_lifetime_serial : scene.page_generation};
                return true;
            },
            [](void*, u64 key, double offset, bool* changed) noexcept {
                auto* settings = g_app.settings;
                if (!settings || !settings->scene.active)
                    return false;
                auto& scene = settings->scene;
                const float before = key ? scene.sidebar_offset : scene.content_offset;
                const bool ok = ui_settings_scene_scroll(&scene, key != 0, static_cast<float>(offset) - before);
                *changed = before != (key ? scene.sidebar_offset : scene.content_offset);
                return ok;
            }};
}

namespace settings_detail {

// Keyboard paging, track clicks and focus reveals glide on the wheel's curve
// (and jump when smooth scrolling is off or motion is reduced).

void settings_glide_to(bool sidebar, float target) noexcept {
    ui_scroll_host_scroll_to(&g_app.scroll_host, application_settings_scroll_binding(sidebar), target,
                             clock_microseconds(g_app.clock, g_app.process_start, clock_now()),
                             96.0 / maximum(g_app.renderer.dpi, 1u));
}
void settings_glide_reveal(float before) noexcept {
    auto& scene = g_app.settings->scene;
    const float after = scene.content_offset;
    if (after == before)
        return;
    ui_settings_scene_scroll(&scene, false, before - after);
    settings_glide_to(false, after);
}

} // namespace settings_detail

bool application_settings_synchronize() noexcept {
    // One write per completed splitter drag, outside the pointer path.
    application_preferences_persist_pane_widths();
    if (!application_custom_theme_synchronize())
        return false;
    if (application_channel_overview_active())
        return application_channel_overview_synchronize();
    finish_signed_out();
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return true;
    if (g_signed_out.active && !g_app.login.visible) {
        // Signed in while Settings was open: the account owns the runtime now.
        reset_value(&g_signed_out);
        ui_settings_scene_set_presentation(&settings->scene, UiSettingsPresentation::modal);
        if (!application_login_view_suspend(false))
            log_write(LogLevel::warning, "ui", "login_view_resume_failed");
    }
    if (!settings_allowed()) {
        application_settings_close(false);
        return true;
    }
    const u32 native_page = native_page_of(*settings);
    // A connection change (READY, a stop) re-projects the account pages by itself.
    const bool connection = application_account_settings_connection_changed();
    if (settings->native_page != native_page ||
        settings->custom_theme_revision != application_custom_theme_view().revision || connection)
        project(*settings);
    const bool notifications = native_page == static_cast<u32>(SettingsPanel::notifications) + 1 ||
                               native_page == kSettingsSearchPage;
    if (notifications && !settings->notifications_active) {
        settings->notifications_active = true;
        application_notifications_settings_refresh(&g_app);
        project(*settings);
    } else if (!notifications)
        settings->notifications_active = false;
    if (settings->choice.active && !choice_popup_current(settings->choice))
        close_choice();
    ui_settings_scene_set_focus_visible(&settings->scene, g_app.keyboard_focus_visible);
    if (!update_scene(*settings, application_viewport_dips()))
        return recover_rejected_page(*settings);
    const u64 now = clock_monotonic_milliseconds();
    bool animating = ui_settings_scene_tick(&settings->scene, now, g_app.preferences.reduced_motion);
    animating = application_store_synchronize(&settings->scene) || animating;
    if (settings->scene.flash_control) {
        const auto timing = ui_settings_layout_tokens(application_theme().styles).duration;
        const u64 elapsed = now - settings->flash_started_ms;
        const u64 total = timing.flash_ms, fade = minimum<u64>(timing.flash_fade_ms, total);
        float strength = 0;
        if (elapsed < total)
            strength = elapsed < fade ? maximum(.001f, static_cast<float>(elapsed) / static_cast<float>(fade))
                       : elapsed > total - fade ? static_cast<float>(total - elapsed) / static_cast<float>(fade)
                                                : 1.f;
        ui_settings_scene_set_flash(&settings->scene, settings->scene.flash_control, strength);
        animating = animating || settings->scene.flash_control;
    }
    {
        // Dialog entrance: a spring from the scrim fade to full scale.
        const auto timing = ui_settings_layout_tokens(application_theme().styles).duration;
        const bool motion = g_app.window && !application_reduced_motion() && !g_app.renderer.live_resize;
        if (settings->entrance_opens != settings->scene.opens) {
            settings->entrance_opens = settings->scene.opens;
            settings->entrance_started_ms = motion ? now : 0;
        }
        const u64 duration = maximum<u64>(1, timing.modal_enter_ms);
        const u64 elapsed = now - settings->entrance_started_ms;
        const bool running = motion && settings->entrance_started_ms && elapsed < duration;
        renderer_set_settings_modal_progress(&g_app.renderer,
                                             running ? ui_settings_modal_entrance(timing, elapsed) : 1.f);
        animating = animating || running;
    }
    if (animating)
        application_request_frame();
    const auto palette_revision =
        hash64_counted_bytes(kHash64Seed, &g_app.preferences.gradient, sizeof(g_app.preferences.gradient));
    if (!ui_settings_scene_bind_palette(&settings->scene,
                                        {settings, resolve_palette, palette_revision ? palette_revision : 1}))
        return false;
    if (settings->choice.active) {
        UiRect anchor{};
        if (!ui_settings_scene_control_anchor(
                settings->scene, {UiSettingsTargetKind::control, settings->choice.token.page, settings->choice.control_id},
                &anchor) ||
            !ui_rect_equal(anchor, settings->choice.anchor) ||
            !ui_rect_equal(g_app.popup_scene.viewport, application_viewport_dips()))
            close_choice();
    }
    return (g_app.renderer.interaction_text_settings_revision == settings->scene.revision &&
            g_app.renderer.interaction_text_popup_revision ==
                (g_app.popup_scene.active && g_app.popup_scene.kind == PopupKind::settings_choice
                     ? g_app.popup_scene.revision
                     : 0)) ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}
void application_settings_choice_account_invalidated() noexcept {
    close_choice();
}
void application_settings_release() noexcept {
    application_store_release();
    application_theme_editor_release();
    application_channel_overview_release();
    application_settings_close(false);
    auto* settings = g_app.settings;
    if (!settings)
        return;
    application_settings_cancel_pointer();
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    ui_settings_scene_release(&settings->scene);
    ui_page_registry_clear(&settings->registry);
    message_composer_release(&settings->search_edit);
    g_app.settings = nullptr;
    g_app.settings_held_key = 0;
    VirtualFree(settings, 0, MEM_RELEASE);
}

} // namespace maicord::app_detail

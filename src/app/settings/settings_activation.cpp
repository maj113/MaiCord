#include "maicord/ui/theme/background_color_math.hpp"
#include "settings_internal.hpp"
#include "../effects/effects.hpp"
#include "../account/profile_editor.hpp"
#include "../account/activity.hpp"
#include "../account/account_presentation.hpp"
#include "../account/application_account_logout.hpp"
#include "../notifications/notifications.hpp"
#include "theme_editor.hpp"
#include "../update/updates.hpp"
#include "../chat/application_link_trust.hpp"
#include "user_settings.hpp"
#include "account_settings.hpp"
#include "keybinds_settings.hpp"
#include "../notifications/desktop.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {
namespace settings_detail {

bool resolve_palette(void*, u64 control, u32 choice, BackgroundTheme* output) noexcept {
    // The current device's accent dot uses the theme accent.
    if (output && choice == kUiSettingsAccentChoice && settings_list_control(control)) {
        const Color accent = application_theme().accent;
        *output = background_theme_defaults();
        output->colors[0] = background_color_pack(accent);
        output->color_count = 1;
        return true;
    }
    SettingBinding binding{};
    control_node(control, &binding);
    if (output && binding == SettingBinding::installed_themes) {
        // An installed theme shows its own preview; the Browse themes tile has none.
        const auto installed = application_custom_theme_installed();
        const u32 at = installed_theme_at(choice);
        if (choice >= kInstalledThemeTiles || at >= installed.count)
            return false;
        *output = installed[at].preview;
        return true;
    }
    return settings_theme_swatch(g_app.preferences, binding, choice, output);
}
void close_choice() noexcept {
    if (g_app.settings)
        reset_value(&g_app.settings->choice);
    if (g_app.popup_owner_kind != PopupOwnerKind::settings_choice)
        return;
    const u32 count = minimum(g_app.popup_scene.item_count, g_app.popup_scene.items.count);
    popup_scene_close(&g_app.popup_scene);
    for (u32 index = 0; index < count; ++index)
        reset_value(&g_app.popup_scene.items[index]);
    g_app.popup_owner_kind = PopupOwnerKind::none;
    g_app.renderer.retained_presentation.valid = false;
}
const UiSettingsControl* choice_control(const ApplicationSettingsChoice& choice) noexcept {
    const auto* settings = g_app.settings;
    if (!choice.active || !settings || !settings->scene.active || !settings_allowed() ||
        choice.profile_id != g_app.account.status.profile_id ||
        !plugin_ui_scope_current(g_app.plugin_account_scope_serial, choice.account_serial) ||
        !ui_page_key_equal(settings->scene.selected_page, choice.token.page) ||
        settings->registry.revision != choice.token.registry_revision)
        return nullptr;
    const auto* content = &settings->scene.content;
    if (content->revision != choice.token.content_revision || content->controls.count > 8192 ||
        !content->controls.data || choice.control_index >= content->controls.count)
        return nullptr;
    const auto& control = content->controls[choice.control_index];
    return control.id == choice.control_id && control.kind == UiSettingsControlKind::choice && control.enabled &&
                   control.choices.count == choice.choice_count && control.choices.count &&
                   control.choices.count <= kUiSettingsChoiceMaximum &&
                   (control.selected_choice < control.choices.count || control.selected_choice == kUiSettingsNoChoice)
               ? &control
               : nullptr;
}
bool choice_popup_current(const ApplicationSettingsChoice& choice) noexcept {
    return choice_control(choice) && g_app.popup_owner_kind == PopupOwnerKind::settings_choice &&
           g_app.popup_scene.active && g_app.popup_scene.kind == PopupKind::settings_choice &&
           g_app.popup_scene.owner_id == choice.control_id && g_app.popup_scene.opens == choice.popup_opens &&
           g_app.popup_scene.widgets.generation == choice.popup_generation;
}
bool project_choice(ApplicationSettingsChoice choice) noexcept {
    const auto* control = choice_control(choice);
    if (!control) {
        close_choice();
        return false;
    }
    PopupItem items[kUiSettingsChoiceMaximumItems]{};
    const u32 count = ui_settings_choice_items(*control, choice.first, {items, kUiSettingsChoiceMaximumItems});
    if (!count || !popup_scene_open(&g_app.popup_scene, PopupKind::settings_choice, choice.control_id, {items, count},
                                    choice.anchor, application_viewport_dips(), maximum(220.f, ui_rect_width(choice.anchor)),
                                    false)
                       .ok()) {
        close_choice();
        return false;
    }
    g_app.popup_owner_kind = PopupOwnerKind::settings_choice;
    choice.popup_opens = g_app.popup_scene.opens;
    choice.popup_generation = g_app.popup_scene.widgets.generation;
    choice.pressed_action = 0;
    g_app.settings->choice = choice;
    u32 index = choice.first ? 1 : 0;
    if (control->selected_choice >= choice.first && control->selected_choice < choice.first + kUiSettingsChoiceWindow)
        index += control->selected_choice - choice.first;
    bool changed = false;
    popup_scene_select_index(&g_app.popup_scene, index, &changed);
    if (refresh())
        return true;
    close_choice();
    return false;
}

static bool open_choice(const UiSettingsTarget& target) noexcept {
    auto* settings = g_app.settings;
    ApplicationSettingsChoice choice{};
    const auto* control = settings ? ui_settings_scene_control(settings->scene, target, &choice.control_index) : nullptr;
    if (!control || control->kind != UiSettingsControlKind::choice || !control->enabled ||
        !ui_settings_scene_control_anchor(settings->scene, target, &choice.anchor))
        return false;
    choice.token = {target.page, settings->registry.revision, settings->scene.content_revision};
    choice.control_id = target.control_id;
    choice.account_serial = g_app.plugin_account_scope_serial;
    choice.profile_id = g_app.account.status.profile_id;
    choice.choice_count = control->choices.count;
    // An unselected dropdown opens on its first window.
    choice.first = control->selected_choice < control->choices.count
                       ? control->selected_choice / kUiSettingsChoiceWindow * kUiSettingsChoiceWindow
                       : 0;
    choice.active = true;
    return project_choice(choice);
}

// Publishes the search model's text: switches to or from the results page.
void apply_search(ApplicationSettings& settings) noexcept {
    if (settings.search_edit.text_revision == settings.search_revision &&
        settings.search_edit.bytes == settings.search_bytes)
        return;
    settings.search_revision = settings.search_edit.text_revision;
    const u32 valid = settings.search_bytes = minimum(settings.search_edit.bytes, kSettingsSearchBytes);
    const auto* selected = ui_page_registry_selected(settings.registry);
    const UiPageKey search{kNativeSettingsOwner, kSettingsSearchPage};
    const bool searching = selected && ui_page_key_equal(selected->key, search);
    application_custom_theme_cancel_interaction();
    close_choice();
    const UiPageKey fallback{kNativeSettingsOwner, static_cast<u64>(SettingsPanel::appearance) + 1};
    if (valid && !searching) {
        settings.search_return =
            selected && selected->key.owner_id == kNativeSettingsOwner ? selected->key.page_id : fallback.page_id;
        ui_page_registry_select(&settings.registry, search);
    } else if (!valid && searching) {
        if (!ui_page_registry_select(&settings.registry, {kNativeSettingsOwner, settings.search_return}).ok())
            ui_page_registry_select(&settings.registry, fallback);
    }
    ++settings.content[kSettingsSearchPage - 1].revision;
    project(settings);
}
void set_search(ApplicationSettings& settings, const Utf8View& text) noexcept {
    const u32 bytes = minimum(text.bytes, kSettingsSearchBytes);
    u32 valid = 0;
    while (valid < bytes) {
        const auto decoded = utf8_decode_one(text.data + valid, bytes - valid);
        if (!decoded.valid || !decoded.width || valid + decoded.width > bytes)
            break;
        valid += decoded.width;
    }
    if (valid != settings.search_edit.bytes || !bytes_equal(settings.search_text, text.data, valid))
        message_composer_replace_range(&settings.search_edit, 0, settings.search_edit.bytes, {text.data, valid});
    apply_search(settings);
}

// The scene holds the flashed control; synchronize animates its strength.
static bool flash(ApplicationSettings& settings, u64 control_id) noexcept {
    settings.flash_started_ms = clock_monotonic_milliseconds();
    return ui_settings_scene_set_flash(&settings.scene, control_id, .001f);
}

// Opens a panel, optionally scrolling to and flashing one of its controls.
bool navigate(ApplicationSettings& settings, const UiPageKey& page, u64 control_id) noexcept {
    if (!page_allowed(page))
        return false;
    // Profiles is the profile editor's page: it takes the modal over.
    if (page.owner_id == kNativeSettingsOwner && page.page_id == static_cast<u64>(SettingsPanel::profiles) + 1)
        return application_profile_editor_open(ProfileEditorTab::main);
    if (settings.search_bytes)
        set_search(settings, {});
    if (!theme_panel(page))
        application_custom_theme_cancel_interaction();
    close_choice();
    if (!ui_page_registry_select(&settings.registry, page).ok())
        return false;
    project(settings);
    if (!update_scene(settings, application_viewport_dips()))
        return false;
    if (control_id) {
        const UiSettingsTarget target{UiSettingsTargetKind::control, page, control_id};
        ui_settings_scene_reveal(&settings.scene, target);
        ui_settings_scene_focus(&settings.scene, target);
        flash(settings, control_id);
    }
    return refresh();
}

static bool navigate_link(ApplicationSettings& settings, u64 id) noexcept {
    if (id >= kSettingsPanelLinkFirst + 0x100 && id < kSettingsPanelLinkFirst + 0x100 + settings.registry.count)
        return navigate(settings, settings.registry.storage[static_cast<u32>(id - kSettingsPanelLinkFirst - 0x100)].key,
                        0);
    if (id >= kSettingsPanelLinkFirst && id < kSettingsPanelLinkFirst + kSettingsPanelCount)
        return navigate(settings, {kNativeSettingsOwner, id - kSettingsPanelLinkFirst + 1}, 0);
    return false;
}

bool open_result(ApplicationSettings& settings, u64 control_id) noexcept {
    UiSettingsSchemaLocation location{};
    if (navigate_link(settings, control_id))
        return true;
    if (!settings_control_location(control_id, &location))
        return false;
    const UiPageKey page{kNativeSettingsOwner, location.panel + 1};
    // A store page draws its own column, with no row to reveal: its search-only rows open the page and start their
    // action there (Themes > Import theme…).
    if (page.page_id == kThemesPage || page.page_id == kPluginsPage || page.page_id == kEffectsPage) {
        SettingBinding binding{};
        control_node(control_id, &binding);
        return navigate(settings, page, 0) &&
               (binding != SettingBinding::import_theme ||
                settled(settings, application_custom_theme_action(ApplicationCustomThemeAction::import_file)));
    }
    return navigate(settings, page, control_id);
}
bool searching(const ApplicationSettings& settings) noexcept {
    return ui_page_key_equal(settings.scene.selected_page, {kNativeSettingsOwner, kSettingsSearchPage});
}

// Motion needs a presenting window; headless hosts settle immediately.
static void animate_toggle(ApplicationSettings& settings, const UiSettingsTarget& target,
                           const UiSettingsActivation& request) noexcept {
    if (request.kind == UiSettingsControlKind::toggle && g_app.window)
        ui_settings_scene_animate_toggle(&settings.scene, target, request.checked,
                                         clock_monotonic_milliseconds());
}

// Reprojects after an action and reports its result once the scene is refreshed.
bool settled(ApplicationSettings& settings, bool result) noexcept {
    project(settings);
    return refresh() && result;
}

// A toggle's result: the thumb animates on success, then the page refreshes.
static bool settled_toggle(ApplicationSettings& settings, const UiSettingsTarget& target,
                           const UiSettingsActivation& request, bool result) noexcept {
    if (result)
        animate_toggle(settings, target, request);
    return settled(settings, result);
}

bool activate(const UiSettingsTarget& target, bool reverse, bool picker, u32 selected, bool save) noexcept {
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return false;
    UiSettingsActivation request{};
    if (!ui_settings_scene_activation(settings->scene, target, reverse, &request))
        return false;
    const bool choice_kind = request.kind == UiSettingsControlKind::choice || request.kind == UiSettingsControlKind::slider ||
                             request.kind == UiSettingsControlKind::palette || request.kind == UiSettingsControlKind::radio ||
                             settings_option_row(request.kind);
    if (choice_kind && target.kind == UiSettingsTargetKind::control) {
        if (picker && request.kind == UiSettingsControlKind::choice)
            return open_choice(target);
        if (selected != ~0u) {
            const auto* control = ui_settings_scene_control(settings->scene, target);
            // A colours row reports its trailing button as choices.count.
            const u32 limit = !control ? 0
                              : control->choices.count + (control->kind == UiSettingsControlKind::colors &&
                                                                  control->action_label.bytes ? 1u : 0u);
            if (selected >= limit)
                return false;
            request.selected_choice = selected;
        }
    }
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        application_settings_close();
        return true;
    case UiSettingsTargetKind::search:
        return ui_settings_scene_focus(&settings->scene, target) && refresh();
    case UiSettingsTargetKind::search_clear:
        set_search(*settings, {});
        ui_settings_scene_focus(&settings->scene, {UiSettingsTargetKind::search, {}, 0});
        return refresh();
    case UiSettingsTargetKind::action:
        if (target.control_id == kSettingsLogOutAction) {
            application_settings_close();
            return application_account_logout_open();
        }
        return false;
    case UiSettingsTargetKind::page:
        if (target.page.owner_id == kNativeSettingsOwner &&
            target.page.page_id == static_cast<u64>(SettingsPanel::profiles) + 1)
            return application_profile_editor_open(ProfileEditorTab::main);
        if (settings->search_bytes)
            set_search(*settings, {});
        if (!theme_panel(target.page))
            application_custom_theme_cancel_interaction();
        close_choice();
        return ui_page_registry_select(&settings->registry, target.page).ok() && refresh() &&
               ui_settings_scene_focus(&settings->scene, target);
    default:
        break;
    }
    // The Plugins page is the store's and takes its own input.
    if (target.page.page_id == kPluginsPage)
        return false;
    if (request.kind == UiSettingsControlKind::category)
        return navigate_link(*settings, target.control_id);
    if (application_keybinds_settings_row(target.control_id))
        return settled(*settings, application_keybinds_settings_activate_row(target.control_id,
                                                                              request.selected_choice));
    if (application_activity_settings_row(target.control_id))
        return settled(*settings, application_activity_settings_activate_row(target.control_id));
    if (application_updates_changelog_row(target.control_id))
        return settled(*settings, application_updates_activate_row(target.control_id));
    if (settings_list_control(target.control_id))
        return settled(*settings, application_account_settings_activate_row(target.control_id));
    SettingBinding binding{};
    if (!control_node(target.control_id, &binding))
        return false;
    if (bool result = false; binding != SettingBinding::game_detection &&
                             application_activity_settings_activate(binding, request.checked,
                                                                    request.selected_choice, &result))
        return settled_toggle(*settings, target, request, result);
    switch (binding) {
    case SettingBinding::customize_theme:
        return application_theme_editor_open();
    case SettingBinding::browse_effects:
        // The store pages are settings pages: selecting one swaps the panel, keeping the modal.
        return application_settings_select_page({kNativeSettingsOwner, kEffectsPage});
    case SettingBinding::import_theme:
        // Search leads to Themes (leaving the results), whose header owns Import; the picker opens from there.
        return navigate(*settings, {kNativeSettingsOwner, kThemesPage}, 0) &&
               settled(*settings, application_custom_theme_action(ApplicationCustomThemeAction::import_file));
    case SettingBinding::installed_themes: {
        // The tiles are the installed themes, then Browse themes.
        const u32 installed = application_custom_theme_installed().count;
        const u32 shown = installed < kInstalledThemeTiles ? installed : kInstalledThemeTiles;
        if (request.selected_choice >= shown)
            return navigate(*settings, {kNativeSettingsOwner, kThemesPage}, 0);
        return settled(*settings, application_custom_theme_apply_installed(installed_theme_at(request.selected_choice), false));
    }
    case SettingBinding::spotify_account:
        return settled(*settings, media_account_action());
    case SettingBinding::notification_setup:
        // Setup is a user action; showing status or entering the page never enrolls.
        application_notifications_setup(&g_app);
        return settled(*settings, true);
    case SettingBinding::windows_notification_settings: {
        const HINSTANCE opened =
            ShellExecuteW(win32_hwnd(g_app.window), L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL);
        settings->notifications_settings_failed = reinterpret_cast<usize>(opened) <= 32;
        return settled(*settings, true);
    }
    case SettingBinding::preferences_retry:
        persist();
        return settled(*settings, true);
    case SettingBinding::update_status:
    case SettingBinding::update_channel:
    case SettingBinding::update_interval:
        return settled(*settings, application_updates_activate(binding, request.selected_choice));
    case SettingBinding::update_release_notes: {
        // Through the external-link confirmation, like any link outside MaiCord.
        char url[kApplicationUpdatesReleaseNotesBytes];
        const u32 bytes = application_updates_release_notes({url, sizeof(url)});
        return bytes && application_link_open({url, bytes}, false);
    }
    default:
        break;
    }
    const auto backend = settings_binding_backend(binding);
    if (backend == SettingBackend::os)
        // Startup entry and protocol handler: the Windows registry is the value.
        return settled_toggle(*settings, target, request,
                              application_desktop_settings_activate(binding, request.checked));
    if (backend == SettingBackend::voice) {
        const bool result =
            application_voice_settings_activate(binding, request.checked, request.selected_choice, save);
        if (result)
            animate_toggle(*settings, target, request);
        if (save)
            return settled(*settings, result);
        project(*settings);
        return update_scene(*settings, settings->scene.viewport) && application_request_frame() && result;
    }
    if (backend == SettingBackend::keybinds)
        return settled(*settings, application_keybinds_settings_activate(binding));
    if (backend == SettingBackend::proto1 || backend == SettingBackend::account)
        return settled_toggle(*settings, target, request,
                              backend == SettingBackend::proto1
                                  ? application_user_settings_activate(binding, request.checked, request.selected_choice)
                                  : application_account_settings_activate(binding));
    if (backend != SettingBackend::local)
        return false;
    // The built-in gradient knobs never act on (or fork) an installed theme: the projection disables them.
    if ((binding == SettingBinding::background_effect || binding == SettingBinding::gradient_direction ||
         binding == SettingBinding::gradient_intensity) &&
        application_custom_theme_view().custom_active)
        return false;
    if (bool result = false; binding == SettingBinding::background_effect &&
                             application_effects_activate(request.selected_choice, &result))
        return settled(*settings, result);
    auto next = g_app.preferences;
    if (binding == SettingBinding::gradient_direction || binding == SettingBinding::gradient_intensity)
        next.gradient = gradient_edit_base();
    if (!settings_store_write(&next, binding, {request.checked, request.selected_choice}) ||
        !client_preferences_valid(next))
        return false;
    const bool shell_changed = next.member_pane_visible != g_app.preferences.member_pane_visible;
    const auto prior_preferences = g_app.preferences;
    g_app.preferences = next;
    if (theme_binding(binding) &&
        !application_custom_theme_preferences_changed(
            binding == SettingBinding::theme_base || binding == SettingBinding::theme_color, false)) {
        g_app.preferences = prior_preferences;
        return false;
    }
    if (binding == SettingBinding::desktop_notifications || binding == SettingBinding::message_sounds ||
        binding == SettingBinding::selected_channel_sounds || binding == SettingBinding::output_volume)
        application_notifications_settings_refresh(&g_app);
    animate_toggle(*settings, target, request);
    application_preferences_apply();
    // Hidden channels and voice name colours change what the channel list shows.
    if (g_app.account_ui && g_app.renderer_ready &&
        (binding == SettingBinding::show_hidden_channels || binding == SettingBinding::role_colors_everywhere))
        (void)(synchronize_navigation_view() && refresh_account_text(true));
    if (save)
        persist();
    project(*settings);
    // Borrowed descriptors advance their source revision immediately. Keep the
    // retained scene current for the next queued input without shaping text;
    // the frame commits the glyph suffix once after the input burst.
    if (!save)
        return update_scene(*settings, settings->scene.viewport) && application_request_frame();
    return (!shell_changed || synchronize_shell_layout()) && refresh();
}

} // namespace settings_detail
} // namespace maicord::app_detail

#include "maicord/ui/splitter.hpp"
#include "application_content_point.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "application_settings.hpp"
#include "application_profile_editor.hpp"
#include "application_activity.hpp"
#include "application_channel_overview.hpp"
#include "application_state.hpp"
#include "application_login_ui.hpp"
#include "application_ui_runtime.hpp"
#include "application_account_presentation.hpp"
#include "application_account_module.hpp"
#include "application_account_logout.hpp"
#include "application_profile.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_interaction.hpp"
#include "application_scrolling.hpp"
#include "application_modal_scroll.hpp"
#include "application_shell_input.hpp"
#include "application_attachments.hpp"
#include "application_plugins.hpp"
#include "application_plugin_settings_projection.hpp"
#include "application_plugin_browser.hpp"
#include "application_plugin_browser_projection.hpp"
#include "application_friends.hpp"
#include "application_notifications.hpp"
#include "application_theme_editor.hpp"
#include "application_custom_theme.hpp"
#include "application_updates.hpp"
#include "settings_catalog.hpp"
#include "text_edit_keys.hpp"
#include "application_voice_settings.hpp"
#include "application_user_settings.hpp"
#include "application_account_settings.hpp"
#include "application_keybinds_settings.hpp"
#include "application_desktop.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/app/plugin_settings_projection.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/guild_icon_media.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/settings_choice.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {
struct ApplicationSettingsChoice {
    NativePluginUiToken token;
    u64 control_id, account_serial, profile_id;
    UiRect anchor;
    u32 first, control_index, choice_count, popup_opens, pressed_action;
    u16 popup_generation;
    bool active;
};
static_assert(sizeof(ApplicationSettingsChoice) == 96);
// Native panels, search and plugin pages share one registry.
inline constexpr u32 kSettingsPageCapacity = 24;
struct ApplicationPluginSettingsPage {
    UiPageRegistry registry;
    UiPage pages[kSettingsPageCapacity];
    UiSettingsPageContent content;
    UiSettingsControl* controls;
    NativePluginUiToken token;
    u32 backed_count;
};
// One bank serves the plugin catalog (its 21-row ceiling), any catalog panel
// and search results. Keybinds, which lists every default shortcut, projects
// into its own bank (application_keybinds_settings_bank).
inline constexpr u32 kSettingsControlCapacity = 28;
static_assert(kSettingsControlCapacity >= kApplicationPluginSettingsMaximumControls);
// Only short pages carry avatar media (the Account card); longer ones have none.
inline constexpr u32 kSettingsAvatarRows = 12;
inline constexpr u32 kSettingsPanelRowLimit = kKeybindsSettingsBankCapacity;
static_assert(kSettingsPanelRowLimit >= kSettingsControlCapacity);
inline constexpr u32 kSettingsSearchHits = 16;
inline constexpr u32 kSettingsSearchBytes = 40;
inline constexpr u32 kSettingsSearchPage = kSettingsPanelCount + 1;
inline constexpr u64 kSettingsPanelLinkFirst = 0x7F000000u;
struct ApplicationSettings {
    UiSettingsScene scene;
    UiPageRegistry registry;
    UiPage pages[kSettingsPageCapacity];
    NativePluginHost plugins;
    NativePluginSlot plugin_slots[8];
    // One selected native page borrows this descriptor bank. Native panels,
    // search results and the plugin catalog never borrow it at the same time.
    UiSettingsControl controls[kSettingsControlCapacity];
    UiSettingsControlPresentation presentation[kSettingsControlCapacity];
    UiSettingsAvatarMedia avatar_media[kSettingsAvatarRows];
    u64 custom_theme_revision;
    u32 native_page;
    ApplicationPluginCatalogText catalog_text[kApplicationPluginCatalogWindow];
    u64 catalog_revision, pressed_catalog_revision, pressed_account_serial, pressed_profile;
    UiSettingsPageContent content[kSettingsSearchPage];
    // The search field edits search_text through the shared text model, like
    // every other single-line field; search_bytes is the applied query.
    char search_text[kSettingsSearchBytes];
    u32 search_bytes, search_revision;
    MessageComposer search_edit;
    bool search_selecting;
    // A press on the scrim around the modal; releasing it there closes.
    bool backdrop_pressed;
    u64 search_return, flash_started_ms, entrance_started_ms, entrance_opens;
    UiWidgetManager* previous_focus;
    UiSettingsTarget pressed;
    u32 pressed_revision;
    NativePluginUiToken pressed_token;
    bool pressed_plugin;
    bool previous_composer_focus, save_failed;
    bool notifications_active, notifications_settings_failed;
    NativePluginError plugin_error;
    bool scrollbar_dragging, scrollbar_sidebar;
    bool slider_dragging;
    u16 slider_generation;
    u32 preference_key;
    ClientPreferences slider_baseline;
    ClientThemeStorageSnapshot* slider_theme_baseline;
    UiSettingsTarget slider_target;
    bool slider_preview;
    UiRect slider_viewport;
    // Pointer offset from the grabber's centre when the drag started on it.
    float slider_grab;
    float scrollbar_grab;
    ApplicationSettingsChoice choice;
    ApplicationPluginSettingsPage* plugin_page;
    bool plugin_page_committed;
};
// The bank is allocated lazily; nothing is allocated at application startup.
static_assert(sizeof(ApplicationSettings) <= 3 * 4096);
namespace {
// Settings opened before sign-in: it borrows an account UI runtime for its
// text and measurement (released again unless an account started meanwhile)
// and covers the sign-in form until it closes.
struct SignedOutSession {
    bool active;
    bool created_runtime;
    // Closed: the next frame returns the runtime and the sign-in form, outside
    // any caller still holding the settings owner.
    bool ending;
} g_signed_out;

bool settings_allowed() noexcept {
    return g_app.account_ui && (!g_app.login.visible || g_signed_out.active);
}

bool page_allowed(UiPageKey page) noexcept {
    return !g_signed_out.active ||
           (page.owner_id == kSettingsNativeOwner && page.page_id >= 1 &&
            page.page_id <= kSettingsPanelCount &&
            settings_panel_signed_out(static_cast<SettingsPanel>(page.page_id - 1)));
}

constexpr u64 kNativeSettingsOwner = kSettingsNativeOwner;
constexpr u64 kPluginsPage = static_cast<u64>(SettingsPanel::plugins) + 1;

SettingBinding binding_of(const UiSettingsSchemaNode& node) noexcept {
    return node.binding < static_cast<u32>(SettingBinding::count) ? static_cast<SettingBinding>(node.binding)
                                                                  : SettingBinding::none;
}
bool theme_panel(UiPageKey page) noexcept {
    return page.owner_id == kNativeSettingsOwner && page.page_id == static_cast<u64>(SettingsPanel::appearance) + 1;
}
// Rows of side-by-side options that a click picks directly.
bool option_row(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::segments || kind == UiSettingsControlKind::swatches ||
           kind == UiSettingsControlKind::tabs || kind == UiSettingsControlKind::buttons ||
           kind == UiSettingsControlKind::colors;
}
bool theme_binding(SettingBinding binding) noexcept {
    return binding == SettingBinding::theme_base || binding == SettingBinding::theme_color ||
           binding == SettingBinding::background_effect || binding == SettingBinding::gradient_direction ||
           binding == SettingBinding::gradient_intensity;
}
const UiSettingsSchemaNode* control_node(u64 id, SettingBinding* binding = nullptr) noexcept {
    UiSettingsSchemaLocation location{};
    const auto* node = settings_control_location(id, &location) ? settings_catalog_node(location) : nullptr;
    if (binding)
        *binding = node ? binding_of(*node) : SettingBinding::none;
    return node;
}
bool release_slider_theme(ApplicationSettings& settings) noexcept {
    if (!settings.slider_theme_baseline)
        return true;
    if (!VirtualFree(settings.slider_theme_baseline, 0, MEM_RELEASE))
        return false;
    settings.slider_theme_baseline = nullptr;
    return true;
}
bool capture_slider_theme(ApplicationSettings& settings, UiSettingsTarget target) noexcept {
    if (!release_slider_theme(settings))
        return false;
    // Built-in and notification gestures already have a preferences
    // baseline. Only an active imported background needs a whole document.
    SettingBinding binding{};
    control_node(target.control_id, &binding);
    if (!theme_binding(binding) || !application_custom_theme_document())
        return true;
    auto* snapshot = static_cast<ClientThemeStorageSnapshot*>(
        VirtualAlloc(nullptr, sizeof(ClientThemeStorageSnapshot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!snapshot)
        return false;
    settings.slider_theme_baseline = snapshot;
    if (application_custom_theme_snapshot(snapshot))
        return true;
    release_slider_theme(settings);
    return false;
}
bool release_plugin_page(ApplicationSettings& settings) noexcept {
    auto* page = settings.plugin_page;
    if (!page)
        return true;
    if (settings.plugin_page_committed) {
        if (page->controls)
            clear_bytes(page->controls, page->backed_count * static_cast<u32>(sizeof(UiSettingsControl)));
        clear_bytes(page, sizeof(*page));
    }
    if (!VirtualFree(page, 0, MEM_RELEASE))
        return false;
    settings.plugin_page = nullptr;
    settings.plugin_page_committed = false;
    return true;
}
bool update_scene(ApplicationSettings& settings, UiRect viewport) noexcept;
void close_choice() noexcept;
void detach_plugin(void*, u64 owner_id) noexcept {
    application_plugins_detach_owner(owner_id);
    application_settings_close();
}
BackgroundTheme gradient_edit_base() noexcept {
    const auto& background = application_theme().background;
    return static_cast<u8>(g_app.preferences.theme) >= static_cast<u8>(ClientTheme::aurora) &&
                   background_theme_valid(background)
               ? background
               : g_app.preferences.gradient;
}
bool persist() noexcept {
    const bool theme_saved = application_custom_theme_preferences_changed(false, true);
    const bool result = theme_saved &&
                        (g_app.preferences_writer || client_preferences_writer_create(&g_app.preferences_writer)) &&
                        client_preferences_writer_submit(g_app.preferences_writer, g_app.preferences,
                                                         &g_app.preferences_request_revision);
    g_app.preferences_save_failed = !result;
    g_app.preferences_save_pending = result;
    if (g_app.settings)
        g_app.settings->save_failed = !result;
    if (!result)
        log_u64(LogLevel::warning, "preferences", "save_queue_failed", GetLastError());
    return result;
}
bool same_control(const UiSettingsControl& a, const UiSettingsControl& b) noexcept {
    return a.id == b.id && a.kind == b.kind && a.enabled == b.enabled && a.checked == b.checked &&
           a.choices.data == b.choices.data && a.choices.count == b.choices.count &&
           a.selected_choice == b.selected_choice && a.editor == b.editor && utf8_equal(a.label, b.label) &&
           utf8_equal(a.description, b.description) && utf8_equal(a.action_label, b.action_label);
}
bool same_presentation(const UiSettingsControlPresentation& a, const UiSettingsControlPresentation& b) noexcept {
    return a.variant == b.variant && a.flags == b.flags && a.default_choice == b.default_choice;
}
Utf8View search_query(const ApplicationSettings& settings) noexcept {
    return {settings.search_text, settings.search_bytes};
}

// Live projection of one catalog node. Returns false when the node has no
// backing in the current state and must be omitted.
bool project_node(const ApplicationSettings& settings, u32 panel, u32 index, UiSettingsControl* control,
                  UiSettingsControlPresentation* presentation) noexcept {
    const auto& node = settings_catalog().panels[panel].nodes[index];
    const auto binding = binding_of(node);
    const auto custom = application_custom_theme_view();
    const bool theme_idle = !custom.busy && !custom.previewing;
    const bool idle = !custom.candidate_ready && !custom.previewing && !custom.busy;
    *control = {settings_control_id(panel, index), node.label, node.description, node.kind, true, false,
                node.choices, 0, node.action_label, nullptr};
    *presentation = {static_cast<UiSettingsVariant>(node.variant),
                     static_cast<u8>(node.flags & ui_settings_schema_marks ? ui_settings_presentation_marks : 0),
                     node.default_choice < kUiSettingsNoDefault ? static_cast<u16>(node.default_choice)
                                                                : kUiSettingsNoDefault};
    if (node.kind == UiSettingsControlKind::category)
        return !(node.flags & ui_settings_schema_dynamic) || custom.revision != 0;
    if (application_desktop_settings_project(binding, control))
        return true;
    if (binding != SettingBinding::game_detection && application_activity_settings_project(binding, control))
        return true;
    if (settings_binding_backend(binding) == SettingBackend::voice)
        return application_voice_settings_project(binding, control, presentation);
    if (settings_binding_backend(binding) == SettingBackend::proto1)
        return application_user_settings_project(binding, control, presentation);
    if (settings_binding_backend(binding) == SettingBackend::account)
        return application_account_settings_project(binding, control, presentation);
    if (settings_binding_backend(binding) == SettingBackend::keybinds)
        return application_keybinds_settings_project(binding, control, presentation);
    auto preferences = g_app.preferences;
    if (theme_binding(binding))
        preferences.gradient = gradient_edit_base();
    SettingValue value{};
    if (settings_store_read(preferences, binding, &value)) {
        control->checked = value.checked;
        control->selected_choice = value.choice;
    }
    // The default group spacing, and its highlighted mark, follow the message
    // display: 16px cozy, 0px compact.
    if (binding == SettingBinding::group_spacing) {
        const u16 fallback = g_app.message_scene.display.compact ? 0 : 3;
        presentation->default_choice = fallback;
        if (control->selected_choice == kUiSettingsNoChoice)
            control->selected_choice = fallback;
    }
    // Registered plugin effects follow the built-in effects.
    if (binding == SettingBinding::background_effect)
        application_plugins_fx_project(control);
    switch (binding) {
    case SettingBinding::theme_base:
    case SettingBinding::theme_color:
    case SettingBinding::background_effect:
    case SettingBinding::gradient_direction:
    case SettingBinding::gradient_intensity:
    case SettingBinding::customize_theme:
        control->enabled = theme_idle;
        return true;
    case SettingBinding::custom_import:
        control->description = custom.current;
        control->action_label =
            custom.can_retry_read ? utf8_literal("Choose another file…") : utf8_literal("Import theme…");
        control->enabled = !custom.busy;
        return custom.revision != 0;
    case SettingBinding::custom_installed: {
        const auto installed = application_custom_theme_catalog();
        control->choices = installed;
        control->selected_choice = 0;
        for (u32 i = 0; i < installed.count; ++i)
            if (custom.custom_active && utf8_equal(installed[i], custom.current))
                control->selected_choice = i;
        control->enabled = !custom.busy;
        return custom.revision && installed.count;
    }
    case SettingBinding::custom_preview:
        control->description = custom.candidate;
        return custom.revision && custom.candidate_ready && !custom.previewing;
    case SettingBinding::custom_apply:
        control->description = custom.candidate;
        return custom.revision && custom.candidate_ready && custom.previewing;
    case SettingBinding::custom_cancel:
        return custom.revision && (custom.candidate_ready || custom.previewing || custom.busy);
    case SettingBinding::custom_builtin:
        control->enabled = !custom.busy;
        return custom.revision && idle && !custom.can_retry_read && custom.custom_active;
    case SettingBinding::custom_restore:
        control->enabled = !custom.busy;
        return custom.revision && idle && !custom.can_retry_read && !custom.custom_active && custom.imported;
    case SettingBinding::custom_retry_read:
        control->description = custom.status;
        control->enabled = !custom.busy;
        return custom.revision && idle && custom.can_retry_read;
    case SettingBinding::custom_retry_save:
        control->description = custom.status;
        control->enabled = !custom.previewing;
        return custom.revision && idle && custom.save_failed;
    case SettingBinding::notification_setup:
        control->description = application_notifications_status(&g_app);
        return true;
    case SettingBinding::windows_notification_settings:
        if (settings.notifications_settings_failed)
            control->description = utf8_literal("Windows notification settings couldn't open. Try again.");
        return true;
    case SettingBinding::preferences_retry:
        control->description = g_app.preferences_save_pending ? utf8_literal("Saving…") : node.description;
        control->enabled = !g_app.preferences_save_pending;
        return settings.save_failed;
    case SettingBinding::update_status:
    case SettingBinding::update_channel:
    case SettingBinding::update_rollback:
    case SettingBinding::update_notice:
        return application_updates_project(binding, control, presentation);
    default:
        return true;
    }
}

UiSettingsAvatarMedia self_avatar_media() noexcept {
    return g_app.account_ui ? application_user_avatar_media(g_app.account_ui->chat_state.self_user_id)
                            : UiSettingsAvatarMedia{};
}
bool same_avatar_media(const UiSettingsAvatarMedia& left, const UiSettingsAvatarMedia& right) noexcept {
    return media_asset_key_equal(left.key, right.key) && left.hash_bytes == right.hash_bytes &&
           bytes_equal(left.hash, right.hash, left.hash_bytes);
}

u32 project_panel(ApplicationSettings& settings, u32 panel, bool* changed, UiSettingsControl* bank,
                  UiSettingsControlPresentation* bank_presentation, u32 capacity) noexcept {
    UiSettingsControl controls[kSettingsPanelRowLimit]{};
    UiSettingsControlPresentation presentation[kSettingsPanelRowLimit]{};
    capacity = minimum(capacity, kSettingsPanelRowLimit);
    u32 count = 0;
    UiSettingsSchemaLocation retry{};
    if (settings.save_failed && settings_catalog_binding(SettingBinding::preferences_retry, &retry) &&
        retry.panel != panel && project_node(settings, retry.panel, retry.node, &controls[0], &presentation[0]))
        ++count;
    const auto& nodes = settings_catalog().panels[panel].nodes;
    for (u32 i = 0; i < nodes.count && count < capacity; ++i)
        if (project_node(settings, panel, i, &controls[count], &presentation[count])) {
            ++count;
            const auto binding = binding_of(nodes[i]);
            if (binding == SettingBinding::devices_list || binding == SettingBinding::apps_list)
                count += application_account_settings_list(binding, {controls + count, capacity - count},
                                                           {presentation + count, capacity - count});
            if (binding == SettingBinding::keybinds_custom || binding == SettingBinding::keybinds_defaults)
                count += application_keybinds_settings_list(binding, {controls + count, capacity - count},
                                                            {presentation + count, capacity - count});
            if (binding == SettingBinding::registered_games)
                count += application_activity_settings_list({controls + count, capacity - count},
                                                            {presentation + count, capacity - count});
        }
    // A category left without rows (every dynamic row hidden) is dropped.
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) {
        if (controls[i].kind == UiSettingsControlKind::category &&
            (i + 1 == count || controls[i + 1].kind == UiSettingsControlKind::category))
            continue;
        *changed = *changed || !same_control(bank[kept], controls[i]) ||
                   !same_presentation(bank_presentation[kept], presentation[i]);
        bank[kept] = controls[i];
        bank_presentation[kept++] = presentation[i];
    }
    return kept;
}

u32 project_search(ApplicationSettings& settings, bool* changed) noexcept {
    UiSettingsSearchHit hits[kSettingsSearchHits]{};
    const auto& schema = settings_catalog();
    const u32 found = ui_settings_schema_search(schema, search_query(settings), {hits, kSettingsSearchHits});
    u32 count = 0;
    const auto emit = [&](const UiSettingsControl& control, const UiSettingsControlPresentation& presentation) noexcept {
        if (count >= kSettingsControlCapacity)
            return;
        *changed = *changed || !same_control(settings.controls[count], control) ||
                   !same_presentation(settings.presentation[count], presentation);
        settings.controls[count] = control;
        settings.presentation[count++] = presentation;
    };
    for (u32 panel = 0; panel < schema.panels.count; ++panel) {
        if (g_signed_out.active && !settings_panel_signed_out(static_cast<SettingsPanel>(panel)))
            continue;
        const auto& source = schema.panels[panel];
        bool header = false;
        const auto group = [&]() noexcept {
            if (header)
                return;
            header = true;
            emit({kSettingsPanelLinkFirst + panel, source.title, {}, UiSettingsControlKind::category, true, false},
                 {UiSettingsVariant::standard, ui_settings_presentation_link, kUiSettingsNoDefault});
        };
        for (u32 node = 0; node < source.nodes.count; ++node) {
            bool hit = false;
            for (u32 i = 0; i < found && !hit; ++i)
                hit = hits[i].panel == panel && hits[i].node == node;
            UiSettingsControl control{};
            UiSettingsControlPresentation presentation{};
            if (!hit || !project_node(settings, panel, node, &control, &presentation))
                continue;
            group();
            emit(control, presentation);
        }
        if (!header && (ui_settings_search_text_matches(search_query(settings), source.title) ||
                        ui_settings_search_text_matches(search_query(settings), source.keywords)))
            group();
    }
    for (u32 page = 0; page < settings.registry.count; ++page) {
        const auto& entry = settings.registry.storage[page];
        if (entry.key.owner_id != kNativeSettingsOwner && !g_signed_out.active &&
            ui_settings_search_text_matches(search_query(settings), entry.label))
            emit({kSettingsPanelLinkFirst + 0x100 + page, entry.label, {}, UiSettingsControlKind::category, true,
                  false},
                 {UiSettingsVariant::standard, ui_settings_presentation_link, kUiSettingsNoDefault});
    }
    if (!count)
        emit({kSettingsPanelLinkFirst - 1, utf8_literal("No results"), utf8_literal("Try a different word."),
              UiSettingsControlKind::notice, true, false},
             {UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
    return count;
}

void project(ApplicationSettings& settings) noexcept {
    bool changed[kSettingsSearchPage]{};
    const auto custom = application_custom_theme_view();
    if (settings.custom_theme_revision != custom.revision) {
        settings.custom_theme_revision = custom.revision;
        changed[static_cast<u32>(SettingsPanel::appearance)] = true;
    }
    const auto* selected = ui_page_registry_selected(settings.registry);
    const u32 native = selected && selected->key.owner_id == kNativeSettingsOwner &&
                               selected->key.page_id >= 1 && selected->key.page_id <= kSettingsSearchPage
                           ? static_cast<u32>(selected->key.page_id)
                           : 0;
    if (settings.native_page != native) {
        // Release the previous borrow before building the next native page.
        for (u32 i = 0; i < kSettingsSearchPage; ++i) {
            if (settings.content[i].controls.count)
                changed[i] = true;
            settings.content[i].controls = {};
            settings.content[i].presentation = nullptr;
            settings.content[i].meters = nullptr;
            settings.content[i].avatar_media = nullptr;
        }
        settings.native_page = native;
        application_voice_settings_visible(native == static_cast<u32>(SettingsPanel::voice) + 1);
        application_keybinds_settings_visible(native == static_cast<u32>(SettingsPanel::keybinds) + 1);
        application_plugin_browser_page_visible(native == kPluginsPage);
        application_account_settings_visible(native >= 1 && native <= kSettingsPanelCount
                                                 ? static_cast<SettingsPanel>(native - 1)
                                                 : SettingsPanel::count);
    }
    auto& appearance = settings.content[static_cast<u32>(SettingsPanel::appearance)];
    const Utf8View status = custom.revision ? custom.status : Utf8View{};
    if (!utf8_equal(appearance.description, status)) {
        appearance.description = status;
        changed[static_cast<u32>(SettingsPanel::appearance)] = true;
    }
    UiSettingsControl* browser_controls = nullptr;
    UiSettingsControlPresentation* browser_presentation = nullptr;
    bool browser_changed = false;
    const u32 browser_count = native == kPluginsPage
                                  ? application_plugin_browser_project(&browser_controls, &browser_presentation,
                                                                       settings.plugin_error, &browser_changed)
                                  : 0;
    if (browser_count) {
        // The catalog tabs borrow their own bank, like Keybinds.
        auto& page = changed[kPluginsPage - 1];
        auto& content = settings.content[kPluginsPage - 1];
        const u64 catalog_revision = application_plugins_catalog_revision();
        page = page || browser_changed || settings.catalog_revision != catalog_revision ||
               content.controls.count != browser_count || content.controls.data != browser_controls;
        settings.catalog_revision = catalog_revision;
        content.controls = {browser_controls, browser_count};
        content.presentation = browser_presentation;
        content.avatar_media = nullptr;
        content.meters = nullptr;
    } else if (native == kPluginsPage) {
        UiSettingsControl plugin_controls[kApplicationPluginSettingsMaximumControls]{};
        const u32 plugin_count = application_plugins_settings_controls(
            {plugin_controls, kApplicationPluginSettingsMaximumControls},
            {settings.catalog_text, kApplicationPluginCatalogWindow}, settings.plugin_error);
        auto& page = changed[kPluginsPage - 1];
        for (u32 i = 0; i < plugin_count; ++i) {
            page = page || !same_control(settings.controls[i], plugin_controls[i]);
            settings.controls[i] = plugin_controls[i];
        }
        const u64 catalog_revision = application_plugins_catalog_revision();
        page = page || settings.catalog_revision != catalog_revision ||
               settings.content[kPluginsPage - 1].controls.count != plugin_count;
        settings.catalog_revision = catalog_revision;
        settings.content[kPluginsPage - 1].controls = {settings.controls, plugin_count};
        settings.content[kPluginsPage - 1].presentation = nullptr;
        settings.content[kPluginsPage - 1].avatar_media = nullptr;
    } else if (native == static_cast<u32>(SettingsPanel::keybinds) + 1) {
        // Keybinds borrows its own bank, sized for every default shortcut.
        auto& page = changed[native - 1];
        auto& content = settings.content[native - 1];
        UiSettingsControl* controls = nullptr;
        UiSettingsControlPresentation* presentation = nullptr;
        application_keybinds_settings_bank(&controls, &presentation);
        const u32 count =
            project_panel(settings, native - 1, &page, controls, presentation, kKeybindsSettingsBankCapacity);
        page = page || content.controls.count != count || content.controls.data != controls;
        content.controls = {controls, count};
        content.presentation = presentation;
        content.avatar_media = nullptr;
        content.meters = nullptr;
    } else if (native) {
        auto& page = changed[native - 1];
        const u32 count = native == kSettingsSearchPage
                              ? project_search(settings, &page)
                              : project_panel(settings, native - 1, &page, settings.controls, settings.presentation,
                                              kSettingsControlCapacity);
        auto& content = settings.content[native - 1];
        page = page || content.controls.count != count;
        content.controls = {settings.controls, count};
        content.presentation = settings.presentation;
        // The account card draws the cached self avatar over its initials,
        // through the same user-avatar key as the footer and message authors.
        UiSettingsSchemaLocation card{};
        const u64 card_id =
            settings_catalog_binding(SettingBinding::account_card, &card) ? settings_control_id(card.panel, card.node) : 0;
        const auto self = self_avatar_media();
        const bool avatars = count <= kSettingsAvatarRows;
        for (u32 i = 0; avatars && i < count; ++i) {
            const auto media = card_id && settings.controls[i].id == card_id ? self : UiSettingsAvatarMedia{};
            if (!same_avatar_media(settings.avatar_media[i], media)) {
                settings.avatar_media[i] = media;
                page = true;
            }
        }
        page = page || (content.avatar_media != nullptr) != avatars;
        content.avatar_media = avatars ? settings.avatar_media : nullptr;
        content.meters = native == static_cast<u32>(SettingsPanel::voice) + 1
                             ? application_voice_settings_meters({settings.controls, count})
                             : nullptr;
    }
    for (u32 i = 0; i < kSettingsSearchPage; ++i)
        if (changed[i])
            ++settings.content[i].revision;
}

} // namespace

UiSettingsAvatarMedia application_user_avatar_media(u64 user_id) noexcept {
    UiSettingsAvatarMedia media{};
    if (!g_app.account_ui || !user_id)
        return media;
    const auto* user = user_store_find(g_app.account_ui->user_store, user_id);
    if (!user)
        return media;
    const Utf8View hash = user_store_avatar_hash(*user);
    const u32 variant = user_avatar_variant(hash);
    if (!variant || !hash.bytes || hash.bytes > kUiSettingsAvatarHashBytes)
        return media;
    media.key = media_asset_user_avatar(user->id, variant, asset_hash_animated(hash));
    media.hash_bytes = static_cast<u8>(hash.bytes);
    copy_bytes(media.hash, hash.data, hash.bytes);
    return media;
}

UiSettingsAvatarMedia application_guild_icon_media(u64 guild_id) noexcept {
    UiSettingsAvatarMedia media{};
    if (!g_app.account_ui || !guild_id)
        return media;
    MediaAssetKey key{};
    Utf8View hash{};
    if (!guild_icon_media_key(g_app.account_ui->chat_state, guild_id, &key, &hash) ||
        hash.bytes > kUiSettingsAvatarHashBytes)
        return media;
    media.key = key;
    media.hash_bytes = static_cast<u8>(hash.bytes);
    copy_bytes(media.hash, hash.data, hash.bytes);
    return media;
}

UiSettingsAvatarMedia application_listed_user_avatar_media(u64 user_id, Utf8View avatar_hash,
                                                           u16 discriminator) noexcept {
    UiSettingsAvatarMedia media{};
    if (!user_id)
        return media;
    if (const u32 variant = user_avatar_variant(avatar_hash);
        variant && avatar_hash.bytes <= kUiSettingsAvatarHashBytes) {
        media.key = media_asset_user_avatar(user_id, variant, asset_hash_animated(avatar_hash));
        media.hash_bytes = static_cast<u8>(avatar_hash.bytes);
        copy_bytes(media.hash, avatar_hash.data, avatar_hash.bytes);
        return media;
    }
    // "embed/avatars/{n}.png", as the profile card requests it.
    constexpr char prefix[] = "embed/avatars/";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    const u32 index = discriminator ? discriminator % 5u : static_cast<u32>((user_id >> 22) % 6u);
    copy_bytes(media.hash, prefix, prefix_bytes);
    media.hash[prefix_bytes] = static_cast<char>('0' + index);
    copy_bytes(media.hash + prefix_bytes + 1, ".png", 4);
    media.hash_bytes = static_cast<u8>(prefix_bytes + 5);
    char url[64];
    constexpr char host[] = "https://cdn.discordapp.com/";
    copy_bytes(url, host, sizeof(host) - 1);
    copy_bytes(url + sizeof(host) - 1, media.hash, media.hash_bytes);
    media.key = media_asset_embed(hash64_bytes(kHash64Seed, url, sizeof(host) - 1 + media.hash_bytes), 1, 0,
                                  MediaEmbedResource::image, MediaAttachmentFormat::png, false,
                                  MediaResolutionTier::px512);
    media.key.kind = MediaAssetKind::profile_image;
    return media;
}

namespace {
bool begin_signed_out() noexcept {
    if (!g_app.login.visible)
        return g_app.account_ui != nullptr;
    if (g_signed_out.active)
        return true;
    if (g_signed_out.ending) {
        // Reopened before the previous session finished: keep its runtime.
        g_signed_out = {true, g_signed_out.created_runtime, false};
        return true;
    }
    const bool created = !g_app.account_ui;
    if (!application_initialize_account_ui(g_app))
        return false;
    if (!application_login_view_suspend(true)) {
        if (created)
            application_release_account_ui(g_app);
        return false;
    }
    g_signed_out = {true, created, false};
    return true;
}

void end_signed_out() noexcept {
    if (g_signed_out.active)
        g_signed_out = {false, g_signed_out.created_runtime, true};
}

void finish_signed_out() noexcept {
    if (!g_signed_out.ending)
        return;
    const bool release = g_signed_out.created_runtime && !g_app.account.instance && g_app.login.visible;
    g_signed_out = {};
    if (release)
        application_release_account_ui(g_app);
    if (!application_login_view_suspend(false))
        log_write(LogLevel::warning, "ui", "login_view_resume_failed");
}

ApplicationSettings* ensure() noexcept {
    if (g_app.settings)
        return g_app.settings;
    if (g_app.settings_lifetime_serial == ~u64{0})
        return nullptr;
    auto* settings = static_cast<ApplicationSettings*>(
        VirtualAlloc(nullptr, sizeof(ApplicationSettings), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!settings)
        return nullptr;
    if (!ui_page_registry_initialize(&settings->registry, {settings->pages, kSettingsPageCapacity})) {
        VirtualFree(settings, 0, MEM_RELEASE);
        return nullptr;
    }
    if (!native_plugin_host_initialize(&settings->plugins, &settings->registry, {settings->plugin_slots, 8},
                                       {native_plugin_known_capabilities, 4, 64 * 1024 + kPluginSdkStateHeaderBytes, 256 * 1024, detach_plugin,
                                        nullptr, kNativeSettingsOwner})) {
        VirtualFree(settings, 0, MEM_RELEASE);
        return nullptr;
    }
    const auto& schema = settings_catalog();
    for (u32 i = 0; i < kSettingsSearchPage; ++i) {
        const Utf8View label = i < kSettingsPanelCount ? schema.panels[i].title : utf8_literal("Search results");
        const bool plugins = i == kPluginsPage - 1;
        settings->content[i] = {label, plugins ? kApplicationPluginSettingsDescription : Utf8View{}, {}, 1};
        if (!ui_page_registry_register(&settings->registry, {{kNativeSettingsOwner, i + 1}, label, &settings->content[i]})
                 .ok()) {
            VirtualFree(settings, 0, MEM_RELEASE);
            return nullptr;
        }
    }
    ui_page_registry_select(&settings->registry, {kNativeSettingsOwner, static_cast<u64>(SettingsPanel::appearance) + 1});
    message_composer_initialize(&settings->search_edit, {settings->search_text, kSettingsSearchBytes});
    settings->search_revision = settings->search_edit.text_revision;
    ui_settings_scene_set_presentation(&settings->scene, UiSettingsPresentation::modal);
    ++g_app.settings_lifetime_serial;
    g_app.settings = settings;
    settings->save_failed = g_app.preferences_save_failed;
    project(*settings);
    return settings;
}
// A native page whose projection the scene rejects would fail every frame
// and stall presentation. Fall back to Appearance (then close) instead; a
// transient rejection (a too-small viewport) keeps failing until it passes.
bool recover_rejected_page(ApplicationSettings& settings) noexcept {
    const auto* selected = ui_page_registry_selected(settings.registry);
    if (!selected || selected->key.owner_id != kNativeSettingsOwner || !selected->context ||
        ui_settings_page_content_valid(*static_cast<const UiSettingsPageContent*>(selected->context)))
        return false;
    log_u64(LogLevel::error, "preferences", "settings_page_rejected", selected->key.page_id);
    const UiPageKey fallback{kNativeSettingsOwner, static_cast<u64>(SettingsPanel::appearance) + 1};
    if (!ui_page_key_equal(selected->key, fallback) && ui_page_registry_select(&settings.registry, fallback).ok()) {
        project(settings);
        if (update_scene(settings, application_viewport_dips()))
            return application_request_frame();
    }
    application_settings_close(true);
    return true;
}
bool refresh() noexcept {
    if (!application_settings_synchronize())
        return false;
    return application_request_frame();
}
void bind_chrome(ApplicationSettings& settings) noexcept {
    // Contributed pages expand from the registry, so its revision relayouts;
    // the signed-out list is a navigation of its own.
    const bool signed_out = g_signed_out.active;
    const auto navigation =
        signed_out ? settings_catalog_navigation_signed_out()
                   : settings_catalog_navigation(application_account_logout_available());
    ui_settings_scene_bind_navigation(&settings.scene, navigation,
                                      settings.registry.revision ^ (signed_out ? 0x8000'0000u : 0u));
    const bool focused = settings.scene.focus.kind == UiSettingsTargetKind::search && g_app.window &&
                         GetFocus() == g_app.window;
    ui_settings_scene_set_search(
        &settings.scene,
        {{search_query(settings), minimum(settings.search_edit.anchor, settings.search_bytes),
          minimum(settings.search_edit.caret, settings.search_bytes), true, focused, focused},
         focused ? &g_app.renderer.media_picker_edit_geometry : nullptr,
         utf8_literal("Search")});
}
bool update_scene(ApplicationSettings& settings, UiRect viewport) noexcept {
    ui_settings_scene_bind_theme_styles(&settings.scene, application_theme().styles);
    bind_chrome(settings);
    const auto* selected = ui_page_registry_selected(settings.registry);
    if (!selected || selected->key.owner_id == kNativeSettingsOwner) {
        const bool updated = ui_settings_scene_update(&settings.scene, &settings.registry, viewport);
        if (updated && !release_plugin_page(settings))
            log_write(LogLevel::warning, "plugins", "settings_projection_release_pending");
        return updated;
    }
    const auto* slot = native_plugin_host_find(settings.plugins, selected->key.owner_id);
    NativePluginSettingsSnapshot source{};
    const auto read = slot ? native_plugin_settings_snapshot(selected->context, &source)
                           : NativePluginSettingsError::invalid_argument;
    auto fail = [&](NativePluginSettingsError error) noexcept {
        close_choice();
        settings.plugin_error = error == NativePluginSettingsError::unsupported ? NativePluginError::unsupported
                                : error == NativePluginSettingsError::capacity  ? NativePluginError::capacity
                                                                                : NativePluginError::invalid_argument;
        project(settings);
        if (!ui_page_registry_select(&settings.registry, {kNativeSettingsOwner, kPluginsPage}).ok())
            return false;
        return update_scene(settings, viewport);
    };
    if (read != NativePluginSettingsError::none)
        return fail(read);
    auto* page = settings.plugin_page;
    if (!page) {
        const usize maximum_bytes = sizeof(ApplicationPluginSettingsPage) +
                                    static_cast<usize>(kNativePluginSettingsMaximumControls) * sizeof(UiSettingsControl);
        page = static_cast<ApplicationPluginSettingsPage*>(VirtualAlloc(nullptr, maximum_bytes, MEM_RESERVE, PAGE_READWRITE));
        if (!page)
            return fail(NativePluginSettingsError::capacity);
        settings.plugin_page = page;
    }
    if (!settings.plugin_page_committed) {
        if (!VirtualAlloc(page, sizeof(*page), MEM_COMMIT, PAGE_READWRITE))
            return fail(NativePluginSettingsError::capacity);
        settings.plugin_page_committed = true;
    }
    // A failed release leaves an erased holder that can be retried or reused.
    page->controls = reinterpret_cast<UiSettingsControl*>(reinterpret_cast<u8*>(page) + sizeof(*page));
    if (source.controls.count > page->backed_count) {
        const usize bytes = sizeof(*page) + static_cast<usize>(source.controls.count) * sizeof(UiSettingsControl);
        if (!VirtualAlloc(page, bytes, MEM_COMMIT, PAGE_READWRITE))
            return fail(NativePluginSettingsError::capacity);
        page->backed_count = source.controls.count;
    }
    const NativePluginUiToken observed{selected->key, slot->generation, settings.registry.revision, source.revision};
    if (!ui_page_key_equal(page->token.page, observed.page) || page->token.generation != observed.generation ||
        page->token.registry_revision != observed.registry_revision ||
        page->token.content_revision != observed.content_revision) {
        const auto projected =
            native_plugin_settings_project(source, {page->controls, page->backed_count}, &page->content);
        if (projected != NativePluginSettingsError::none)
            return fail(projected);
        page->registry = settings.registry;
        page->registry.storage = {page->pages, kSettingsPageCapacity};
        for (u32 i = 0; i < settings.registry.count; ++i) {
            page->pages[i] = settings.pages[i];
            if (page->pages[i].key.owner_id != kNativeSettingsOwner)
                page->pages[i].context = nullptr;
        }
        page->pages[page->registry.selected_index].context = &page->content;
        page->token = observed;
    }
    return ui_settings_scene_update(&settings.scene, &page->registry, viewport);
}
bool resolve_palette(void*, u64 control, u32 choice, BackgroundTheme* output) noexcept {
    // The current device's accent dot uses the theme accent.
    if (output && choice == kUiSettingsAccentChoice && control >= kSettingsListFirst &&
        control < kSettingsListFirst + 0x10000) {
        const Color accent = application_theme().accent;
        *output = background_theme_defaults();
        output->colors[0] = static_cast<u32>(accent.red * 255 + .5f) << 16 |
                            static_cast<u32>(accent.green * 255 + .5f) << 8 | static_cast<u32>(accent.blue * 255 + .5f);
        output->color_count = 1;
        return true;
    }
    SettingBinding binding{};
    control_node(control, &binding);
    return settings_theme_swatch(g_app.preferences, binding, choice, output);
}
void close_choice() noexcept {
    if (g_app.settings)
        g_app.settings->choice = {};
    if (g_app.popup_owner_kind != PopupOwnerKind::settings_choice)
        return;
    const u32 count = minimum(g_app.popup_scene.item_count, g_app.popup_scene.items.count);
    popup_scene_close(&g_app.popup_scene);
    for (u32 index = 0; index < count; ++index)
        g_app.popup_scene.items[index] = {};
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
    if (choice.token.page.owner_id != kNativeSettingsOwner) {
        NativePluginUiToken current{};
        if (!native_plugin_host_settings_token(&settings->plugins, choice.token.page, &current).ok() ||
            current.generation != choice.token.generation ||
            current.registry_revision != choice.token.registry_revision ||
            current.content_revision != choice.token.content_revision)
            return nullptr;
    }
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
bool open_choice(UiSettingsTarget target, const NativePluginUiToken* expected) noexcept {
    auto* settings = g_app.settings;
    ApplicationSettingsChoice choice{};
    const auto* control = settings ? ui_settings_scene_control(settings->scene, target, &choice.control_index) : nullptr;
    if (!control || control->kind != UiSettingsControlKind::choice || !control->enabled ||
        !ui_settings_scene_control_anchor(settings->scene, target, &choice.anchor))
        return false;
    choice.token = {target.page, 0, settings->registry.revision, settings->scene.content_revision};
    if (target.page.owner_id != kNativeSettingsOwner) {
        if (expected)
            choice.token = *expected;
        else if (!native_plugin_host_settings_token(&settings->plugins, target.page, &choice.token).ok())
            return false;
    }
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
        // Plugin pages are not remembered; clearing returns to Appearance.
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
void set_search(ApplicationSettings& settings, Utf8View text) noexcept {
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
bool flash(ApplicationSettings& settings, u64 control_id) noexcept {
    settings.flash_started_ms = clock_monotonic_milliseconds();
    return ui_settings_scene_set_flash(&settings.scene, control_id, .001f);
}
// Opens a panel, optionally scrolling to and flashing one of its controls.
bool navigate(ApplicationSettings& settings, UiPageKey page, u64 control_id) noexcept {
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
bool navigate_link(ApplicationSettings& settings, u64 id) noexcept {
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
    return settings_control_location(control_id, &location) &&
           navigate(settings, {kNativeSettingsOwner, location.panel + 1}, control_id);
}
bool searching(const ApplicationSettings& settings) noexcept {
    return ui_page_key_equal(settings.scene.selected_page, {kNativeSettingsOwner, kSettingsSearchPage});
}

// Motion needs a presenting window; headless hosts settle immediately.
void animate_toggle(ApplicationSettings& settings, UiSettingsTarget target,
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

bool activate_plugin(ApplicationSettings& settings, UiSettingsTarget target, const UiSettingsActivation& request,
                     const NativePluginUiToken* expected) noexcept {
    // ABI-5 text, colour and keybind rows edit through the text form.
    if (request.kind == UiSettingsControlKind::action && application_plugins_settings_edit(target.page, target.control_id))
        return true;
    NativePluginUiToken token{};
    if (expected)
        token = *expected;
    else if (!native_plugin_host_settings_token(&settings.plugins, target.page, &token).ok())
        return false;
    const u64 serial = g_app.plugin_account_scope_serial, profile = g_app.account.status.profile_id;
    const auto result = native_plugin_host_activate_settings(&settings.plugins, token, request);
    if (!plugin_ui_scope_current(g_app.plugin_account_scope_serial, serial) ||
        profile != g_app.account.status.profile_id)
        return false;
    if (result.ok())
        animate_toggle(settings, target, request);
    return result.ok() && (!result.changed || refresh());
}
bool activate_plugin_catalog(ApplicationSettings& settings, UiSettingsTarget target, const UiSettingsActivation& request) noexcept {
    settings.plugin_error = NativePluginError::none;
    if (plugin_browser_control(target.control_id)) {
        const bool result = application_plugin_browser_activate(target.control_id, request);
        if (g_app.settings != &settings)
            return result;
        if (result)
            animate_toggle(settings, target, request);
        return settled(settings, result);
    }
    if (target.control_id == 1) {
        if (application_plugins_save_pending())
            application_plugins_retry_save();
        else
            application_plugins_choose();
        return settled(settings, true);
    }
    if (target.control_id == kApplicationPluginInstallAction) {
        application_plugins_install();
        return settled(settings, true);
    }
    if (target.control_id >= 100 && target.control_id < 108)
        return application_plugins_unload(static_cast<u32>(target.control_id - 100));
    if (target.control_id == kApplicationPluginDiscoverAction)
        return application_plugins_discover();
    const u64 revision = settings.catalog_revision;
    if (target.control_id == kApplicationPluginForgetAction)
        return application_plugins_catalog_forget(revision);
    if (target.control_id == kApplicationPluginPreviousAction || target.control_id == kApplicationPluginNextAction)
        return application_plugins_catalog_page(target.control_id == kApplicationPluginPreviousAction, revision);
    if (target.control_id >= kApplicationPluginCatalogActionFirst &&
        target.control_id < kApplicationPluginCatalogActionFirst + kNativePluginCatalogMaximumEntries)
        return application_plugins_catalog_enable(
            static_cast<u32>(target.control_id - kApplicationPluginCatalogActionFirst), revision);
    return false;
}
bool activate(UiSettingsTarget target, bool reverse, const NativePluginUiToken* expected = nullptr, bool picker = false,
              u32 selected = ~0u, bool save = true) noexcept {
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return false;
    UiSettingsActivation request{};
    if (!ui_settings_scene_activation(settings->scene, target, reverse, &request))
        return false;
    const bool choice_kind = request.kind == UiSettingsControlKind::choice || request.kind == UiSettingsControlKind::slider ||
                             request.kind == UiSettingsControlKind::palette || request.kind == UiSettingsControlKind::radio ||
                             option_row(request.kind);
    if (choice_kind && target.kind == UiSettingsTargetKind::control) {
        if (picker && request.kind == UiSettingsControlKind::choice)
            return open_choice(target, expected);
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
    if (target.page.owner_id != kNativeSettingsOwner)
        return activate_plugin(*settings, target, request, expected);
    if (target.page.page_id == kPluginsPage)
        return activate_plugin_catalog(*settings, target, request);
    if (request.kind == UiSettingsControlKind::category)
        return navigate_link(*settings, target.control_id);
    if (application_keybinds_settings_row(target.control_id))
        return settled(*settings, application_keybinds_settings_activate_row(target.control_id,
                                                                              request.selected_choice));
    if (application_activity_settings_row(target.control_id))
        return settled(*settings, application_activity_settings_activate_row(target.control_id));
    if (target.control_id >= kSettingsListFirst && target.control_id < kSettingsListFirst + 0x10000)
        return settled(*settings, application_account_settings_activate_row(target.control_id));
    SettingBinding binding{};
    if (!control_node(target.control_id, &binding))
        return false;
    if (bool result = false; binding != SettingBinding::game_detection &&
                             application_activity_settings_activate(binding, request.checked,
                                                                    request.selected_choice, &result)) {
        if (result)
            animate_toggle(*settings, target, request);
        return settled(*settings, result);
    }
    switch (binding) {
    case SettingBinding::customize_theme:
        return application_theme_editor_open();
    case SettingBinding::custom_builtin:
        return settled(*settings, application_custom_theme_preferences_changed(true, true));
    case SettingBinding::custom_installed:
        return settled(*settings, application_custom_theme_select_installed(request.selected_choice));
    case SettingBinding::custom_import:
    case SettingBinding::custom_preview:
    case SettingBinding::custom_apply:
    case SettingBinding::custom_cancel:
    case SettingBinding::custom_restore:
    case SettingBinding::custom_retry_read:
    case SettingBinding::custom_retry_save: {
        const auto action = binding == SettingBinding::custom_import    ? ApplicationCustomThemeAction::import_file
                            : binding == SettingBinding::custom_preview ? ApplicationCustomThemeAction::preview
                            : binding == SettingBinding::custom_apply   ? ApplicationCustomThemeAction::apply
                            : binding == SettingBinding::custom_cancel  ? ApplicationCustomThemeAction::cancel
                            : binding == SettingBinding::custom_restore ? ApplicationCustomThemeAction::restore_imported
                            : binding == SettingBinding::custom_retry_read ? ApplicationCustomThemeAction::retry_read
                                                                           : ApplicationCustomThemeAction::retry_save;
        return settled(*settings, application_custom_theme_action(action));
    }
    case SettingBinding::notification_setup:
        // Setup is a user action; showing status or entering the page never enrolls.
        application_notifications_setup(&g_app);
        return settled(*settings, true);
    case SettingBinding::windows_notification_settings: {
        const HINSTANCE opened =
            ShellExecuteW(g_app.window, L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL);
        settings->notifications_settings_failed = reinterpret_cast<usize>(opened) <= 32;
        return settled(*settings, true);
    }
    case SettingBinding::preferences_retry:
        persist();
        return settled(*settings, true);
    case SettingBinding::update_status:
    case SettingBinding::update_channel:
    case SettingBinding::update_rollback:
        return settled(*settings, application_updates_activate(binding, request.selected_choice));
    default:
        break;
    }
    if (settings_binding_backend(binding) == SettingBackend::os) {
        // Startup entry and protocol handler: the Windows registry is the value.
        const bool result = application_desktop_settings_activate(binding, request.checked);
        if (result)
            animate_toggle(*settings, target, request);
        return settled(*settings, result);
    }
    if (settings_binding_backend(binding) == SettingBackend::voice) {
        const bool result =
            application_voice_settings_activate(binding, request.checked, request.selected_choice, save);
        if (result)
            animate_toggle(*settings, target, request);
        if (save)
            return settled(*settings, result);
        project(*settings);
        return update_scene(*settings, settings->scene.viewport) && application_request_frame() && result;
    }
    if (settings_binding_backend(binding) == SettingBackend::keybinds)
        return settled(*settings, application_keybinds_settings_activate(binding));
    if (settings_binding_backend(binding) == SettingBackend::proto1 ||
        settings_binding_backend(binding) == SettingBackend::account) {
        const bool result = settings_binding_backend(binding) == SettingBackend::proto1
                                ? application_user_settings_activate(binding, request.checked, request.selected_choice)
                                : application_account_settings_activate(binding);
        if (result)
            animate_toggle(*settings, target, request);
        return settled(*settings, result);
    }
    if (settings_binding_backend(binding) != SettingBackend::local)
        return false;
    if (bool result = false; binding == SettingBinding::background_effect &&
                             application_plugins_fx_activate(request.selected_choice, &result))
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
bool choice_action(u32 action, const ApplicationSettingsChoice* captured = nullptr) noexcept {
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
    close_choice(); // No module label/descriptor survives the callback.
    if (!choice_control(choice))
        return refresh();
    const bool result =
        activate({UiSettingsTargetKind::control, choice.token.page, choice.control_id}, false,
                 choice.token.page.owner_id == kNativeSettingsOwner ? nullptr : &choice.token, false, selected);
    if (!plugin_ui_scope_current(g_app.plugin_account_scope_serial, choice.account_serial) ||
        g_app.account.status.profile_id != choice.profile_id)
        return false;
    return refresh() && result;
}
// Focus moves to the field before the edit is published, so the stale scene
// (the results page has not been laid out yet) cannot drop it.
bool search_changed(ApplicationSettings& settings) noexcept {
    settings.scene.focus = {UiSettingsTargetKind::search, {}, 0};
    apply_search(settings);
    const bool refreshed = refresh();
    ui_settings_scene_focus(&settings.scene, {UiSettingsTargetKind::search, {}, 0});
    return refreshed;
}
bool search_character(ApplicationSettings& settings, u32 code) noexcept {
    auto& model = settings.search_edit;
    if (!model.history)
        message_composer_enable_history(&model, 2048);
    message_composer_insert_utf16(&model, static_cast<u16>(code), clock_monotonic_milliseconds());
    return search_changed(settings);
}
// Editing keys of the focused search field; false when the key is not one.
bool search_key(ApplicationSettings& settings, u32 key, bool control, bool shift) noexcept {
    auto& model = settings.search_edit;
    if (!model.history)
        message_composer_enable_history(&model, 2048);
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
} // namespace

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
namespace {
// Set by WM_ACTIVATE(WA_INACTIVE), cleared on activation. A window shown
// without activation (offline probes) counts as active.
bool g_window_inactive = false;
} // namespace
void application_animation_activity_synchronize() noexcept {
    const auto now = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
    // The official client holds avatar, server icon, emoji, GIF
    // and sticker animations while its window is inactive. The Settings
    // modal keeps the shell beneath it live, animations included.
    if (g_app.preferences.reduced_motion || g_app.renderer.live_resize || g_window_inactive ||
        (g_app.window && IsIconic(g_app.window)))
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
bool application_settings_commit_preferences() noexcept {
    const bool accepted = persist();
    if (g_app.settings) {
        if (!g_app.settings->slider_preview && !release_slider_theme(*g_app.settings))
            return false;
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
            if (settings->slider_theme_baseline)
                application_custom_theme_restore(*settings->slider_theme_baseline, false);
            else
                application_custom_theme_preferences_changed(false, false);
        } else if (binding == SettingBinding::output_volume)
            g_app.preferences.output_volume = settings->slider_baseline.output_volume;
        application_preferences_apply();
        project(*settings);
        refresh();
    }
    release_slider_theme(*settings);
    close_choice();
    settings->pressed = {};
    settings->pressed_plugin = false;
    settings->slider_dragging = false;
    settings->search_selecting = false;
    settings->backdrop_pressed = false;
    ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {});
    if (g_app.window && GetCapture() == g_app.window)
        ReleaseCapture();
}
UiPageRegistry* application_settings_pages() noexcept {
    auto* settings = ensure();
    return settings ? &settings->registry : nullptr;
}
NativePluginHost* application_settings_plugin_host() noexcept {
    auto* settings = ensure();
    return settings ? &settings->plugins : nullptr;
}
NativePluginHost* application_settings_plugin_host_current() noexcept {
    return g_app.settings ? &g_app.settings->plugins : nullptr;
}
u32 application_settings_owner_bytes() noexcept {
    return sizeof(ApplicationSettings);
}
u64 application_settings_lifetime() noexcept {
    return g_app.settings_lifetime_serial;
}
bool application_settings_open_plugin_page(UiPageKey page) noexcept {
    auto* settings = g_app.settings;
    const auto* slot = settings ? native_plugin_host_find(settings->plugins, page.owner_id) : nullptr;
    if (!slot || slot->phase != NativePluginPhase::enabled || !ui_page_registry_find(settings->registry, page))
        return false;
    if (!application_settings_open())
        return false;
    set_search(*settings, {});
    if (!ui_page_registry_select(&settings->registry, page).ok()) {
        application_settings_close();
        return false;
    }
    return refresh();
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
        return application_settings_open_plugin_page(page);
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
bool application_settings_signed_out() noexcept {
    return g_signed_out.active;
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
        g_app.settings->save_failed = !result.ok();
        project(*g_app.settings);
        if (g_app.settings->scene.active)
            refresh();
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
    if (GetCapture() == g_app.window)
        ReleaseCapture();
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
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return;
    ui_scroll_host_cancel(&g_app.scroll_host);
    application_voice_settings_visible(false);
    application_settings_cancel_pointer();
    if (!release_slider_theme(*settings)) {
        log_write(LogLevel::warning, "preferences", "slider_theme_release_pending");
        return;
    }
    const auto* previous = settings->previous_focus;
    const bool composer = settings->previous_composer_focus;
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    settings->pressed = {};
    settings->notifications_active = false;
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&settings->scene);
    if (!release_plugin_page(*settings))
        log_write(LogLevel::warning, "plugins", "settings_projection_release_pending");
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
                const auto* node = ui_widget_manager_layout_node(scene.widgets, left ? scene.sidebar : scene.body);
                if (!node)
                    return false;
                const auto geometry = ui_settings_scene_scrollbar(scene, left);
                *range = {ui_rect_intersection(node->rectangle, node->clip_rectangle),
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
// Keyboard paging, track clicks and focus reveals glide on the wheel's curve
// (and jump when smooth scrolling is off or motion is reduced).
namespace {
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
} // namespace
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
        g_signed_out = {};
        ui_settings_scene_set_presentation(&settings->scene, UiSettingsPresentation::modal);
        if (!application_login_view_suspend(false))
            log_write(LogLevel::warning, "ui", "login_view_resume_failed");
    }
    if (!settings_allowed()) {
        application_settings_close(false);
        return true;
    }
    if (auto* page = settings->plugin_page; page && settings->plugin_page_committed &&
                                            settings->scene.registry == &page->registry &&
                                            settings->registry.revision == page->token.registry_revision &&
                                            page->registry.revision != page->token.registry_revision) {
        const auto* requested = ui_page_registry_selected(page->registry);
        if (requested)
            ui_page_registry_select(&settings->registry, requested->key);
    }
    const auto* selected = ui_page_registry_selected(settings->registry);
    const u32 native_page = selected && selected->key.owner_id == kNativeSettingsOwner &&
                                    selected->key.page_id <= kSettingsSearchPage
                                ? static_cast<u32>(selected->key.page_id)
                                : 0;
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
    if (settings->scene.flash_control) {
        const auto tokens = ui_settings_tokens(application_theme());
        const u64 elapsed = now - settings->flash_started_ms;
        const u64 total = tokens.duration.flash_ms, fade = minimum<u64>(tokens.duration.flash_fade_ms, total);
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
        const auto tokens = ui_settings_tokens(application_theme());
        const bool motion = g_app.window && !g_app.preferences.reduced_motion && !g_app.renderer.live_resize &&
                            (!g_app.surface_motion.system_preference_cached || g_app.surface_motion.client_area_animation);
        if (settings->entrance_opens != settings->scene.opens) {
            settings->entrance_opens = settings->scene.opens;
            settings->entrance_started_ms = motion ? now : 0;
        }
        const u64 duration = maximum<u64>(1, tokens.duration.modal_enter_ms);
        const u64 elapsed = now - settings->entrance_started_ms;
        const bool running = motion && settings->entrance_started_ms && elapsed < duration;
        renderer_set_settings_modal_progress(&g_app.renderer,
                                             running ? ui_settings_modal_entrance(tokens.duration, elapsed) : 1.f);
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
void application_settings_plugin_catalog_unbind() noexcept {
    application_settings_close(false);
    if (!g_app.settings)
        return;
    auto& settings = *g_app.settings;
    settings.pressed = {};
    settings.pressed_plugin = false;
    for (auto& control : settings.controls)
        control = {};
    settings.content[kPluginsPage - 1].controls = {};
    ++settings.content[kPluginsPage - 1].revision;
}
bool application_settings_plugin_catalog_reopen() noexcept {
    if (!application_settings_open() || !g_app.settings)
        return false;
    set_search(*g_app.settings, {});
    return ui_page_registry_select(&g_app.settings->registry, {kNativeSettingsOwner, kPluginsPage}).ok() && refresh();
}
void application_settings_release() noexcept {
    application_theme_editor_release();
    application_channel_overview_release();
    application_settings_close(false);
    auto* settings = g_app.settings;
    if (!settings)
        return;
    application_settings_cancel_pointer();
    if (!release_slider_theme(*settings)) {
        log_write(LogLevel::warning, "preferences", "slider_theme_release_pending");
        return;
    }
    if (!application_plugins_release()) {
        log_write(LogLevel::error, "plugins", "module_release_pending");
        return;
    }
    if (!native_plugin_host_disable_all(&settings->plugins).ok()) {
        log_write(LogLevel::error, "plugins", "settings_release_busy");
        return;
    }
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    ui_settings_scene_release(&settings->scene);
    application_plugin_browser_settings_released();
    if (!release_plugin_page(*settings)) {
        log_write(LogLevel::error, "plugins", "settings_projection_release_pending");
        return;
    }
    ui_page_registry_clear(&settings->registry);
    message_composer_release(&settings->search_edit);
    g_app.settings = nullptr;
    g_app.settings_held_key = 0;
    VirtualFree(settings, 0, MEM_RELEASE);
}
bool application_settings_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (application_channel_overview_input(kind, code, repeated))
        return true;
    if (g_app.settings && kind == UiInputKind::key_up && g_app.settings->preference_key == code) {
        g_app.settings->preference_key = 0;
        g_app.settings->slider_preview = false;
        application_settings_commit_preferences();
        return true;
    }
    if (ui_input_consume_key_tail(&g_app.settings_held_key, {kind, code, repeated}))
        return true;
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active) {
        if (kind == UiInputKind::key_down && !repeated &&
            shortcut_matches(ShortcutAction::open_settings, shortcut_chord_from_keyboard(code))) {
            if (application_settings_open())
                g_app.settings_held_key = code;
            return g_app.settings_held_key != 0;
        }
        return false;
    }
    if (application_voice_settings_capture(kind, code) || application_keybinds_settings_capture(kind, code))
        return settled(*settings, true);
    if (kind == UiInputKind::system_key_down && code == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return false;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return true;
    if (settings->choice.active) {
        if (!choice_popup_current(settings->choice)) {
            close_choice();
            refresh();
            return true;
        }
        if (code == VK_ESCAPE) {
            g_app.settings_held_key = code;
            close_choice();
            refresh();
            return true;
        }
        const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool changed = false;
        if (code == VK_UP || code == VK_DOWN || code == VK_TAB) {
            popup_scene_move_selection(&g_app.popup_scene, code == VK_UP || (code == VK_TAB && reverse) ? -1 : 1,
                                       &changed);
            if (changed)
                refresh();
        } else if (code == VK_PRIOR || code == VK_NEXT) {
            choice_action(code == VK_PRIOR ? kUiSettingsChoicePrevious : kUiSettingsChoiceNext);
        } else if ((code == VK_RETURN || code == VK_SPACE) && !repeated) {
            g_app.settings_held_key = code;
            choice_action(popup_scene_selected_action(g_app.popup_scene));
        } else if ((code == VK_LEFT || code == VK_RIGHT) && !repeated) {
            const auto choice = settings->choice;
            close_choice();
            g_app.settings_held_key = code;
            if (choice_control(choice))
                activate({UiSettingsTargetKind::control, choice.token.page, choice.control_id}, code == VK_LEFT,
                         choice.token.page.owner_id == kNativeSettingsOwner ? nullptr : &choice.token);
            if (plugin_ui_scope_current(g_app.plugin_account_scope_serial, choice.account_serial) &&
                choice.profile_id == g_app.account.status.profile_id)
                refresh();
        }
        return true;
    }
    const bool control_key = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto focused = ui_settings_scene_focused(settings->scene);
    const auto zone = ui_settings_scene_focus_zone(settings->scene);
    if (code == 'F' && control_key && !repeated) {
        g_app.settings_held_key = code;
        ui_settings_scene_focus(&settings->scene, {UiSettingsTargetKind::search, {}, 0});
        return refresh();
    }
    if (code == VK_ESCAPE) {
        g_app.settings_held_key = code;
        // Escape unwinds: search text, then the dialog.
        if (settings->search_bytes) {
            set_search(*settings, {});
            ui_settings_scene_focus(&settings->scene, {UiSettingsTargetKind::search, {}, 0});
            return refresh();
        }
        application_settings_close();
        return true;
    }
    if (zone == UiSettingsFocusZone::search) {
        if (search_key(*settings, code, control_key && !(GetKeyState(VK_MENU) & 0x8000), reverse))
            return true;
        if (code == VK_RETURN && !repeated && searching(*settings)) {
            g_app.settings_held_key = code;
            const auto controls = settings->scene.content.controls;
            for (u32 i = 0; i < controls.count; ++i)
                if (controls[i].kind != UiSettingsControlKind::notice) {
                    open_result(*settings, controls[i].id);
                    break;
                }
            return true;
        }
        if (code == VK_DOWN && searching(*settings)) {
            ui_settings_scene_move_zone(&settings->scene, false);
            ui_settings_scene_move_zone(&settings->scene, false);
            refresh();
            return true;
        }
    }
    const auto* focused_control = ui_settings_scene_control(settings->scene, focused);
    if (focused_control && (focused_control->kind == UiSettingsControlKind::slider ||
                            focused_control->kind == UiSettingsControlKind::palette ||
                            focused_control->kind == UiSettingsControlKind::radio)) {
        const bool decrease = code == VK_LEFT, increase = code == VK_RIGHT;
        // Page Up/Down move a slider by a tenth of its range.
        const bool page = focused_control->kind == UiSettingsControlKind::slider && (code == VK_PRIOR || code == VK_NEXT);
        if (code == VK_HOME || code == VK_END || decrease || increase || page) {
            UiSettingsActivation activation{};
            const auto direction = code == VK_HOME   ? UiSettingsSliderKey::home
                                   : code == VK_END   ? UiSettingsSliderKey::end
                                   : code == VK_PRIOR ? UiSettingsSliderKey::page_increase
                                   : code == VK_NEXT  ? UiSettingsSliderKey::page_decrease
                                   : decrease         ? UiSettingsSliderKey::decrease
                                                      : UiSettingsSliderKey::increase;
            if (ui_settings_scene_slider_key(settings->scene, focused, direction, &activation) &&
                activation.selected_choice != focused_control->selected_choice) {
                const bool preview = focused_control->kind == UiSettingsControlKind::slider &&
                                     focused.page.owner_id == kNativeSettingsOwner;
                if (preview && !settings->slider_preview) {
                    settings->slider_baseline = g_app.preferences;
                    if (!capture_slider_theme(*settings, focused))
                        return true;
                    settings->slider_target = focused;
                    settings->pressed_account_serial = g_app.plugin_account_scope_serial;
                    settings->pressed_profile = g_app.account.status.profile_id;
                    settings->slider_preview = true;
                }
                if (preview)
                    settings->preference_key = code;
                activate(focused, false, nullptr, false, activation.selected_choice, !preview);
            }
            return true;
        }
        if (code == VK_RETURN || code == VK_SPACE) {
            g_app.settings_held_key = code;
            if (code == VK_RETURN && searching(*settings) && !repeated)
                open_result(*settings, focused.control_id);
            return true;
        }
    }
    if (code == VK_TAB) {
        ui_settings_scene_move_zone(&settings->scene, reverse);
        refresh();
        return true;
    }
    if (code == VK_UP || code == VK_DOWN) {
        const float before = settings->scene.content_offset;
        if (!ui_settings_scene_move_within(&settings->scene, code == VK_UP) && zone == UiSettingsFocusZone::none)
            ui_settings_scene_move_zone(&settings->scene, code == VK_UP);
        settings_glide_reveal(before);
        refresh();
        return true;
    }
    if ((code == VK_HOME || code == VK_END) && zone != UiSettingsFocusZone::search) {
        const float before = settings->scene.content_offset;
        u32 steps = 0;
        while (steps < 64 && ui_settings_scene_move_within(&settings->scene, code == VK_HOME))
            ++steps;
        settings_glide_reveal(before);
        refresh();
        return true;
    }
    if ((code == VK_RETURN || code == VK_SPACE || code == VK_LEFT || code == VK_RIGHT) && !repeated) {
        if (zone == UiSettingsFocusZone::search)
            return true;
        g_app.settings_held_key = code;
        if (code == VK_RETURN && searching(*settings) && focused.kind == UiSettingsTargetKind::control) {
            open_result(*settings, focused.control_id);
            return true;
        }
        if ((code == VK_LEFT || code == VK_RIGHT) && zone != UiSettingsFocusZone::panel)
            return true;
        activate(focused, code == VK_LEFT || reverse, nullptr, code == VK_RETURN || code == VK_SPACE);
        return true;
    }
    if (code == VK_PRIOR || code == VK_NEXT) {
        settings_glide_to(false, settings->scene.content_offset +
                                     (code == VK_PRIOR ? -1.f : 1.f) * ui_rect_height(settings->scene.viewport) * .8f);
        refresh();
    }
    return true;
}
bool application_settings_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (application_channel_overview_window_message(message, wparam, lparam))
        return true;
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return false;
    if (message == WM_XBUTTONDOWN || message == WM_MBUTTONDOWN) {
        const u32 button = message == WM_MBUTTONDOWN               ? VK_MBUTTON
                           : GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? VK_XBUTTON1
                                                                    : VK_XBUTTON2;
        if (application_voice_settings_capture(UiInputKind::key_down, button) ||
            application_keybinds_settings_capture(UiInputKind::key_down, button))
            return settled(*settings, true);
    }
    if (message == WM_CHAR) {
        const u32 character = static_cast<u32>(wparam);
        if (character < 0x20 || character == 0x7f || character > 0xffff || settings->choice.active)
            return true;
        const auto zone = ui_settings_scene_focus_zone(settings->scene);
        // Typing from the sidebar jumps to search; panel rows keep Space.
        if (zone == UiSettingsFocusZone::search || zone == UiSettingsFocusZone::sidebar ||
            zone == UiSettingsFocusZone::none)
            search_character(*settings, character);
        return true;
    }
    // Native action controls receive a second press as DBLCLK, not DOWN.
    // Inline text editors are routed above this settings-only controller.
    const bool double_click = message == WM_LBUTTONDBLCLK;
    if (double_click)
        message = WM_LBUTTONDOWN;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS || message == WM_SIZE ||
        message == WM_DPICHANGED) {
        const bool was_choice = settings->choice.active;
        application_settings_cancel_pointer();
        settings->scrollbar_dragging = false;
        if (was_choice)
            refresh();
        else
            application_request_frame();
        return false;
    }
    if (message == WM_MOUSELEAVE) {
        if (settings->choice.active) {
            bool changed = false;
            popup_scene_set_hover(&g_app.popup_scene, -1, -1, &changed);
            if (changed)
                refresh();
        }
        const auto pointer = ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::move, {-1, -1});
        if (pointer.changed)
            application_request_frame();
        return true;
    }
    if (message == WM_MOUSEWHEEL) {
        if (settings->choice.active) {
            choice_action(static_cast<short>(HIWORD(wparam)) > 0 ? kUiSettingsChoicePrevious : kUiSettingsChoiceNext);
            return true;
        }
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        if (!application_screen_to_content(&point))
            return true;
        const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
        const bool left = ui_rect_contains(settings->scene.layout.sidebar, point.x * scale, point.y * scale);
        application_modal_scroll_wheel(&g_app.scroll_host, application_settings_scroll_binding(left), g_app.window,
                                       g_app.renderer.dpi, wparam, lparam,
                                       clock_microseconds(g_app.clock, g_app.process_start, clock_now()));
        refresh();
        return true;
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        const float x = client_dip_x(lparam), y = client_dip_y(lparam);
        if (settings->slider_dragging) {
            const auto target = settings->pressed;
            const auto* control = ui_settings_scene_control(settings->scene, target);
            const bool current = control && control->kind == UiSettingsControlKind::slider &&
                                 target.page.owner_id == kNativeSettingsOwner &&
                                 settings->pressed_account_serial == g_app.plugin_account_scope_serial &&
                                 settings->pressed_profile == g_app.account.status.profile_id &&
                                 settings->slider_generation == settings->scene.widgets.generation &&
                                 ui_rect_equal(settings->slider_viewport, settings->scene.viewport) &&
                                 GetCapture() == g_app.window;
            if (!current) {
                application_settings_cancel_pointer();
                return true;
            }
            if (message == WM_MOUSEMOVE) {
                UiSettingsActivation value{};
                if (ui_settings_scene_slider_pointer(settings->scene, target, x - settings->slider_grab, &value) &&
                    value.selected_choice != control->selected_choice)
                    activate(target, false, nullptr, false, value.selected_choice, false);
            } else if (message == WM_LBUTTONUP) {
                settings->slider_preview = false;
                application_settings_cancel_pointer();
                application_settings_commit_preferences();
            }
            return true;
        }
        if (settings->choice.active) {
            if (!choice_popup_current(settings->choice)) {
                close_choice();
                refresh();
                return true;
            }
            const auto hit = popup_scene_hit_test(g_app.popup_scene, x, y);
            if (message == WM_MOUSEMOVE) {
                bool changed = false;
                popup_scene_set_hover(&g_app.popup_scene, x, y, &changed);
                if (changed)
                    refresh();
            } else if (message == WM_LBUTTONDOWN) {
                if (!hit.inside) {
                    close_choice();
                    refresh();
                    return true;
                }
                const auto choice = settings->choice;
                SetFocus(g_app.window);
                if (!choice_popup_current(choice))
                    return true;
                SetCapture(g_app.window);
                if (!choice_popup_current(choice))
                    return true;
                g_app.settings->choice.pressed_action = hit.enabled && GetCapture() == g_app.window ? hit.action : 0;
            } else {
                auto choice = settings->choice;
                settings->choice.pressed_action = 0;
                const bool accepted = hit.enabled && hit.action == choice.pressed_action;
                close_choice();
                choice.popup_generation = g_app.popup_scene.widgets.generation;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
                // Capture cancellation may run nested UI. Both the closed bank
                // and copied control token must remain current before dispatch.
                if (accepted)
                    choice_action(hit.action, &choice);
            }
            return true;
        }
        // Like the official client's modals, a click on the scrim (press and release
        // outside the dialog) closes Settings.
        const auto backdrop = [&]() noexcept {
            const auto& scene = settings->scene;
            return scene.presentation == UiSettingsPresentation::modal && !scene.layout.compact &&
                   ui_rect_contains(scene.viewport, x, y) && !ui_rect_contains(scene.layout.modal, x, y);
        };
        if (settings->backdrop_pressed) {
            if (message == WM_LBUTTONUP) {
                settings->backdrop_pressed = false;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
                if (backdrop())
                    application_settings_close();
            }
            return true;
        }
        if (message == WM_LBUTTONDOWN && backdrop()) {
            ui_scroll_host_cancel(&g_app.scroll_host);
            settings->backdrop_pressed = true;
            SetCapture(g_app.window);
            return true;
        }
        if (settings->search_selecting) {
            if (message == WM_MOUSEMOVE)
                search_point(*settings, x, y, true);
            else if (message == WM_LBUTTONUP) {
                settings->search_selecting = false;
                settings->pressed = {};
                ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {});
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
            }
            return true;
        }
        if (settings->scrollbar_dragging) {
            if (message == WM_MOUSEMOVE) {
                const auto geometry = ui_settings_scene_scrollbar(settings->scene, settings->scrollbar_sidebar);
                const float offset =
                    settings->scrollbar_sidebar ? settings->scene.sidebar_offset : settings->scene.content_offset;
                const float desired = ui_scrollbar_scroll_from_pointer(geometry, y, settings->scrollbar_grab);
                ui_settings_scene_scroll(&settings->scene, settings->scrollbar_sidebar, desired - offset);
                refresh();
            } else if (message == WM_LBUTTONUP) {
                settings->scrollbar_dragging = false;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
            }
            return true;
        }
        if (message == WM_LBUTTONDOWN)
            for (u32 i = 0; i < 2; ++i) {
                const bool sidebar = i == 0;
                const auto geometry = ui_settings_scene_scrollbar(settings->scene, sidebar);
                if (!ui_scrollbar_track_hit(geometry, x, y))
                    continue;
                ui_scroll_host_cancel(&g_app.scroll_host);
                if (ui_scrollbar_thumb_hit(geometry, x, y)) {
                    settings->scrollbar_dragging = true;
                    settings->scrollbar_sidebar = sidebar;
                    settings->scrollbar_grab = y - geometry.thumb.top;
                    SetCapture(g_app.window);
                    if (GetCapture() != g_app.window)
                        settings->scrollbar_dragging = false;
                } else {
                    settings_glide_to(sidebar, (sidebar ? settings->scene.sidebar_offset
                                                        : settings->scene.content_offset) +
                                                   (y < geometry.thumb.top ? -1.f : 1.f) *
                                                       ui_rect_height(geometry.track));
                    refresh();
                }
                return true;
            }
        const auto hit = ui_settings_scene_hit_test(settings->scene, x, y);
        SettingBinding hit_binding{};
        if (hit.kind == UiSettingsTargetKind::control && hit.page.owner_id == kNativeSettingsOwner)
            control_node(hit.control_id, &hit_binding);
        // Preview replaces its button with Apply at the same position. The
        // second click of Preview's double-click must not commit that new
        // action; a fresh press/release still applies.
        if (double_click && hit_binding == SettingBinding::custom_apply) {
            application_settings_cancel_pointer();
            return true;
        }
        const auto* control = ui_settings_scene_control(settings->scene, hit);
        if (message == WM_LBUTTONDOWN && control && control->kind == UiSettingsControlKind::slider &&
            !ui_settings_scene_slider_hit(settings->scene, hit, x, y))
            return true;
        if (message == WM_MOUSEMOVE && control &&
            (control->kind == UiSettingsControlKind::palette || control->kind == UiSettingsControlKind::radio) &&
            ui_settings_scene_palette_hover(&settings->scene, hit, x, y))
            application_request_frame();
        const bool track_leave = !settings->scene.widgets.hovered.generation;
        const auto pointer = ui_button_pointer(&settings->scene.widgets,
                                               message == WM_LBUTTONDOWN ? UiButtonPointerEvent::press
                                               : message == WM_LBUTTONUP ? UiButtonPointerEvent::release
                                                                         : UiButtonPointerEvent::move,
                                               {x, y});
        if (pointer.changed)
            application_request_frame();
        if (message == WM_MOUSEMOVE && track_leave && pointer.target.generation) {
            TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, g_app.window, 0};
            TrackMouseEvent(&tracking);
        }
        if (message == WM_LBUTTONDOWN) {
            const u64 account_serial = g_app.plugin_account_scope_serial, profile = g_app.account.status.profile_id;
            const u64 catalog_revision = application_plugins_catalog_revision();
            settings->pressed = hit;
            ui_settings_scene_focus(&settings->scene, hit);
            SetFocus(g_app.window);
            SetCapture(g_app.window);
            refresh();
            if (g_app.settings != settings || !settings->scene.active ||
                account_serial != g_app.plugin_account_scope_serial || profile != g_app.account.status.profile_id ||
                catalog_revision != application_plugins_catalog_revision())
                return true;
            settings->pressed_revision = settings->scene.revision;
            settings->pressed_catalog_revision = catalog_revision;
            // A press in the search field places the caret and may drag a selection.
            if (hit.kind == UiSettingsTargetKind::search && GetCapture() == g_app.window) {
                settings->search_selecting = true;
                search_point(*settings, x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                return true;
            }
            settings->pressed_account_serial = account_serial;
            settings->pressed_profile = profile;
            if (control && control->kind == UiSettingsControlKind::slider && hit.page.owner_id == kNativeSettingsOwner &&
                GetCapture() == g_app.window) {
                settings->slider_dragging = true;
                settings->slider_baseline = g_app.preferences;
                if (!capture_slider_theme(*settings, hit)) {
                    application_settings_cancel_pointer();
                    return true;
                }
                settings->slider_target = hit;
                settings->slider_preview = true;
                settings->slider_generation = settings->scene.widgets.generation;
                settings->slider_viewport = settings->scene.viewport;
                UiSettingsSliderGeometry slider{};
                settings->slider_grab = 0;
                if (ui_settings_scene_slider_geometry(settings->scene, hit, &slider))
                    settings->slider_grab = ui_slider_grab_offset(slider.thumb, x, y);
                UiSettingsActivation value{};
                if (ui_settings_scene_slider_pointer(settings->scene, hit, x - settings->slider_grab, &value))
                    activate(hit, false, nullptr, false, value.selected_choice, false);
            }
            settings->pressed_plugin =
                hit.kind == UiSettingsTargetKind::control && hit.page.owner_id != kNativeSettingsOwner;
            if (settings->pressed_plugin &&
                !native_plugin_host_settings_token(&settings->plugins, hit.page, &settings->pressed_token).ok())
                settings->pressed = {};
            if (GetCapture() != g_app.window) {
                settings->pressed = {};
                if (ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {}).changed)
                    application_request_frame();
            }
        } else if (message == WM_LBUTTONUP) {
            const auto pressed = settings->pressed;
            const auto token = settings->pressed_token;
            const bool plugin = settings->pressed_plugin;
            const u32 revision = settings->pressed_revision;
            const u64 catalog_revision = settings->pressed_catalog_revision;
            const u64 serial = settings->pressed_account_serial, profile = settings->pressed_profile;
            settings->pressed = {};
            settings->pressed_plugin = false;
            if (GetCapture() == g_app.window)
                ReleaseCapture();
            if (g_app.settings == settings && settings->scene.active && serial == g_app.plugin_account_scope_serial &&
                profile == g_app.account.status.profile_id &&
                catalog_revision == application_plugins_catalog_revision() && pointer.activate &&
                ui_settings_target_equal(hit, pressed) && revision == settings->scene.revision) {
                UiSettingsActivation value{};
                if (control && control->kind == UiSettingsControlKind::palette) {
                    if (ui_settings_scene_palette_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, plugin ? &token : nullptr, false, value.selected_choice);
                } else if (control && control->kind == UiSettingsControlKind::radio) {
                    if (ui_settings_scene_radio_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, plugin ? &token : nullptr, false, value.selected_choice);
                } else if (control && option_row(control->kind)) {
                    // Tabs, segments, swatches and button groups take the
                    // option under the pointer; a click between them does nothing.
                    if (ui_settings_scene_option_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, plugin ? &token : nullptr, false, value.selected_choice);
                } else if (plugin && control && control->kind == UiSettingsControlKind::slider) {
                    // Plugin sliders take the clicked step; dragging stays native-only.
                    if (ui_settings_scene_slider_pointer(settings->scene, hit, x, &value))
                        activate(hit, false, &token, false, value.selected_choice);
                } else if (control && (control->kind == UiSettingsControlKind::action ||
                                       control->kind == UiSettingsControlKind::notice)) {
                    if (ui_settings_scene_button_hit(settings->scene, hit, x, y))
                        activate(hit, false, plugin ? &token : nullptr, true);
                } else
                    activate(hit, false, plugin ? &token : nullptr, true);
            }
        }
        return true;
    }
    if (message == WM_DROPFILES) {
        DragFinish(reinterpret_cast<HDROP>(wparam));
        return true;
    }
    return message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
           message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MOUSEHWHEEL ||
           message == WM_LBUTTONDBLCLK;
}
} // namespace maicord::app_detail

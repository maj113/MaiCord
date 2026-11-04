#include "settings_internal.hpp"
#include "../effects/effects.hpp"
#include "../account/activity.hpp"
#include "../account/login/ui.hpp"
#include "../shell/ui_runtime.hpp"
#include "../account/account_presentation.hpp"
#include "../account/application_account_logout.hpp"
#include "../notifications/notifications.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../update/updates.hpp"
#include "maicord/ui/widgets/kit.hpp"
#include "../store/store_host.hpp"
#include "user_settings.hpp"
#include "account_settings.hpp"
#include "keybinds_settings.hpp"
#include "../notifications/desktop.hpp"
#include "maicord/app/account/login/oauth_account.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/ui/guild/icon_media.hpp"
#include "../shell/content_point.hpp"

namespace maicord::app_detail {

static_assert(sizeof(ApplicationSettingsChoice) == 88);

inline constexpr u32 kSettingsPanelRowLimit = kKeybindsSettingsBankCapacity;
static_assert(kSettingsPanelRowLimit >= kSettingsControlCapacity && kSettingsPanelRowLimit >= kApplicationUpdatesSettingsRows);
inline constexpr u32 kSettingsSearchHits = 16;

// The bank is allocated lazily; nothing is allocated at application startup.
static_assert(sizeof(ApplicationSettings) <= 4 * 4096);

namespace settings_detail {

MediaAccount g_spotify_account{};

Utf8View saved_account_message() noexcept {
    return utf8_literal("Sign-in saved on this device. Spotify playback is limited to previews.");
}

static void media_saved_state() noexcept {
    auto& account = g_spotify_account;
    if (account.checked) return;
    account.client_bytes = oauth_account_client(provider, {account.client, sizeof(account.client)});
    SecureBuffer refresh{};
    account.saved = oauth_account_load(account.client_id(), &refresh, provider);
    secure_buffer_release(&refresh);
    account.checked = true;
    if (account.saved) account.message = saved_account_message();
}

void media_close(MediaAccount& account) noexcept {
    account.stopping = !oauth_sign_in_stop(&account.sign_in, 0);
}
bool media_account_action() noexcept {
    media_saved_state();
    auto& account = g_spotify_account;
    if (account.saved) {
        if (!oauth_account_forget(provider)) {
            account.message = utf8_literal("Could not remove the saved sign-in. Try again.");
            return false;
        }
        account.saved = false;
        account.message = utf8_literal("Signed out on this device.");
        return true;
    }
    if (account.sign_in.runtime) {
        media_close(account);
        account.message = utf8_literal("Sign-in cancelled.");
        return true;
    }
    account.client_bytes = oauth_account_client(provider, {account.client, sizeof(account.client)});
    if (!account.client_bytes) {
        account.message = utf8_literal("Configure MaiCord's Spotify client registration before signing in.");
        return false;
    }
    if (!g_app.window) return false;
    account.launched = false;
    account.stopping = false;
    const bool started = oauth_sign_in_start(&account.sign_in, account.client_id(), ++account.generation, 300000, {},
        provider);
    account.message = started ? utf8_literal("Opening sign-in in your browser...") : utf8_literal("Could not start sign-in. Try again.");
    return started;
}

SignedOutSession g_signed_out;

bool settings_allowed() noexcept {
    return g_app.account_ui && (!g_app.login.visible || g_signed_out.active);
}

bool page_allowed(const UiPageKey& page) noexcept {
    return !g_signed_out.active ||
           (page.owner_id == kSettingsNativeOwner && page.page_id >= 1 &&
            page.page_id <= kSettingsPanelCount &&
            settings_panel_signed_out(static_cast<SettingsPanel>(page.page_id - 1)));
}

static_assert(kUiSettingsNoDefault == 0xffff, "UiSettingsSchemaNode stores no default as 0xffff");
static SettingBinding binding_of(const UiSettingsSchemaNode& node) noexcept {
    return node.binding_slot < static_cast<u32>(SettingBinding::count) ? static_cast<SettingBinding>(node.binding_slot)
                                                                       : SettingBinding::none;
}

bool theme_panel(const UiPageKey& page) noexcept {
    return page.owner_id == kNativeSettingsOwner && (page.page_id == static_cast<u64>(SettingsPanel::appearance) + 1 ||
                                                     page.page_id == static_cast<u64>(SettingsPanel::themes) + 1);
}
u32 installed_theme_at(u32 tile) noexcept {
    const u32 count = application_custom_theme_installed().count;
    const u32 active = application_custom_theme_installed_active();
    return count > kInstalledThemeTiles && active < count && active >= kInstalledThemeTiles &&
                   tile == kInstalledThemeTiles - 1
               ? active
               : tile;
}
bool theme_binding(SettingBinding binding) noexcept {
    return binding == SettingBinding::theme_base || binding == SettingBinding::theme_color ||
           binding == SettingBinding::background_effect || binding == SettingBinding::gradient_direction ||
           binding == SettingBinding::gradient_intensity;
}
const UiSettingsSchemaNode* control_node(u64 id, SettingBinding* binding) noexcept {
    UiSettingsSchemaLocation location{};
    const auto* node = settings_control_location(id, &location) ? settings_catalog_node(location) : nullptr;
    if (binding)
        *binding = node ? binding_of(*node) : SettingBinding::none;
    return node;
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

static bool same_control(const UiSettingsControl& a, const UiSettingsControl& b) noexcept {
    return a.id == b.id && a.kind == b.kind && a.enabled == b.enabled && a.checked == b.checked &&
           a.choices.data == b.choices.data && a.choices.count == b.choices.count &&
           a.selected_choice == b.selected_choice && a.editor == b.editor && utf8_equal(a.label, b.label) &&
           utf8_equal(a.description, b.description) && utf8_equal(a.action_label, b.action_label) &&
           utf8_equal(a.detail, b.detail);
}
static bool same_presentation(const UiSettingsControlPresentation& a, const UiSettingsControlPresentation& b) noexcept {
    return a.variant == b.variant && a.flags == b.flags && a.default_choice == b.default_choice &&
           a.chip_tones == b.chip_tones && a.icon_plus_one == b.icon_plus_one && a.more == b.more;
}
static Utf8View search_query(const ApplicationSettings& settings) noexcept {
    return {settings.search_text, settings.search_bytes};
}
// The "Your themes" tile captions: installed theme names (borrowed from the installed list, which only changes
// with a new theme revision and so a new projection), then "Browse" (opens Themes; a swatch caption fits one word).
static Utf8View g_theme_tiles[kInstalledThemeTiles + 1];

// Live projection of one catalog node. Returns false when the node has no
// backing in the current state and must be omitted.
static bool project_node(const ApplicationSettings& settings, u32 panel, u32 index, UiSettingsControl* control,
                         UiSettingsControlPresentation* presentation) noexcept {
    const auto& node = settings_catalog().panels[panel].nodes[index];
    const auto binding = binding_of(node);
    const auto custom = application_custom_theme_view();
    const bool theme_idle = !custom.busy && !custom.previewing;
    *control = {settings_control_id(panel, index), node.label(), node.description(), node.kind, true, false,
                node.choices(), 0, node.action_label(), nullptr};
    *presentation = {static_cast<UiSettingsVariant>(node.variant),
                     static_cast<u8>(node.flags & ui_settings_schema_marks ? ui_settings_presentation_marks : 0),
                     node.default_choice_slot}; // 0xffff is kUiSettingsNoDefault
    if (binding == SettingBinding::spotify_account) {
        media_saved_state();
        const auto& account = g_spotify_account;
        if (account.message.bytes) control->description = account.message;
        control->action_label = account.saved ? utf8_literal("Sign out")
                                : account.sign_in.runtime ? utf8_literal("Cancel") : utf8_literal("Sign in");
    }
    if (binding == SettingBinding::update_changelog)
        return application_updates_project(binding, control, presentation);
    if (node.kind == UiSettingsControlKind::category)
        return true;
    if (application_desktop_settings_project(binding, control))
        return true;
    if (binding != SettingBinding::game_detection && application_activity_settings_project(binding, control))
        return true;
    const auto backend = settings_binding_backend(binding);
    if (backend == SettingBackend::voice)
        return application_voice_settings_project(binding, control, presentation);
    if (backend == SettingBackend::proto1)
        return application_user_settings_project(binding, control, presentation);
    if (backend == SettingBackend::account)
        return application_account_settings_project(binding, control, presentation);
    if (backend == SettingBackend::keybinds)
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
    // Installed package effects follow the built-in effects.
    if (binding == SettingBinding::background_effect)
        application_effects_project(control);
    switch (binding) {
    case SettingBinding::theme_base:
    case SettingBinding::theme_color:
        // A custom theme only borrows its base: no built-in tile is the current theme.
        if (custom.custom_active)
            control->selected_choice = kUiSettingsNoChoice;
        control->enabled = theme_idle;
        return true;
    case SettingBinding::background_effect:
    case SettingBinding::gradient_direction:
    case SettingBinding::gradient_intensity:
        // These edit the built-in gradient. An installed theme is edited with Customize, which keeps its own look.
        control->enabled = theme_idle && !custom.custom_active;
        if (custom.custom_active && binding == SettingBinding::background_effect) {
            static char hint[128 + kClientThemeNameMaximumBytes];
            constexpr Utf8View head = utf8_literal("Gradient settings apply to built-in themes. Use Customize to edit ");
            const u32 name = minimum<u32>(custom.current.bytes, kClientThemeNameMaximumBytes);
            copy_bytes(hint, head.data, head.bytes);
            copy_bytes(hint + head.bytes, custom.current.data, name);
            hint[head.bytes + name] = '.';
            control->description = {hint, head.bytes + name + 1};
        }
        return true;
    case SettingBinding::customize_theme:
        control->enabled = theme_idle;
        return true;
    case SettingBinding::browse_effects:
    case SettingBinding::import_theme:
        return true;
    case SettingBinding::installed_themes: {
        // Installed and imported themes, then the tile that opens Themes; the active one is checked.
        const auto installed = application_custom_theme_installed();
        const u32 active = application_custom_theme_installed_active();
        const u32 shown = minimum(installed.count, kInstalledThemeTiles);
        control->selected_choice = kUiSettingsNoChoice;
        for (u32 i = 0; i < shown; ++i) {
            const u32 at = installed_theme_at(i);
            g_theme_tiles[i] = {installed[at].name, installed[at].name_bytes};
            if (at == active)
                control->selected_choice = i;
        }
        g_theme_tiles[shown] = utf8_literal("Browse");
        control->choices = {g_theme_tiles, shown + 1};
        control->enabled = theme_idle;
        // The row icon marks the last tile as the Browse tile: the scene draws it as an arrow, not a theme.
        presentation->icon_plus_one = static_cast<u8>(UiIcon::arrow_right) + 1;
        return true;
    }
    case SettingBinding::notification_setup:
        control->description = application_notifications_status(&g_app);
        return true;
    case SettingBinding::windows_notification_settings:
        if (settings.notifications_settings_failed)
            control->description = utf8_literal("Windows notification settings couldn't open. Try again.");
        return true;
    case SettingBinding::preferences_retry:
        control->description = g_app.preferences_save_pending ? utf8_literal("Saving…") : node.description();
        control->enabled = !g_app.preferences_save_pending;
        return settings.save_failed;
    case SettingBinding::update_status:
    case SettingBinding::update_channel:
    case SettingBinding::update_interval:
    case SettingBinding::update_notice:
    case SettingBinding::update_release_notes:
        return application_updates_project(binding, control, presentation);
    default:
        return true;
    }
}

static UiSettingsAvatarMedia self_avatar_media() noexcept {
    return g_app.account_ui ? application_user_avatar_media(g_app.account_ui->chat_state.self_user_id)
                            : UiSettingsAvatarMedia{};
}
static bool same_avatar_media(const UiSettingsAvatarMedia& left, const UiSettingsAvatarMedia& right) noexcept {
    return media_asset_key_equal(left.key, right.key) && left.hash_bytes == right.hash_bytes &&
           bytes_equal(left.hash, right.hash, left.hash_bytes);
}

static u32 project_panel(ApplicationSettings& settings, u32 panel, bool* changed, UiSettingsControl* bank,
                         UiSettingsControlPresentation* bank_presentation, u32 capacity) noexcept {
    // Staging rows (several KiB) on the UI thread, not re-entered; on the stack each projection
    // walked its guard pages. Value-initialized per call, as the arrays were.
    static UiSettingsControl controls[kSettingsPanelRowLimit];
    static UiSettingsControlPresentation presentation[kSettingsPanelRowLimit];
    for (u32 i = 0; i < capacity; ++i) {
        reset_value(&controls[i]);
        reset_value(&presentation[i]);
    }
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
            if (binding == SettingBinding::update_changelog)
                count += application_updates_settings_list({controls + count, capacity - count},
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

static u32 project_search(ApplicationSettings& settings, bool* changed) noexcept {
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

// The selected page's id when it is one of the native pages (panels, then search results), else zero.
u32 native_page_of(const ApplicationSettings& settings) noexcept {
    const auto* selected = ui_page_registry_selected(settings.registry);
    return selected && selected->key.owner_id == kNativeSettingsOwner && selected->key.page_id >= 1 &&
                   selected->key.page_id <= kSettingsSearchPage
               ? static_cast<u32>(selected->key.page_id)
               : 0;
}

void project(ApplicationSettings& settings) noexcept {
    bool changed[kSettingsSearchPage]{};
    const auto custom = application_custom_theme_view();
    if (settings.custom_theme_revision != custom.revision) {
        settings.custom_theme_revision = custom.revision;
        changed[static_cast<u32>(SettingsPanel::appearance)] = true;
    }
    const u32 native = native_page_of(settings);
    if (settings.native_page != native) {
        // Release the previous borrow before building the next native page.
        for (u32 i = 0; i < kSettingsSearchPage; ++i) {
            if (settings.content[i].controls.count)
                changed[i] = true;
            reset_value(&settings.content[i].controls);
            settings.content[i].presentation = nullptr;
            settings.content[i].meters = nullptr;
            settings.content[i].avatar_media = nullptr;
            settings.content[i].list_items = nullptr;
        }
        settings.native_page = native;
        application_voice_settings_visible(native == static_cast<u32>(SettingsPanel::voice) + 1);
        application_updates_visible(native == static_cast<u32>(SettingsPanel::updates) + 1);
        application_keybinds_settings_visible(native == static_cast<u32>(SettingsPanel::keybinds) + 1);
        application_account_settings_visible(native >= 1 && native <= kSettingsPanelCount
                                                 ? static_cast<SettingsPanel>(native - 1)
                                                 : SettingsPanel::count);
    }
    auto& appearance = settings.content[static_cast<u32>(SettingsPanel::appearance)];
    // The page's one-line subtitle; theme import and save feedback lives on the Themes page.
    const Utf8View summary = settings_catalog_panel(SettingsPanel::appearance).summary;
    if (!utf8_equal(appearance.description, summary)) {
        appearance.description = summary;
        changed[static_cast<u32>(SettingsPanel::appearance)] = true;
    }
    CatalogKind store_kind{};
    if (application_store_kind(native, &store_kind)) {
        // A host page: the store scene draws its own column in the rectangle the settings scene gives it.
    } else if (native == static_cast<u32>(SettingsPanel::updates) + 1 && application_updates_settings_bank()) {
        // Updates borrows its own bank for the changelog rows and the status
        // card's progress meter.
        auto& page = changed[native - 1];
        auto& content = settings.content[native - 1];
        auto& bank = *application_updates_settings_bank();
        const u32 count = project_panel(settings, native - 1, &page, bank.controls, bank.presentation,
                                        kApplicationUpdatesSettingsRows);
        UiSettingsSchemaLocation location{};
        const u64 status = settings_catalog_binding(SettingBinding::update_status, &location)
                               ? settings_control_id(location.panel, location.node)
                               : 0;
        for (u32 i = 0; i < count; ++i)
            bank.meters[i] = bank.controls[i].id == status ? application_updates_meter() : kUiSettingsNoMeter;
        page = page || content.controls.count != count || content.controls.data != bank.controls;
        content.controls = {bank.controls, count};
        content.presentation = bank.presentation;
        content.list_items = nullptr;
        content.meters = bank.meters;
        content.avatar_media = nullptr;
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

} // namespace settings_detail

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

UiSettingsAvatarMedia application_listed_user_avatar_media(u64 user_id, const Utf8View& avatar_hash,
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
    static constexpr char prefix[] = "embed/avatars/";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    const u32 index = discriminator ? discriminator % 5u : static_cast<u32>((user_id >> 22) % 6u);
    copy_bytes(media.hash, prefix, prefix_bytes);
    media.hash[prefix_bytes] = static_cast<char>('0' + index);
    copy_bytes(media.hash + prefix_bytes + 1, ".png", 4);
    media.hash_bytes = static_cast<u8>(prefix_bytes + 5);
    char url[64];
    static constexpr char host[] = "https://cdn.discordapp.com/";
    copy_bytes(url, host, sizeof(host) - 1);
    copy_bytes(url + sizeof(host) - 1, media.hash, media.hash_bytes);
    media.key = media_asset_embed(hash64_bytes(kHash64Seed, url, sizeof(host) - 1 + media.hash_bytes), 1, 0,
                                  MediaEmbedResource::image, MediaAttachmentFormat::png, false,
                                  MediaResolutionTier::px512);
    media.key.kind = MediaAssetKind::profile_image;
    return media;
}

namespace settings_detail {

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
    // Settings enters over the sign-in screen, not the empty shell behind it.
    if (!renderer_freeze_login_backdrop(&g_app.renderer, application_theme()))
        log_write(LogLevel::warning, "ui", "login_backdrop_unavailable");
    if (!application_login_view_suspend(true)) {
        renderer_release_settings_backdrop(&g_app.renderer);
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
    // An open that failed after the sign-in screen was frozen must not lend
    // it to a later dialog.
    if (g_app.renderer.settings_backdrop_frozen && !g_app.renderer.settings_scene)
        renderer_release_settings_backdrop(&g_app.renderer);
}

void finish_signed_out() noexcept {
    if (!g_signed_out.ending)
        return;
    const bool release = g_signed_out.created_runtime && !g_app.account.instance && g_app.login.visible;
    reset_value(&g_signed_out);
    if (release)
        application_release_account_ui(g_app);
    if (!application_login_view_suspend(false))
        log_write(LogLevel::warning, "ui", "login_view_resume_failed");
}

ApplicationSettings* ensure() noexcept {
    if (g_app.settings)
        return g_app.settings;
    auto* settings = static_cast<ApplicationSettings*>(
        commit_pages(sizeof(ApplicationSettings)));
    if (!settings)
        return nullptr;
    if (!ui_page_registry_initialize(&settings->registry, {settings->pages, kSettingsPageCapacity})) {
        pages_release(settings);
        return nullptr;
    }
    const auto& schema = settings_catalog();
    for (u32 i = 0; i < kSettingsSearchPage; ++i) {
        const Utf8View label = i < kSettingsPanelCount ? schema.panels[i].title : utf8_literal("Search results");
        const bool store = i == kPluginsPage - 1 || i == kThemesPage - 1 || i == kEffectsPage - 1;
        settings->content[i] = {label, i < kSettingsPanelCount && !store ? schema.panels[i].summary : Utf8View{}, {}, 1};
        settings->content[i].host = settings->content[i].wide = store;
        if (!ui_page_registry_register(&settings->registry, {{kNativeSettingsOwner, i + 1}, label, &settings->content[i]})
                 .ok()) {
            pages_release(settings);
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

static UiKitMeasure g_measure;
// Before sign-in no account cache exists; the settings measure through a cache of their own over the renderer.
static UiTextMeasureRecord g_measure_records[128];
static UiTextMeasureCache g_measure_cache;
static u32 g_measure_typography;
// The catalog's sidebar with the attention dot on Updates while an update waits for Restart to update.
static UiSettingsNavEntry g_navigation[kUiSettingsNavigationMaximum]{};

static UiTextMeasureCache* measure_cache() noexcept {
    if (g_app.account_ui)
        return &g_app.account_ui->text_measure_cache;
    if (!g_measure_cache.storage.data &&
        !ui_text_measure_cache_initialize(&g_measure_cache, {g_measure_records, 128}, renderer_text_measure_backend,
                                          &g_app.renderer))
        return nullptr;
    if (g_measure_typography != renderer_typography_revision(g_app.renderer)) {
        g_measure_typography = renderer_typography_revision(g_app.renderer);
        ui_text_measure_cache_invalidate(&g_measure_cache);
    }
    return &g_measure_cache;
}

static void bind_chrome(ApplicationSettings& settings) noexcept {
    // Widths and wrapped heights come from a text cache (shaped, not estimated).
    ui_kit_measure_cache(&g_measure, measure_cache());
    ui_settings_scene_bind_measure(&settings.scene, &g_measure);
    // The signed-out list is a navigation of its own.
    const bool signed_out = g_signed_out.active;
    auto navigation = signed_out ? settings_catalog_navigation_signed_out()
                                 : settings_catalog_navigation(application_account_logout_available());
    bool attention = application_update_badge_restarts(application_updates_view().state);
    if (attention) {
        const auto dotted = settings_catalog_navigation_dotted(navigation, g_navigation);
        attention = dotted.count != 0;
        if (attention)
            navigation = dotted;
    }
    ui_settings_scene_bind_navigation(&settings.scene, navigation,
                                      settings.registry.revision ^ (signed_out ? 0x8000'0000u : 0u) ^ (attention ? 0x4000'0000u : 0u));
    const bool focused = settings.scene.focus.kind == UiSettingsTargetKind::search &&
                         application_window_keyboard_focus();
    ui_settings_scene_set_search(
        &settings.scene,
        {{search_query(settings), minimum(settings.search_edit.anchor, settings.search_bytes),
          minimum(settings.search_edit.caret, settings.search_bytes), true, focused, focused},
         focused ? &g_app.renderer.media_picker_edit_geometry : nullptr,
         utf8_literal("Search")});
}

bool update_scene(ApplicationSettings& settings, const UiRect& viewport) noexcept {
    ui_settings_scene_bind_theme_styles(&settings.scene, application_theme().styles);
    bind_chrome(settings);
    return ui_settings_scene_update(&settings.scene, &settings.registry, viewport);
}

} // namespace settings_detail
} // namespace maicord::app_detail

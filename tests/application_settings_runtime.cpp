#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_channel_overview.hpp"
#include "application_friends.hpp"
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_profile.hpp"
#include "application_profile_editor.hpp"
#include "application_account_menu.hpp"
#include "application_scrolling.hpp"
#include "application_message_interaction.hpp"
#include "application_content_input.hpp"
#include "application_shell_input.hpp"
#include "application_attachments.hpp"
#include "application_notifications.hpp"
#include "application_text_form.hpp"
#include "application_theme_editor.hpp"
#include "application_custom_theme.hpp"
#include "application_surface_motion.hpp"
#include "application_tooltip.hpp"
#include "application_voice.hpp"
#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/render/content_text_retention.hpp"
#include "maicord/ui/settings_choice.hpp"
#include "fixtures/native_plugin_fixture.hpp"
#include "settings_runtime_keys.hpp"
#include "test_output.hpp"

namespace maicord {
void test_application_plugin_navigation_runtime(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_settings_choice_runtime(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_plugin_catalog_runtime(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_plugins_read_runtime(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_custom_theme(void (*check)(bool, const char*) noexcept) noexcept;
namespace app_detail {
void test_application_user_settings_runtime(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_keybinds_settings_runtime(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_plugins_actions_runtime(void (*check)(bool, const char*) noexcept) noexcept;
}
namespace settings_runtime_test {
using namespace app_detail;
ApplicationCustomThemeView custom_theme_projection{};
ApplicationCustomThemeAction custom_theme_last_action{};
u32 custom_theme_actions{};
bool custom_theme_pointer_transition{};
constexpr u64 fixture_owner = 0x4D43504649585432ull;
using native_plugin_fixture::FixtureAudit;
struct Observations {
    UiRect viewport{0, 0, 1000, 700};
    u32 text_builds, layout_updates, account_text_updates, forced_account_text_updates, suspends,
        drop_updates, member_updates;
    u32 resize_sync_presents, resize_finishes, resize_finish_failures;
    bool resize_finish_failed;
    bool fail_text;
    u32 fail_text_remaining;
    NativePluginModule* detaching_module;
    FixtureAudit* audit;
    bool detached_live_code, detached_live_state, detached_active_scene, detached_live_menu_labels;
    u32 profile_opens;
    u64 profile_user;
    ProfileAnchor profile_anchor;
    u32 account_menu_opens;
    UiWidgetHandle account_menu_source;
    UiRect account_menu_anchor;
    bool friends_active, requests_active;
    u32 requests_closed, popup_dismissals, opened_urls;
    u32 notifications_refreshes, notifications_setups;
    u32 content_hits, text_hits, profile_hits;
    bool detached_active_popup, detached_popup_labels;
} observed{};
struct GradientFormBoundary {
    ApplicationTextFormRequest request;
    char initial[128];
    u32 opens;
    bool active;
} gradient_form{};
struct MenuFixture {
    PopupItem items[16];
    PopupRowWidget rows[16];
    UiWidget widgets[34];
    UiLayoutNode nodes[34];
    UiFriendsScene friends, requests;
    UiFriendsRow row, requests_row;
    UiFriendsSource source, requests_source;
    MessageRecord records[4];
    u32 order[4];
    u8 text[256];
} menu_fixture{};
bool resolve_friend(void* context, u32 index, UiFriendsRow* output) noexcept {
    if (index || !output)
        return false;
    *output = *static_cast<UiFriendsRow*>(context);
    return true;
}
bool find_friend(void* context, u64 id, u32* index) noexcept {
    if (!index || static_cast<UiFriendsRow*>(context)->user_id != id)
        return false;
    *index = 0;
    return true;
}
struct PickerRuntime {
    ConstSlice<wchar_t> path;
    NativeFilePickerResult result;
};
struct PickerBoundary {
    ConstSlice<wchar_t> next_path;
    u32 starts, releases, cancellations, path_reads, release_timeouts;
} picker_boundary{};
HMODULE manual_rollback_reference = nullptr;
void manual_scope_change() noexcept {
    application_plugins_account_scope_invalidated();
}
void manual_failed_free() noexcept {
    if (!g_app.plugins)
        return;
    // The genuine reference remains pinned and is restored immediately after
    // the asserted API failure. No pointer is freed or dereferenced by this seam.
    manual_rollback_reference = g_app.plugins->modules[0].module;
    g_app.plugins->modules[0].module = reinterpret_cast<HMODULE>(static_cast<usize>(1));
}
u32 failures = 0;
void check(bool success, const char* name) noexcept {
    if (success)
        return;
    ++failures;
    test_write_text("FAIL ");
    test_write_line(name);
}
void measurement(const char* name, u64 value) noexcept {
    test_write_metric(name, value);
}
UiSettingsScene* scene() noexcept {
    return const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
}
bool activate_control(UiPageKey page, u64 control, u32 key = VK_RETURN) noexcept {
    auto* current = scene();
    if (!current || !ui_settings_scene_select(current, page) || !application_settings_synchronize() ||
        !ui_settings_scene_focus(current, {UiSettingsTargetKind::control, page, control}))
        return false;
    const bool consumed = application_settings_input(UiInputKind::key_down, key, false);
    const bool tail = application_settings_input(UiInputKind::key_up, key, false);
    // The production frame publishes the coalesced glyph suffix after input.
    return consumed && tail && application_settings_synchronize();
}
bool await_save() noexcept {
    const HANDLE event = application_preferences_notification();
    const u64 deadline = GetTickCount64() + 5'000;
    while (g_app.preferences_save_pending) {
        const u64 now = GetTickCount64();
        if (!event || now >= deadline ||
            WaitForSingleObject(event, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            return false;
        application_preferences_poll();
    }
    return true;
}
bool await_save_retained() noexcept {
    auto* current = scene();
    if (!current || !current->widgets.count)
        return false;
    const u32 revision = current->revision;
    const u16 generation = current->widgets.storage[0].generation;
    const u32 text_builds = observed.text_builds;
    void* widgets = current->storage.arena.base;
    // A successful save changes no visible state: the scene and text stay retained.
    return await_save() && scene() == current && current->revision == revision &&
           current->widgets.storage[0].generation == generation && current->storage.arena.base == widgets &&
           observed.text_builds == text_builds;
}
bool labelled(const UiSettingsPageContent& page, u64 id, Utf8View label) noexcept {
    for (u32 i = 0; i < page.controls.count; ++i)
        if (page.controls[i].id == id)
            return utf8_equal(page.controls[i].label, label);
    return false;
}
void test_native_projection_bank() noexcept {
    auto* pages = application_settings_pages();
    auto* current = scene();
    if (!pages || !current) {
        check(false, "settings shared bank: active native owner exists");
        return;
    }
    const auto content = [&](u64 page_id) noexcept -> const UiSettingsPageContent* {
        const auto* page = ui_page_registry_find(*pages, {1, page_id});
        return page ? static_cast<const UiSettingsPageContent*>(page->context) : nullptr;
    };
    const auto* appearance = content(2);
    const auto* accessibility = content(3);
    const auto* notifications = content(1);
    const auto* plugins = content(4);
    if (!appearance || !accessibility || !notifications || !plugins || !appearance->controls.data) {
        check(false, "settings shared bank: bounded native page descriptors exist");
        return;
    }
    const auto* shared = appearance->controls.data;
    const auto prefs = g_app.preferences;
    const u32 refreshes = observed.notifications_refreshes, setups = observed.notifications_setups;
    const u32 appearance_revision = appearance->revision;
    check(ui_page_registry_select(pages, kNotificationsPage).ok() && application_settings_synchronize() &&
              current->selected_page.page_id == 1 && notifications->controls.data == shared &&
              notifications->controls.count == 11 && !appearance->controls.data &&
              !appearance->controls.count && appearance->revision > appearance_revision &&
              !plugins->controls.data && !plugins->controls.count &&
              utf8_equal(notifications->controls[0].label, utf8_literal("Overview")) &&
              utf8_equal(notifications->controls[8].label, utf8_literal("Sound volume")) &&
              notifications->controls[8].choices.count == 201 &&
              observed.notifications_refreshes == refreshes + 1 &&
              ui_settings_scene_focus(current, {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.volume")}),
          "settings shared bank: external Notifications selection projects and refreshes before its first "
          "frame");
    const auto allocation = current->storage.arena.base;
    const u16 generation = current->widgets.generation;
    const u32 layout = current->widgets.layout.revision, text_builds = observed.text_builds;
    const auto focused = ui_settings_scene_focused(*current);
    check(application_settings_synchronize() && application_settings_synchronize() &&
              current->storage.arena.base == allocation && current->widgets.generation == generation &&
              current->widgets.layout.revision == layout && observed.text_builds == text_builds + 1 &&
              ui_settings_target_equal(ui_settings_scene_focused(*current), focused) &&
              observed.notifications_refreshes == refreshes + 1,
          "settings shared bank: focused publication builds once then preserves layout and borrowed "
          "controls");
    const u32 notification_revision = notifications->revision;
    check(ui_page_registry_select(pages, kAppearancePage).ok() && application_settings_synchronize() &&
              appearance->controls.data == shared && appearance->controls.count >= 9 &&
              !notifications->controls.data && !notifications->controls.count &&
              notifications->revision > notification_revision &&
              utf8_equal(appearance->controls[0].label, utf8_literal("Theme")) &&
              ui_settings_scene_focus(current, {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.member_list")}) &&
              ui_settings_scene_focused(*current).page.page_id == 2,
          "settings shared bank: 5 to 1 retires old borrow and routes focus to Appearance labels");
    const u32 retired_appearance_revision = appearance->revision;
    check(ui_page_registry_select(pages, kAccessibilityPage).ok() && application_settings_synchronize() &&
              accessibility->controls.data == shared && accessibility->controls.count >= 9 &&
              !appearance->controls.data && appearance->revision > retired_appearance_revision &&
              labelled(*accessibility, cid("accessibility.reduced_motion"), utf8_literal("Enable Reduced Motion")) &&
              ui_settings_scene_focus(current, {UiSettingsTargetKind::control, kAccessibilityPage, cid("accessibility.reduced_motion")}) &&
              ui_settings_scene_focused(*current).page.page_id == 3 &&
              observed.notifications_refreshes == refreshes + 1,
          "settings shared bank: 1 to 2 replaces descriptors without polling inactive Notifications");
    const u32 accessibility_revision = accessibility->revision;
    check(ui_page_registry_select(pages, kNotificationsPage).ok() && application_settings_synchronize() &&
              notifications->controls.data == shared && notifications->controls.count == 11 &&
              !accessibility->controls.data && !accessibility->controls.count &&
              accessibility->revision > accessibility_revision &&
              utf8_equal(notifications->controls[0].label, utf8_literal("Overview")) &&
              ui_settings_scene_focus(current, {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.volume")}) &&
              ui_settings_scene_focused(*current).page.page_id == 1 &&
              observed.notifications_refreshes == refreshes + 2 &&
              observed.notifications_setups == setups &&
              client_preferences_equal(prefs, g_app.preferences) && !g_app.preferences_save_pending,
          "settings shared bank: 2 to 5 reacquires all six values without setup or persistence");
    check(ui_page_registry_select(pages, kAppearancePage).ok() && application_settings_synchronize(),
          "settings shared bank: original Appearance page restores after projection regression");
    const auto before_catalog = appearance->revision;
    const auto widget_generation = current->widgets.generation;
    check(ui_page_registry_select(pages, kPluginsPage).ok() && application_settings_synchronize() &&
              plugins->controls.data && plugins->controls.data != shared && plugins->controls.count &&
              !appearance->controls.data &&
              !appearance->controls.count && appearance->revision > before_catalog &&
              current->widgets.generation != widget_generation &&
              !ui_settings_scene_control(*current, {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.gradient_intensity")}),
          "settings shared bank: the catalog page borrows its own bank and retires Appearance borrow and "
          "input generation");
    const UiSettingsTarget retired_catalog{
        UiSettingsTargetKind::control, kPluginsPage, plugins->controls.count ? plugins->controls[0].id : 1};
    const auto before_appearance = plugins->revision;
    check(ui_page_registry_select(pages, kAppearancePage).ok() && application_settings_synchronize() &&
              appearance->controls.data == shared && appearance->controls.count >= 9 &&
              !plugins->controls.data && !plugins->controls.count &&
              plugins->revision > before_appearance &&
              !ui_settings_scene_control(*current, retired_catalog),
          "settings shared bank: returning Appearance retires catalog source target while keeping "
          "independent catalog backing");
}
// A page whose projection never publishes stalls every frame. The watchdog
// turns such a hang into a prompt failure instead of a CTest timeout.
DWORD WINAPI settings_page_watchdog(void* finished) noexcept {
    if (WaitForSingleObject(finished, 5'000) == WAIT_OBJECT_0)
        return 0;
    constexpr char message[] = "FAIL settings pages: opening the catalog pages hung for 5 s\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, nullptr);
    ExitProcess(3);
}
void test_every_settings_page() noexcept {
    auto* pages = application_settings_pages();
    auto* current = scene();
    const HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE watchdog =
        finished ? CreateThread(nullptr, 0, settings_page_watchdog, finished, 0, nullptr) : nullptr;
    check(pages && current && watchdog, "settings pages: owner and page watchdog exist");
    if (!pages || !current || !watchdog) {
        if (finished)
            CloseHandle(finished);
        return;
    }
    const u64 started = GetTickCount64();
    u32 opened = 0;
    // Pages open the way a sidebar click does: select, then one frame.
    for (u32 page = 1; page <= kSettingsPanelCount; ++page) {
        const UiPageKey key{1, page};
        const auto* entry = ui_page_registry_find(*pages, key);
        const auto* content = entry ? static_cast<const UiSettingsPageContent*>(entry->context) : nullptr;
        const bool ok = content && ui_page_registry_select(pages, key).ok() && application_settings_synchronize() &&
                        application_settings_active() && ui_page_key_equal(current->selected_page, key) &&
                        ui_settings_page_content_valid(*content) && current->widgets.count;
        opened += ok;
        if (!ok) {
            char name[] = "settings pages: catalog page 00 opens with valid content";
            name[29] = static_cast<char>('0' + page / 10);
            name[30] = static_cast<char>('0' + page % 10);
            check(false, name);
        }
    }
    const u64 elapsed = GetTickCount64() - started;
    SetEvent(finished);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);
    CloseHandle(finished);
    check(opened == kSettingsPanelCount, "settings pages: every catalog page opens and publishes");
    check(elapsed < 2'000, "settings pages: every catalog page opens within the bounded time");
    measurement("MEASURE settings_all_pages_ms=", elapsed);
    check(ui_page_registry_select(pages, kAppearancePage).ok() && application_settings_synchronize(),
          "settings pages: Appearance restores after the page sweep");
}
// The search field edits like every other single-line field: typing from the
// sidebar focuses it, and Backspace, Delete, arrows and Ctrl+A apply.
void test_search_field() noexcept {
    auto* current = scene();
    if (!current || !ui_settings_scene_focus(current, {UiSettingsTargetKind::page, kAppearancePage, 0})) {
        check(false, "settings search: sidebar focus precedes typing");
        return;
    }
    const auto key = [](u32 code) noexcept {
        const bool down = application_settings_input(UiInputKind::key_down, code, false);
        return application_settings_input(UiInputKind::key_up, code, false) && down;
    };
    const auto query = [&](const char* expected) noexcept {
        u32 bytes = 0;
        while (expected[bytes])
            ++bytes;
        return utf8_equal(current->search.view.text, {expected, bytes}) && current->search.view.caret <= bytes;
    };
    for (const char* c = "theme"; *c; ++c)
        application_settings_window_message(WM_CHAR, static_cast<WPARAM>(*c), 0);
    check(query("theme") && current->search.view.caret == 5 && ui_page_key_equal(current->selected_page, kSearchPage) &&
              ui_settings_scene_focus_zone(*current) == UiSettingsFocusZone::search,
          "settings search: typing from the sidebar focuses the field and opens results");
    check(key(VK_BACK) && query("them") && ui_settings_scene_focus_zone(*current) == UiSettingsFocusZone::search,
          "settings search: Backspace deletes after the results page relayouts");
    check(key(VK_LEFT) && key(VK_LEFT) && key(VK_DELETE) && query("thm") && current->search.view.caret == 2,
          "settings search: arrows move the caret and Delete removes forward");
    BYTE original_keys[256]{};
    if (GetKeyboardState(original_keys)) {
        BYTE local_keys[256]{};
        local_keys[VK_CONTROL] = 0x80;
        if (SetKeyboardState(local_keys) && (GetKeyState(VK_CONTROL) & 0x8000)) {
            const bool selected = key('A') && current->search.view.anchor == 0 && current->search.view.caret == 3;
            SetKeyboardState(original_keys);
            check(selected && key(VK_BACK) && query("") && ui_page_key_equal(current->selected_page, kAppearancePage) &&
                      ui_settings_scene_focus_zone(*current) == UiSettingsFocusZone::search,
                  "settings search: Ctrl+A then Backspace clears the query and restores the page");
        } else
            SetKeyboardState(original_keys);
    }
    application_settings_window_message(WM_CHAR, 'x', 0);
    check(query("x") && key(VK_ESCAPE) && query("") && application_settings_active() &&
              ui_settings_scene_focus_zone(*current) == UiSettingsFocusZone::search,
          "settings search: Escape clears a query before it closes Settings");
}
bool freed(void* address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    return address && VirtualQuery(address, &region, sizeof(region)) == sizeof(region) &&
           region.State == MEM_FREE;
}
bool fixture_path(wchar_t* output, u32* count) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, output, kNativePluginModuleMaxPathCharacters + 1);
    if (!length || length > kNativePluginModuleMaxPathCharacters)
        return false;
    u32 start = length;
    while (start && output[start - 1] != L'\\')
        --start;
    constexpr wchar_t name[] = L"MaiCordPluginFixture.dll";
    constexpr u32 chars = sizeof(name) / sizeof(name[0]) - 1;
    if (!start || start + chars > kNativePluginModuleMaxPathCharacters)
        return false;
    for (u32 i = 0; i <= chars; ++i)
        output[start + i] = name[i];
    *count = start + chars;
    return true;
}
LPARAM shell_point(UiSemantic semantic) noexcept {
    const auto* node = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, semantic));
    if (!node)
        return 0;
    return MAKELPARAM(static_cast<short>((node->rectangle.left + node->rectangle.right) * .5f),
                      static_cast<short>((node->rectangle.top + node->rectangle.bottom) * .5f));
}
bool menu_fixture_initialize() noexcept {
    auto& fixture = menu_fixture;
    fixture.row = {1234,
                   utf8_literal("Fixture friend"),
                   utf8_literal("fixture"),
                   {},
                   UiPresence::offline,
                   UiFriendsKind::friend_user,
                   false};
    fixture.source = {&fixture.row, 1, 1, resolve_friend, find_friend};
    fixture.requests_row = fixture.row;
    fixture.requests_row.kind = UiFriendsKind::incoming;
    fixture.requests_source = {&fixture.requests_row, 1, 1, resolve_friend, find_friend};
    return popup_scene_initialize(&g_app.popup_scene, {fixture.items, 16}, {fixture.rows, 16},
                                  {fixture.widgets, 34}, {fixture.nodes, 34}) &&
           ui_friends_scene_update(&fixture.friends, &fixture.source, UiFriendsTab::all,
                                   UiFriendsPhase::ready, {}, observed.viewport) &&
           ui_friends_scene_update(&fixture.requests, &fixture.requests_source, UiFriendsTab::pending,
                                   UiFriendsPhase::ready, {}, observed.viewport) &&
           message_store_initialize(&g_app.message_view.store, {fixture.records, 4}, {fixture.order, 4},
                                    {fixture.text, 256});
}
// The fixture DLL registers one user item (1) and one message item (2).
bool open_plugin_menu(ApplicationPluginMenuSurface surface, u64 id, u32 expected_count = 1,
                      bool bind = true) noexcept {
    PopupItem plugins[8]{};
    const u32 count = application_plugins_menu_items(surface, id, {plugins, 8});
    if (count != expected_count || !count || plugins[0].action != kApplicationPluginPopupActionFirst ||
        !(plugins[0].flags & popup_item_separator_before) || plugins[0].media_id || plugins[0].media_flags)
        return false;
    PopupItem combined[9]{{utf8_literal("Built-in fixture command"), 17, popup_item_enabled}};
    for (u32 index = 0; index < count; ++index)
        combined[index + 1] = plugins[index];
    g_app.popup_owner_kind = surface == ApplicationPluginMenuSurface::user      ? PopupOwnerKind::member
                             : surface == ApplicationPluginMenuSurface::friends ? PopupOwnerKind::friends
                                                                                : PopupOwnerKind::message;
    return popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, id, {combined, count + 1},
                            {20, 20, 40, 40}, observed.viewport, 300)
               .ok() &&
           (!bind || application_plugins_menu_bind()) && g_app.popup_scene.items[0].action == 17 &&
           g_app.popup_scene.items[1].action == kApplicationPluginPopupActionFirst;
}
void menu_fixture_release() noexcept {
    dismiss_popup();
    popup_scene_release(&g_app.popup_scene);
    g_app.popup_scene = {};
    g_app.renderer.friends_scene = nullptr;
    g_app.renderer.friends_requests_scene = nullptr;
    ui_friends_scene_release(&menu_fixture.friends);
    ui_friends_scene_release(&menu_fixture.requests);
    observed.friends_active = observed.requests_active = false;
    g_app.message_view.store = {};
    g_app.context_focus = nullptr;
}
// Plugin rows through the real DLL, the SDK registry and the shipping
// projection, binder and dispatcher.
void test_menu_runtime(FixtureAudit& audit) noexcept {
    using S = ApplicationPluginMenuSurface;
    constexpr u32 first = kApplicationPluginPopupActionFirst;
    const auto saved_store = g_app.message_view.store;
    const u64 profile = g_app.account.status.profile_id;
    const u64 channel = g_app.account_ui->chat_state.selected_channel_id;
    application_settings_close(false);
    check(menu_fixture_initialize(), "plugins menu: actual popup and message store initialize");
    g_app.account.status.profile_id = 77;
    audit.popup = &g_app.popup_scene;
    void* owner_memory = g_app.settings;
    bool repeated_projection = true;
    for (u32 index = 0; index < 3; ++index) {
        PopupItem projected[8]{};
        repeated_projection = repeated_projection &&
                              application_plugins_menu_items(S::user, 1234, {projected, 8}) == 1 &&
                              utf8_equal(projected[0].label, utf8_literal("Open native fixture for friend"));
    }
    check(repeated_projection && g_app.settings == owner_memory && !g_app.popup_scene.active &&
              !g_app.popup_scene.owned_storage.base && !audit.menu_activations,
          "plugins menu: repeated projection copies the DLL's user row without popup allocation or DLL entry");
    const u32 callbacks = audit.menu_activations;
    check(open_plugin_menu(S::user, 1234), "plugins menu: widget generation test opens actual bound popup");
    const u32 opens = g_app.popup_scene.opens;
    const u16 generation = g_app.popup_scene.widgets.generation;
    ui_widget_manager_reset(&g_app.popup_scene.widgets);
    check(g_app.popup_scene.opens == opens && g_app.popup_scene.widgets.generation != generation &&
              application_plugins_menu_action(first) && !g_app.popup_scene.active &&
              !application_plugins_menu_state().count && audit.menu_activations == callbacks,
          "plugins menu: real widget reset rejects stale generation without entering the DLL");
    check(open_plugin_menu(S::user, 1234, 1, false) && !application_plugins_menu_state().bound &&
              g_app.popup_scene.active,
          "plugins menu: projected DLL rows materialize before explicit bind");
    g_app.popup_owner_kind = PopupOwnerKind::message;
    check(!application_plugins_menu_bind() && !g_app.popup_scene.active && !application_plugins_menu_state().count &&
              audit.menu_activations == callbacks,
          "plugins menu: failed bind dismisses materialized rows and clears the bank");
    check(open_plugin_menu(S::user, 1234), "plugins menu: account-scoped user popup opens");
    ++g_app.account.status.profile_id;
    check(application_plugins_menu_action(first) && audit.menu_activations == callbacks,
          "plugins menu: switched profile rejects captured semantic target");
    --g_app.account.status.profile_id;
    check(open_plugin_menu(S::user, 1234), "plugins menu: popup generation test opens bound model");
    PopupItem replacement[]{{utf8_literal("Other built-in popup"), 18, popup_item_enabled}};
    check(popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, 1234, {replacement, 1},
                           {30, 30, 50, 50}, observed.viewport, 300)
                  .ok() &&
              application_plugins_menu_action(first) && audit.menu_activations == callbacks,
          "plugins menu: reopening actual popup invalidates earlier bank even for identical owner");
    check(open_plugin_menu(S::user, 1234) && application_plugins_menu_action(first) &&
              audit.menu_activations == callbacks + 1 && audit.menu_callback_popup_dismissed &&
              audit.last_menu_activation.target == MC_MENU_USER && audit.last_menu_activation.user_id == 1234 &&
              !audit.last_menu_activation.channel_id && !audit.last_menu_activation.message_id &&
              !g_app.popup_scene.active && !application_plugins_menu_state().count && !application_settings_active(),
          "plugins menu: actual DLL user handler runs after popup dismissal with the copied user ID");
    // A Friends row menu carries the same user rows; its relationship is the fence.
    observed.friends_active = true;
    check(open_plugin_menu(S::friends, 1234) && g_app.popup_owner_kind == PopupOwnerKind::friends,
          "plugins menu: Friends row popup binds the user rows to the Friends owner");
    menu_fixture.row.kind = UiFriendsKind::blocked;
    check(application_plugins_menu_action(first) && audit.menu_activations == callbacks + 1 &&
              !g_app.popup_scene.active,
          "plugins menu: changed Friends relationship rejects the captured row before DLL entry");
    menu_fixture.row.kind = UiFriendsKind::friend_user;
    check(open_plugin_menu(S::friends, 1234) && application_plugins_menu_action(first) &&
              audit.menu_activations == callbacks + 2 && audit.last_menu_activation.target == MC_MENU_USER &&
              audit.last_menu_activation.user_id == 1234 && audit.menu_callback_popup_dismissed,
          "plugins menu: Friends row dispatches the user context after dismissal");
    observed.friends_active = false;
    g_app.account_ui->chat_state.selected_channel_id = 9012;
    MessageInput message{3456, 9012, 5678, 1, 0, utf8_literal("Fixture message"), 0};
    check(message_store_upsert(&g_app.message_view.store, message).ok() && open_plugin_menu(S::message, 3456),
          "plugins menu: actual message store supplies the message popup");
    message.content = utf8_literal("Changed fixture message");
    check(message_store_upsert(&g_app.message_view.store, message).ok() && application_plugins_menu_action(first) &&
              audit.menu_activations == callbacks + 2,
          "plugins menu: changed message revision rejects action before DLL entry");
    check(open_plugin_menu(S::message, 3456), "plugins menu: chat popup captures current message generation");
    check(message_store_delete(&g_app.message_view.store, 3456) &&
              message_store_upsert(&g_app.message_view.store, message).ok() &&
              application_plugins_menu_action(first) && audit.menu_activations == callbacks + 2,
          "plugins menu: delete and recreate with same message ID cannot revive captured generation");
    check(open_plugin_menu(S::message, 3456), "plugins menu: channel scope test opens live chat popup");
    g_app.account_ui->chat_state.selected_channel_id = 9013;
    check(application_plugins_menu_action(first) && audit.menu_activations == callbacks + 2,
          "plugins menu: channel switch rejects captured chat target");
    g_app.account_ui->chat_state.selected_channel_id = 9012;
    check(open_plugin_menu(S::message, 3456) && application_plugins_menu_action(first) &&
              audit.menu_activations == callbacks + 3 && audit.menu_callback_popup_dismissed &&
              audit.last_menu_activation.target == MC_MENU_MESSAGE && audit.last_menu_activation.item_id == 2 &&
              audit.last_menu_activation.user_id == 5678 && audit.last_menu_activation.channel_id == 9012 &&
              audit.last_menu_activation.message_id == 3456,
          "plugins menu: actual message dispatch delivers the copied author, channel and message IDs");
    const auto configure = reinterpret_cast<native_plugin_fixture::Configure>(
        GetProcAddress(GetModuleHandleW(L"MaiCordPluginFixture.dll"), "maicord_plugin_fixture_configure"));
    check(configure != nullptr, "plugins menu: actual loaded DLL exposes failure configuration");
    if (configure) {
        configure(9, fixture_owner, &audit);
        check(open_plugin_menu(S::message, 3456) && application_plugins_menu_action(first) &&
                  audit.menu_activations == callbacks + 4 && audit.menu_callback_popup_dismissed &&
                  !g_app.popup_scene.active && !application_settings_active(),
              "plugins menu: a rejecting DLL handler leaves no popup and opens nothing");
        configure(0, fixture_owner, &audit);
    }
    // Any registry change expires captured rows, then the removed item is gone.
    check(open_plugin_menu(S::user, 1234) && audit.remove_menu && audit.remove_menu(2) &&
              application_plugins_menu_action(first) && !g_app.popup_scene.active &&
              audit.menu_activations == callbacks + 4,
          "plugins menu: a menu revision change rejects a captured row before DLL entry");
    PopupItem removed[8]{};
    check(!application_plugins_menu_items(S::message, 3456, {removed, 8}) && !application_plugins_menu_state().count,
          "plugins menu: a removed DLL item projects no row");
    menu_fixture_release();
    audit.popup = nullptr;
    g_app.message_view.store = saved_store;
    g_app.account.status.profile_id = profile;
    g_app.account_ui->chat_state.selected_channel_id = channel;
}
void test_popup_unload(ConstSlice<wchar_t> dll_path) noexcept {
    static FixtureAudit audit{};
    audit.pages = application_settings_pages();
    const auto saved_store = g_app.message_view.store;
    const u64 profile = g_app.account.status.profile_id;
    application_settings_close(false);
    HMODULE pin = LoadLibraryExW(dll_path.data, nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(pin != nullptr, "plugins menu: unload fixture pins actual DLL for explicit reload");
    if (!pin) {
        application_settings_open();
        return;
    }
    const auto configure = reinterpret_cast<native_plugin_fixture::Configure>(
        GetProcAddress(pin, "maicord_plugin_fixture_configure"));
    if (configure)
        configure(0, fixture_owner, &audit);
    application_plugins_sdk_bind_host(*g_app.plugins, application_settings_plugin_host());
    const auto loaded = configure
                            ? native_plugin_module_load(&g_app.plugins->modules[0],
                                                        application_settings_plugin_host(), dll_path, MC_PERM_ALL)
                            : NativePluginModuleResult{NativePluginModuleError::missing_export, {}, 0};
    FreeLibrary(pin);
    check(loaded.ok(), "plugins menu: actual DLL reload registers fresh generation for popup unload");
    if (!loaded.ok()) {
        application_settings_open();
        return;
    }
    check(menu_fixture_initialize(), "plugins menu: popup unload initializes real borrowing surfaces");
    g_app.account.status.profile_id = 77;
    check(open_plugin_menu(ApplicationPluginMenuSurface::user, 1234),
          "plugins menu: DLL user row is borrowed by an open popup before unload");
    observed.detaching_module = &g_app.plugins->modules[0];
    observed.audit = &audit;
    observed.detached_active_popup = observed.detached_popup_labels = false;
    observed.detached_live_code = observed.detached_live_state = false;
    auto* sdk = application_settings_plugin_host()->sdk;
    check(application_plugins_unload(0) && !g_app.popup_scene.active &&
              g_app.popup_owner_kind == PopupOwnerKind::none && !application_plugins_menu_state().count &&
              observed.detached_active_popup && observed.detached_popup_labels && observed.detached_live_code &&
              observed.detached_live_state && audit.host_detaches == 1 && audit.stops == 1 &&
              audit.stop_after_detach && audit.stop_without_pages && audit.stop_services_inactive &&
              audit.stop_menu_services_inactive && audit.dll_detaches == 1 && audit.dll_after_stop &&
              !application_plugins_present(0) && !application_settings_active() &&
              GetModuleHandleW(L"MaiCordPluginFixture.dll") == nullptr,
          "plugins menu: unload dismisses the borrowing popup while DLL/state/labels live, clears the bank, then "
          "stops and frees the DLL");
    NativePluginSdkMenuContext context{};
    context.target = MC_MENU_USER;
    context.user_id = 1234;
    NativePluginSdkMenuItem items[4]{};
    check(sdk && !native_plugin_sdk_menu_items(*sdk, MC_MENU_USER, {items, 4}) &&
              !native_plugin_sdk_menu_activate(sdk, fixture_owner, 1, context) && !audit.menu_activations,
          "plugins menu: popup unload leaves no item that could enter the freed DLL");
    observed.detaching_module = nullptr;
    observed.audit = nullptr;
    menu_fixture_release();
    g_app.message_view.store = saved_store;
    g_app.account.status.profile_id = profile;
    check(application_settings_open(), "plugins menu: native Settings reopens after popup owner unload");
}
struct Cleanup {
    wchar_t path[MAX_PATH]{};
    HANDLE locked = INVALID_HANDLE_VALUE;
    ~Cleanup() noexcept {
        if (locked != INVALID_HANDLE_VALUE)
            CloseHandle(locked);
        // No unbounded join and no forced release of a live worker. g_app and
        // its owned VM remain valid until process exit if shutdown times out.
        application_preferences_shutdown();
        check(!g_app.preferences_writer, "settings: bounded preferences shutdown joins worker");
        if (g_app.preferences_writer)
            return;
        application_settings_release();
        if (g_app.settings || g_app.plugins)
            return;
        ui_scene_release(&g_app.shell_scene);
        animation_visual_runtime_release(&g_app.animations);
        if (g_app.account_ui)
            VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
        g_app.account_ui = nullptr;
        if (path[0])
            DeleteFileW(path);
    }
};
void test_modal_scroll_binding(UiScrollBinding binding, const char* label) noexcept {
    UiScrollRange original{}, start{}, middle{}, end{};
    bool changed = false;
    UiScrollHost host{};
    bool ok = binding.read(binding.context, binding.key, &original) && original.maximum > 0 &&
              binding.write(binding.context, binding.key, 0, &changed) &&
              binding.read(binding.context, binding.key, &start);
    const UiPoint point{start.viewport.left + 1, start.viewport.top + 1};
    ok = ok &&
         ui_scroll_host_wheel(&host, binding, point, -120, 3, false, 1'000'000) ==
             UiScrollDispatch::handled &&
         ui_scroll_host_active(host) && binding.read(binding.context, binding.key, &middle) &&
         middle.offset == 0 && ui_scroll_host_tick(&host, 1'100'000, &changed) && changed &&
         binding.read(binding.context, binding.key, &middle) && middle.offset > 0 &&
         middle.offset < minimum(96.0, start.maximum) && middle.generation == start.generation &&
         ui_scroll_host_tick(&host, 1'250'000, &changed) &&
         binding.read(binding.context, binding.key, &end) && end.offset == minimum(96.0, start.maximum) &&
         !ui_scroll_host_active(host);
    host.immediate = true;
    ok = ok && binding.write(binding.context, binding.key, 0, &changed) &&
         ui_scroll_host_wheel(&host, binding, point, -120, 3, false, 1'300'000) ==
             UiScrollDispatch::handled &&
         binding.read(binding.context, binding.key, &end) && end.offset == minimum(96.0, start.maximum) &&
         !ui_scroll_host_active(host);
    binding.write(binding.context, binding.key, original.offset, &changed);
    check(ok, label);
}
void test_controller() noexcept {
    Cleanup cleanup{};
    PopupItem dormant_menu[8]{};
    check(!application_settings_plugin_host_current() && !application_plugins_menu_state().count &&
              !application_plugins_menu_items(ApplicationPluginMenuSurface::user, 1234, {dormant_menu, 8}) &&
              !g_app.settings && !g_app.plugins && !g_app.popup_scene.active &&
              !g_app.popup_scene.owned_storage.base,
          "plugins menu: dormant projection and current-owner getters allocate no settings or plugin "
          "runtime");
    wchar_t directory[MAX_PATH]{};
    const DWORD chars = GetTempPathW(MAX_PATH, directory);
    const bool temporary =
        chars && chars < MAX_PATH && GetTempFileNameW(directory, L"MSR", 0, cleanup.path);
    check(temporary, "settings: external temporary preferences fixture created");
    if (!temporary)
        return;
    u8 legacy_preferences[32]{'M', 'A', 'I', 'C', 'P', 'R', 'E', 'F', 2, 0, 0, 0, 32, 0, 0, 0, 0, 30};
    byte_store_le64(legacy_preferences + 24, hash64_bytes(kHash64Seed, legacy_preferences, 24));
    HANDLE legacy_file = CreateFileW(cleanup.path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD legacy_written = 0;
    const bool legacy_saved = legacy_file != INVALID_HANDLE_VALUE &&
                              WriteFile(legacy_file, legacy_preferences, 32, &legacy_written, nullptr) &&
                              legacy_written == 32;
    if (legacy_file != INVALID_HANDLE_VALUE)
        CloseHandle(legacy_file);
    check(legacy_saved, "settings: bounded v2 preference fixture exists before writer starts");
    check(client_preferences_writer_create(&g_app.preferences_writer, cleanup.path),
          "settings: explicit fixture writer exists before any preference activation");
    if (!g_app.preferences_writer)
        return;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "settings: account UI is an owned zeroed VM object");
    if (!g_app.account_ui)
        return;
    application_plugins_account_scope_invalidated();
    g_app.preferences = client_preferences_defaults();
    check(client_preferences_load_file(cleanup.path, &g_app.preferences).error == ClientPreferencesError::version &&
              client_preferences_equal(g_app.preferences, client_preferences_defaults()),
          "settings: an older preference file loads as absent and native controls start from defaults");
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    g_app.renderer.dpi = 96;
    g_app.window = nullptr;
    g_app.keyboard_focus_visible = true;
    check(animation_visual_runtime_initialize(&g_app.animations),
          "settings: actual animation runtime initializes");
    g_app.renderer_ready = false;
    check(!application_settings_open() && !g_app.settings,
          "settings: renderer readiness gates open before allocation");
    g_app.renderer_ready = true;
    g_app.login.visible = true;
    check(!application_settings_open() && !g_app.settings,
          "settings: login visibility gates account settings");
    g_app.login.visible = false;
    UiWidgetManager previous_focus{};
    const u32 before_open_member_updates = observed.member_updates;
    g_app.context_focus = &previous_focus;
    g_app.message_composer.focused = true;
    g_app.message_composer_pointer_selection = true;
    g_app.message_text_pointer_selection = true;
    g_app.pane_resize = PaneResizeKind::channels;
    check(application_settings_open() && application_settings_active() && scene() && !g_app.context_focus &&
              !g_app.message_composer.focused && !g_app.message_composer_pointer_selection &&
              !g_app.message_text_pointer_selection && g_app.pane_resize == PaneResizeKind::none &&
              !g_app.animations.timeline.suspended && observed.suspends == 1 && observed.drop_updates == 1 &&
              g_app.frame_pending,
          "settings: open suspends composer and selection, keeps the shell's animation live and binds the "
          "real retained scene");
    if (!scene())
        return;
    const auto scroll_test_viewport = scene()->viewport;
    check(ui_settings_scene_update(scene(), application_settings_pages(), {0, 0, 960, 200}),
          "settings scrolling: small viewport exposes both virtual scroll ranges");
    test_modal_scroll_binding(application_settings_scroll_binding(false),
                              "settings scrolling: content uses smooth frames and immediate preference "
                              "with stable page lifetime");
    test_modal_scroll_binding(application_settings_scroll_binding(true),
                              "settings scrolling: sidebar uses smooth frames and immediate preference");
    check(ui_settings_scene_update(scene(), application_settings_pages(), scroll_test_viewport),
          "settings scrolling: viewport restores after scoped scroll checks");
    check(observed.member_updates == before_open_member_updates + 1,
          "settings: opening immediately updates member visibility and subscriptions");
    auto* settings_memory = g_app.settings;
    const u64 settings_lifetime = application_settings_lifetime();
    check(settings_lifetime && application_settings_pages() &&
              application_settings_lifetime() == settings_lifetime,
          "settings: existing owner lookups preserve a nonzero lifetime identity");
    MEMORY_BASIC_INFORMATION settings_region{};
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const u32 settings_bytes = application_settings_owner_bytes();
    const usize expected_committed = (static_cast<usize>(settings_bytes) + system_info.dwPageSize - 1) /
                                     system_info.dwPageSize * system_info.dwPageSize;
    check(settings_bytes &&
              VirtualQuery(settings_memory, &settings_region, sizeof(settings_region)) ==
                  sizeof(settings_region) &&
              settings_region.State == MEM_COMMIT && settings_region.RegionSize == expected_committed,
          "settings: measured owner sizeof matches page-rounded committed VM region");
    measurement("MEASURE settings_owner_bytes=", settings_bytes);
    measurement("MEASURE settings_owner_record_bytes=", settings_bytes);
    measurement("MEASURE settings_owner_committed_bytes=", settings_region.RegionSize);
    measurement("MEASURE login_scene_bytes=", sizeof(LoginScene));
    measurement("MEASURE login_view_bytes=", sizeof(LoginView));
    measurement("MEASURE account_host_bytes=", sizeof(AccountHost));
    measurement("MEASURE application_bytes=", sizeof(Application));
    measurement("MEASURE renderer_owner_bytes=", sizeof(D3D11Renderer));
    measurement("MEASURE plugins_runtime_bytes=", sizeof(ApplicationPlugins));
    measurement("MEASURE plugin_catalog_owner_bytes=", application_plugins_catalog_owner_bytes());
    measurement("MEASURE plugin_catalog_bytes=", sizeof(NativePluginCatalog));
    measurement("MEASURE plugin_manifest_bytes=", sizeof(NativePluginManifest));
    measurement("MEASURE plugin_catalog_entry_bytes=", sizeof(NativePluginCatalogEntry));
    measurement("MEASURE plugin_verified_file_bytes=", sizeof(NativePluginVerifiedFile));
    measurement("MEASURE account_ui_owner_bytes=", sizeof(AccountUiRuntime));
    measurement("MEASURE chat_state_store_bytes=", sizeof(ChatStateStore));
    measurement("MEASURE permission_role_stride_bytes=", sizeof(RoleRecord));
    measurement("MEASURE permission_overwrite_stride_bytes=", sizeof(PermissionOverwriteRecord));
    measurement("MEASURE permission_role_max_bytes=",
                sizeof(RoleRecord) * kChatStateRoleCapacity);
    measurement("MEASURE permission_overwrite_max_bytes=",
                sizeof(PermissionOverwriteRecord) * kChatStateOverwriteCapacity);
    test_native_projection_bank();
    test_every_settings_page();
    test_search_field();
    maicord::app_detail::test_application_keybinds_settings_runtime(check);
    auto* widget_memory = scene()->storage.arena.base;
    const u32 builds = observed.text_builds;
    check(application_settings_open() && g_app.settings == settings_memory &&
              observed.text_builds == builds,
          "settings: duplicate open preserves one controller and retained scene");
    check(application_settings_window_message(WM_CONTEXTMENU, 0, 0) &&
              application_settings_window_message(WM_RBUTTONDOWN, 0, 0) &&
              application_settings_input(UiInputKind::character, 'x', false) &&
              application_settings_active(),
          "settings: modal context menu and text input cannot reach account content");
    const u32 content_hits = observed.content_hits, text_hits = observed.text_hits,
              profile_hits = observed.profile_hits;
    check(set_client_cursor(980, 680) && GetCursor() == LoadCursorW(nullptr, IDC_ARROW) &&
              observed.content_hits == content_hits && observed.text_hits == text_hits &&
              observed.profile_hits == profile_hits,
          "settings: empty opaque page owns cursor without consulting underlying profiles, links or text");
    BYTE original_keys[256]{};
    if (GetKeyboardState(original_keys)) {
        BYTE local_keys[256]{};
        local_keys[VK_MENU] = 0x80;
        const bool set = SetKeyboardState(local_keys) != FALSE;
        if (set && (GetKeyState(VK_MENU) & 0x8000))
            check(!application_settings_input(UiInputKind::system_key_down, VK_F4, false) &&
                      application_settings_active(),
                  "settings: Alt+F4 passes through modal keyboard handling");
        // A platform which cannot establish thread-local modifiers leaves this
        // branch untested; never synthesize input into the desktop session.
        if (set)
            check(SetKeyboardState(original_keys) != FALSE,
                  "settings: original thread keyboard state restored");
    }
    const u32 before_close_member_updates = observed.member_updates;
    // A click on the scrim closes Settings; a press there released over the
    // dialog does not.
    const auto modal = scene()->layout.modal;
    const auto modal_viewport = scene()->viewport;
    if (!scene()->layout.compact && modal.left > modal_viewport.left + 8) {
        const LPARAM scrim = MAKELPARAM(static_cast<int>(modal_viewport.left + 4), static_cast<int>(modal.top + 40));
        const LPARAM dialog = MAKELPARAM(static_cast<int>((modal.left + modal.right) * .5f),
                                         static_cast<int>((modal.top + modal.bottom) * .5f));
        check(application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, scrim) &&
                  application_settings_window_message(WM_LBUTTONUP, 0, dialog) && application_settings_active(),
              "settings: a scrim press released over the dialog keeps Settings open");
        check(application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, scrim) &&
                  application_settings_window_message(WM_LBUTTONUP, 0, scrim) && !application_settings_active(),
              "settings: a click on the scrim closes Settings");
    } else
        application_settings_close();
    check(observed.member_updates == before_close_member_updates + 1,
          "settings: closing immediately restores member visibility and subscriptions");
    check(!application_settings_active() && !scene() && g_app.context_focus == &previous_focus &&
              g_app.message_composer.focused && !g_app.animations.timeline.suspended &&
              freed(widget_memory),
          "settings: close restores prior focus and releases modal widget storage");
    observed.fail_text = true;
    check(!application_settings_open() && !application_settings_active() && !scene() &&
              g_app.context_focus == &previous_focus && g_app.message_composer.focused &&
              !g_app.animations.timeline.suspended,
          "settings: failed renderer projection rolls back modal scene and restores focus");
    observed.fail_text = false;
    check(application_settings_open(), "settings: controller can reopen after projection failure");
    {
        // Leaving Profiles hands the modal back (the editor closed Settings
        // under it); only a genuine reopen plays the entrance.
        const HWND saved_window = g_app.window;
        const HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1000, 700, nullptr, nullptr,
                                            GetModuleHandleW(nullptr), nullptr);
        g_app.window = window;
        application_settings_close(false);
        const bool reopened = application_settings_open() && application_settings_synchronize() &&
                              g_app.renderer.settings_modal_progress < 1.f;
        application_settings_close(false);
        const bool resumed = application_settings_resume() && application_settings_active() &&
                             g_app.renderer.settings_modal_progress == 1.f && application_settings_synchronize() &&
                             g_app.renderer.settings_modal_progress == 1.f;
        g_app.window = saved_window;
        if (window)
            DestroyWindow(window);
        check(window && reopened && resumed,
              "settings: a reopen plays the entrance; the profile editor's hand-back does not replay it");
    }
    if (!scene())
        return;
    g_app.renderer.retained_presentation.valid = true;
    const u32 theme_layouts = observed.layout_updates;
    const u32 theme_account_text = observed.account_text_updates;
    check(
        activate_control(kAppearancePage, cid("appearance.theme"), VK_RIGHT) && g_app.preferences.theme == ClientTheme::amoled &&
            bytes_equal(&application_theme().base_lowest, &kAmoledTheme.base_lowest, sizeof(Color)) &&
            bytes_equal(&application_theme().text_primary, &kAmoledTheme.text_primary, sizeof(Color)) &&
            !application_theme().background.color_count && !g_app.renderer.retained_presentation.valid &&
            g_app.preferences_save_pending && g_app.preferences_request_revision &&
            observed.layout_updates == theme_layouts && observed.account_text_updates == theme_account_text,
        "settings: theme applies palette and queues save without rebuilding hidden shell or account text");
    check(await_save_retained() && !g_app.preferences_save_failed &&
              WaitForSingleObject(application_preferences_notification(), 0) == WAIT_TIMEOUT,
          "settings: Appearance save completion preserves dormant Storage and current scene and resets "
          "notification");
    ClientPreferences loaded{};
    check(client_preferences_load_file(cleanup.path, &loaded).ok() &&
              client_preferences_equal(loaded, g_app.preferences),
          "settings: queued controller choice reaches only external fixture file");
    const float saved_channel_width = g_app.channel_pane_width, saved_member_width = g_app.member_pane_width;
    g_app.channel_pane_width = 300.f;
    g_app.member_pane_width = 260.4f;
    g_app.pane_widths_persist_pending = true;
    g_app.pane_resize = PaneResizeKind::channels;
    check(!application_preferences_persist_pane_widths() && g_app.pane_widths_persist_pending &&
              !g_app.preferences.channel_pane_width,
          "settings: splitter widths never persist while the drag is still active");
    g_app.pane_resize = PaneResizeKind::none;
    check(application_settings_synchronize() && !g_app.pane_widths_persist_pending &&
              g_app.preferences.channel_pane_width == 300 && g_app.preferences.member_pane_width == 260 &&
              await_save() && !g_app.preferences_save_failed &&
              client_preferences_load_file(cleanup.path, &loaded).ok() && loaded.channel_pane_width == 300 &&
              loaded.member_pane_width == 260 && !application_preferences_persist_pane_widths(),
          "settings: a completed splitter drag persists both pane widths once, in whole DIPs");
    g_app.pane_widths_persist_pending = true;
    check(!application_preferences_persist_pane_widths() && !g_app.pane_widths_persist_pending &&
              !g_app.preferences_save_pending,
          "settings: an unchanged splitter release queues no preferences write");
    g_app.preferences.channel_pane_width = g_app.preferences.member_pane_width = 0;
    g_app.channel_pane_width = saved_channel_width;
    g_app.member_pane_width = saved_member_width;
    const auto before_flat_effects = g_app.preferences;
    const ClientTheme flat_bases[]{ClientTheme::dark, ClientTheme::amoled, ClientTheme::light};
    const UiThemeEditorElement flat_effect_cards[]{
        UiThemeEditorElement::effect_stars, UiThemeEditorElement::effect_comets,
        UiThemeEditorElement::effect_both};
    bool flat_effects_preserved = true;
    for (const auto base : flat_bases) {
        g_app.preferences = before_flat_effects;
        g_app.preferences.theme = base;
        g_app.preferences.gradient.effect = BackgroundEffect::none;
        application_preferences_apply();
        flat_effects_preserved = flat_effects_preserved && ui_settings_scene_select(scene(), kAccessibilityPage) &&
                                 application_settings_synchronize();
        for (u32 effect = 1; effect <= kBackgroundEffectProfileCount && flat_effects_preserved; ++effect) {
            const auto expected_effect = static_cast<BackgroundEffect>(effect);
            flat_effects_preserved =
                activate_control(kAppearancePage, cid("appearance.background_effect"), VK_RIGHT) && g_app.preferences.theme == base &&
                g_app.preferences.gradient.effect == expected_effect && await_save_retained() &&
                client_preferences_load_file(cleanup.path, &loaded).ok() && loaded.theme == base &&
                loaded.gradient.effect == expected_effect;
            const auto open_revision = g_app.preferences_request_revision;
            flat_effects_preserved = flat_effects_preserved && application_theme_editor_open() &&
                                     g_app.preferences.theme == base &&
                                     g_app.preferences_request_revision == open_revision;
            auto* flat_editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
            const auto range_control = effect % 3 == 0   ? UiThemeEditorElement::density
                                       : effect % 3 == 1 ? UiThemeEditorElement::speed
                                                         : UiThemeEditorElement::strength;
            flat_effects_preserved =
                flat_effects_preserved && flat_editor &&
                ui_theme_editor_focus(flat_editor, range_control) &&
                application_theme_editor_input(UiInputKind::key_down, VK_RIGHT, false) &&
                application_theme_editor_input(UiInputKind::key_up, VK_RIGHT, false) &&
                g_app.preferences.theme == base && application_settings_synchronize() &&
                await_save_retained();
            application_theme_editor_close(true);
            flat_effects_preserved = flat_effects_preserved && !application_theme_editor_active() &&
                                     application_theme_editor_open() && g_app.preferences.theme == base;
            flat_editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
            flat_effects_preserved =
                flat_effects_preserved && flat_editor &&
                ui_theme_editor_focus(flat_editor, UiThemeEditorElement::effect_none) &&
                application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
                application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
                g_app.preferences.theme == base &&
                g_app.preferences.gradient.effect == BackgroundEffect::none &&
                application_settings_synchronize() && await_save_retained() &&
                ui_theme_editor_focus(flat_editor, flat_effect_cards[effect - 1]) &&
                application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
                application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
                g_app.preferences.theme == base && g_app.preferences.gradient.effect == expected_effect &&
                application_settings_synchronize() && await_save_retained();
            const auto flat_cancel_baseline = g_app.preferences;
            const auto flat_cancel_revision = g_app.preferences_request_revision;
            flat_effects_preserved = flat_effects_preserved &&
                                     ui_theme_editor_focus(flat_editor, UiThemeEditorElement::strength) &&
                                     application_theme_editor_input(UiInputKind::key_down, VK_RIGHT, false);
            application_theme_editor_window_message(WM_CANCELMODE, 0, 0);
            flat_effects_preserved = flat_effects_preserved &&
                                     client_preferences_equal(g_app.preferences, flat_cancel_baseline) &&
                                     g_app.preferences_request_revision == flat_cancel_revision;
            application_theme_editor_close(true);
        }
        // Choices cycle with Left/Right; Home/End belong to slider/palette
        // controls. After Stars and comets, the next supported choice is None.
        flat_effects_preserved = flat_effects_preserved && activate_control(kAppearancePage, cid("appearance.background_effect"), VK_RIGHT) &&
                                 g_app.preferences.theme == base &&
                                 g_app.preferences.gradient.effect == BackgroundEffect::none &&
                                 await_save_retained();
        const bool opposite_light = base != ClientTheme::light;
        const auto opposite_flat = opposite_light ? ClientTheme::light : ClientTheme::dark;
        flat_effects_preserved = flat_effects_preserved && application_theme_editor_open();
        auto* mode_editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
        flat_effects_preserved =
            flat_effects_preserved && mode_editor &&
            ui_theme_editor_focus(mode_editor, opposite_light ? UiThemeEditorElement::light
                                                              : UiThemeEditorElement::dark) &&
            application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
            application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
            g_app.preferences.theme == opposite_flat && application_settings_synchronize() &&
            await_save_retained();
        const auto before_color_revision = g_app.preferences_request_revision;
        flat_effects_preserved =
            flat_effects_preserved && ui_theme_editor_focus(mode_editor, UiThemeEditorElement::intensity) &&
            application_theme_editor_input(UiInputKind::key_down, VK_RIGHT, false) &&
            g_app.preferences.theme ==
                (opposite_light ? ClientTheme::custom_light_gradient : ClientTheme::custom_gradient);
        application_theme_editor_close(false);
        flat_effects_preserved = flat_effects_preserved && g_app.preferences.theme == opposite_flat &&
                                 g_app.preferences_request_revision == before_color_revision;
    }
    check(flat_effects_preserved, "settings/editor effects: all three subtypes and motion ranges save/reopen "
                                  "on Dark AMOLED Light without opting into gradient; None restores flat");
    application_theme_editor_close(false);
    g_app.preferences = before_flat_effects;
    application_preferences_apply();
    check(ui_settings_scene_select(scene(), kAccessibilityPage) && application_settings_synchronize() &&
              ui_settings_scene_select(scene(), kAppearancePage) && application_settings_synchronize(),
          "settings effects: flat combinations restore original current Appearance projection");
    const ClientTheme named_effect_bases[]{ClientTheme::aurora, ClientTheme::sunset, ClientTheme::ocean};
    bool named_effects_preserve_raw = true;
    for (const auto base : named_effect_bases) {
        g_app.preferences = before_flat_effects;
        g_app.preferences.theme = base;
        g_app.preferences.gradient.colors[0] = 0x183752;
        g_app.preferences.gradient.colors[1] = 0x592a41;
        // The saved baseline has three stops. A two-stop fixture must clear
        // its unused tail, just as the real palette editor does.
        g_app.preferences.gradient.colors[2] = 0;
        g_app.preferences.gradient.colors[3] = 0;
        g_app.preferences.gradient.colors[4] = 0;
        g_app.preferences.gradient.color_count = 2;
        g_app.preferences.gradient.angle_degrees = 23;
        g_app.preferences.gradient.intensity = 47;
        g_app.preferences.gradient.effect = BackgroundEffect::none;
        const auto raw_palette = g_app.preferences.gradient;
        check(background_theme_valid(raw_palette),
              "settings/editor effects: named preset dormant two-stop seed is valid");
        named_effects_preserve_raw = named_effects_preserve_raw && background_theme_valid(raw_palette);
        const auto raw_palette_equal = [&](const BackgroundTheme& value) noexcept {
            return bytes_equal(value.colors, raw_palette.colors, sizeof(value.colors)) &&
                   value.color_count == raw_palette.color_count &&
                   value.angle_degrees == raw_palette.angle_degrees &&
                   value.intensity == raw_palette.intensity;
        };
        application_preferences_apply();
        named_effects_preserve_raw =
            named_effects_preserve_raw && ui_settings_scene_select(scene(), kAccessibilityPage) &&
            application_settings_synchronize() && activate_control(kAppearancePage, cid("appearance.background_effect"), VK_LEFT) &&
            g_app.preferences.theme == base &&
            g_app.preferences.gradient.effect == BackgroundEffect::stars_and_comets &&
            raw_palette_equal(g_app.preferences.gradient) && await_save_retained() &&
            client_preferences_load_file(cleanup.path, &loaded).ok() && loaded.theme == base &&
            raw_palette_equal(loaded.gradient) && application_theme_editor_open();
        auto* preset_editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
        named_effects_preserve_raw =
            named_effects_preserve_raw && preset_editor &&
            ui_theme_editor_focus(preset_editor, UiThemeEditorElement::speed) &&
            application_theme_editor_input(UiInputKind::key_down, VK_RIGHT, false) &&
            application_theme_editor_input(UiInputKind::key_up, VK_RIGHT, false) &&
            g_app.preferences.theme == base && raw_palette_equal(g_app.preferences.gradient) &&
            application_settings_synchronize() && await_save_retained() &&
            client_preferences_load_file(cleanup.path, &loaded).ok() && raw_palette_equal(loaded.gradient);
        const auto preset_cancel_baseline = g_app.preferences;
        const auto preset_cancel_revision = g_app.preferences_request_revision;
        named_effects_preserve_raw = named_effects_preserve_raw &&
                                     ui_theme_editor_focus(preset_editor, UiThemeEditorElement::strength) &&
                                     application_theme_editor_input(UiInputKind::key_down, VK_RIGHT, false);
        application_theme_editor_window_message(WM_CANCELMODE, 0, 0);
        named_effects_preserve_raw = named_effects_preserve_raw &&
                                     client_preferences_equal(g_app.preferences, preset_cancel_baseline) &&
                                     g_app.preferences_request_revision == preset_cancel_revision;
        application_theme_editor_close(true);
        named_effects_preserve_raw = named_effects_preserve_raw && application_theme_editor_open() &&
                                     g_app.preferences.theme == base &&
                                     raw_palette_equal(g_app.preferences.gradient);
        application_theme_editor_close(true);
    }
    check(named_effects_preserve_raw, "settings/editor effects: named presets retain dormant RGB count "
                                      "angle intensity on FX save cancel and reopen");
    application_theme_editor_close(false);
    g_app.preferences = before_flat_effects;
    application_preferences_apply();
    check(ui_settings_scene_select(scene(), kAccessibilityPage) && application_settings_synchronize() &&
              ui_settings_scene_select(scene(), kAppearancePage) && application_settings_synchronize(),
          "settings effects: named preset fixtures restore original Appearance projection");
    const auto original_gradient = g_app.preferences.gradient;
    const u32 gradient_layouts = observed.layout_updates, gradient_account = observed.account_text_updates;
    check(activate_control(kAppearancePage, cid("appearance.color_theme"), VK_RIGHT) &&
              activate_control(kAppearancePage, cid("appearance.color_theme"), VK_RIGHT) &&
              g_app.preferences.theme == ClientTheme::sunset &&
              application_theme().background.colors[0] == 0x7a284b &&
              activate_control(kAppearancePage, cid("appearance.gradient_direction"), VK_RIGHT) &&
              g_app.preferences.theme == ClientTheme::custom_gradient &&
              g_app.preferences.gradient.colors[0] == 0x7a284b &&
              g_app.preferences.gradient.angle_degrees == 121,
          "settings gradient: customizing a preset keeps its actual colors instead of dormant defaults");
    g_app.preferences.theme = ClientTheme::dark;
    g_app.preferences.gradient = original_gradient;
    check(ui_settings_scene_select(scene(), kAccessibilityPage) && application_settings_synchronize(),
          "settings gradient: controlled baseline change retires prior shared Appearance projection");
    check(activate_control(kAppearancePage, cid("appearance.theme"), VK_RIGHT), "settings gradient: base Appearance projection restores");
    check(activate_control(kAppearancePage, cid("appearance.background_effect"), VK_RIGHT) && g_app.preferences.theme == ClientTheme::amoled &&
              g_app.preferences.gradient.effect == BackgroundEffect::stars &&
              activate_control(kAppearancePage, cid("appearance.gradient_direction"), VK_RIGHT) &&
              g_app.preferences.theme == ClientTheme::custom_gradient &&
              g_app.preferences.gradient.angle_degrees == 121 && activate_control(kAppearancePage, cid("appearance.gradient_intensity"), VK_RIGHT) &&
              g_app.preferences.gradient.intensity == 41 && observed.layout_updates == gradient_layouts &&
              observed.account_text_updates == gradient_account,
          "settings gradient: effect, arbitrary direction and intensity reuse projection without hidden "
          "chat work");
    const auto* appearance = static_cast<const UiSettingsPageContent*>(
        ui_page_registry_find(*application_settings_pages(), kAppearancePage)->context);
    const auto find = [&](const char* key) noexcept -> const UiSettingsControl* {
        for (u32 i = 0; appearance && i < appearance->controls.count; ++i)
            if (appearance->controls[i].id == cid(key))
                return &appearance->controls[i];
        return nullptr;
    };
    const auto* base = find("appearance.theme");
    const auto* colors = find("appearance.color_theme");
    const auto* effect = find("appearance.background_effect");
    const auto* direction = find("appearance.gradient_direction");
    const auto* intensity = find("appearance.gradient_intensity");
    bool angles = base && colors && effect && direction && intensity && base->choices.count == 3 &&
                  colors->choices.count == 5 && effect->choices.count == kBackgroundEffectCount &&
                  utf8_equal(effect->choices[0], utf8_literal("None")) &&
                  utf8_equal(effect->choices[1], utf8_literal("Stars")) &&
                  utf8_equal(effect->choices[2], utf8_literal("Comets")) &&
                  utf8_equal(effect->choices[3], utf8_literal("Stars and comets")) && direction->choices.count == 361 &&
                  intensity->choices.count == 101;
    for (u32 angle = 0; angles && angle <= 360; ++angle) {
        const auto label = direction->choices[angle];
        u32 value = 0, digits = 0;
        while (digits < label.bytes && label.data[digits] >= '0' && label.data[digits] <= '9')
            value = value * 10 + static_cast<u32>(label.data[digits++] - '0');
        angles = digits && value == angle && utf8_equal({label.data + digits, label.bytes - digits},
                                                        utf8_literal("°"));
    }
    check(angles, "settings gradient: native sliders borrow every exact integer0..360 angle");
    check(activate_control(kAppearancePage, cid("appearance.customize")) && application_theme_editor_active() &&
              g_app.renderer.theme_editor_scene == application_theme_editor_scene(),
          "theme editor host: actual Appearance action opens bounded visual drawer");
    auto* editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
    measurement("MEASURE theme_editor_owner_record_bytes=", application_theme_editor_owner_bytes());
    if (editor) {
        measurement("MEASURE theme_editor_storage_logical_bytes=",
                    kUiThemeEditorElements * (sizeof(UiWidget) + sizeof(UiLayoutNode)));
        MEMORY_BASIC_INFORMATION editor_storage_region{};
        if (VirtualQuery(editor->storage.arena.base, &editor_storage_region,
                         sizeof(editor_storage_region)) == sizeof(editor_storage_region))
            measurement("MEASURE theme_editor_storage_committed_bytes=", editor_storage_region.RegionSize);
    }
    const HWND previous_window = g_app.window;
    const u32 previous_dpi = g_app.renderer.dpi;
    HWND editor_window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1000, 700, nullptr, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
    g_app.window = editor_window;
    g_app.renderer.dpi = 96;
    bool drag = editor && editor_window && ui_theme_editor_focus(editor, UiThemeEditorElement::sv);
    const auto* sv = drag ? ui_theme_editor_node(*editor, UiThemeEditorElement::sv) : nullptr;
    const auto pack = [](float x, float y) noexcept {
        return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
    };
    const u64 before_drag_save = g_app.preferences_request_revision;
    const u32 before_drag_text = observed.text_builds;
    const u32 before_drag_forced = observed.forced_account_text_updates;
    const u64 before_drag_foreground = content_text_theme_signature(application_theme());
    const u16 before_drag_generation = editor ? editor->widgets.generation : 0;
    if (sv) {
        const UiRect rect = sv->rectangle;
        drag = application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                       pack(rect.left + 8, rect.top + 8)) &&
               GetCapture() == editor_window;
        for (u32 i = 0; i < 64 && drag; ++i)
            drag = application_theme_editor_window_message(
                WM_MOUSEMOVE, MK_LBUTTON, pack(rect.left + 8 + static_cast<float>(i) * 2, rect.top + 20));
        drag = drag && g_app.preferences_request_revision == before_drag_save &&
               observed.text_builds == before_drag_text &&
               observed.forced_account_text_updates == before_drag_forced &&
               editor->widgets.generation == before_drag_generation;
        check(drag, "theme editor host: sixty-four captured HSV moves preview without saves or text/layout "
                    "rebuild");
        const u32 palette_publications =
            content_text_theme_signature(application_theme()) != before_drag_foreground ? 1u : 0u;
        check(drag && application_theme_editor_publish_foreground() &&
                  observed.forced_account_text_updates == before_drag_forced + palette_publications &&
                  application_theme_editor_publish_foreground() &&
                  observed.forced_account_text_updates == before_drag_forced + palette_publications &&
                  g_app.preferences_request_revision == before_drag_save,
              "theme editor host: frame publication coalesces latest palette foreground once and retains "
              "unchanged repeats");
        check(application_theme_editor_window_message(WM_LBUTTONUP, 0,
                                                      pack(rect.left + 134, rect.top + 20)) &&
                  GetCapture() != editor_window &&
                  g_app.preferences_request_revision == before_drag_save + 1 &&
                  application_settings_synchronize() && await_save_retained(),
              "theme editor host: actual capture release commits one latest draft");
    } else
        check(false, "theme editor host: actual HSV drag geometry and hidden capture window initialize");
    // Intensity changes the foreground anchor even when a selected HSV stop
    // does not change which other stop is the darkest in a multi-stop palette.
    const auto intensity_baseline = g_app.preferences;
    const u64 intensity_revision = g_app.preferences_request_revision;
    const u32 intensity_forced = observed.forced_account_text_updates;
    bool coalesced_intensity = editor && ui_theme_editor_focus(editor, UiThemeEditorElement::intensity);
    const auto* intensity_node =
        coalesced_intensity ? ui_theme_editor_node(*editor, UiThemeEditorElement::intensity) : nullptr;
    if (intensity_node) {
        const auto r = intensity_node->rectangle;
        coalesced_intensity = application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                                      pack(r.left + 9, r.bottom - 14));
        for (u32 i = 0; i < 64 && coalesced_intensity; ++i)
            coalesced_intensity = application_theme_editor_window_message(
                WM_MOUSEMOVE, MK_LBUTTON,
                pack(r.left + 9 + (r.right - r.left - 29) * static_cast<float>(i) / 63.f, r.bottom - 14));
        coalesced_intensity = coalesced_intensity &&
                              observed.forced_account_text_updates == intensity_forced &&
                              g_app.preferences_request_revision == intensity_revision &&
                              application_theme_editor_publish_foreground() &&
                              observed.forced_account_text_updates == intensity_forced + 1 &&
                              application_theme_editor_publish_foreground() &&
                              observed.forced_account_text_updates == intensity_forced + 1;
        application_theme_editor_window_message(WM_CANCELMODE, 0, 0);
        coalesced_intensity = coalesced_intensity &&
                              client_preferences_equal(g_app.preferences, intensity_baseline) &&
                              application_theme_editor_publish_foreground() &&
                              observed.forced_account_text_updates == intensity_forced + 2 &&
                              g_app.preferences_request_revision == intensity_revision;
    }
    check(coalesced_intensity, "theme editor host: sixty-four intensity previews coalesce real foreground "
                               "once per frame and cancel publishes restored tokens without saves");
    constexpr UINT editor_cancel_messages[]{WM_SIZE, WM_KILLFOCUS};
    for (const UINT cancel_message : editor_cancel_messages) {
        const auto gesture_baseline = g_app.preferences;
        const u64 gesture_revision = g_app.preferences_request_revision;
        const u32 gesture_forced = observed.forced_account_text_updates;
        bool canceled_effect = editor && ui_theme_editor_focus(editor, UiThemeEditorElement::strength);
        const auto* strength_node =
            canceled_effect ? ui_theme_editor_node(*editor, UiThemeEditorElement::strength) : nullptr;
        if (strength_node) {
            const auto r = strength_node->rectangle;
            canceled_effect = application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                                      pack(r.left + 9, r.bottom - 14)) &&
                              GetCapture() == editor_window;
            const u32 gesture_text_builds = observed.text_builds;
            for (u32 i = 0; i < 64 && canceled_effect; ++i)
                canceled_effect = application_theme_editor_window_message(
                    WM_MOUSEMOVE, MK_LBUTTON, pack(r.left + 9 + static_cast<float>(i), r.bottom - 14));
            canceled_effect = canceled_effect &&
                              g_app.preferences.gradient.strength != gesture_baseline.gradient.strength &&
                              observed.text_builds == gesture_text_builds &&
                              g_app.preferences_request_revision == gesture_revision;
            application_theme_editor_window_message(cancel_message, 0, 0);
            canceled_effect =
                canceled_effect && client_preferences_equal(g_app.preferences, gesture_baseline) &&
                g_app.preferences_request_revision == gesture_revision &&
                observed.forced_account_text_updates == gesture_forced && GetCapture() != editor_window;
        }
        check(canceled_effect, "theme editor host: sixty-four effect moves cancel exact profile draft on "
                               "resize or focus loss without writer or Main reshape");
    }
    bool released_effect = editor && ui_theme_editor_focus(editor, UiThemeEditorElement::strength);
    const auto* released_strength =
        released_effect ? ui_theme_editor_node(*editor, UiThemeEditorElement::strength) : nullptr;
    const u64 effect_write = g_app.preferences_request_revision;
    if (released_strength) {
        const auto r = released_strength->rectangle;
        released_effect = application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                                  pack(r.left + 9, r.bottom - 14));
        for (u32 i = 0; i < 64 && released_effect; ++i)
            released_effect = application_theme_editor_window_message(WM_MOUSEMOVE, MK_LBUTTON,
                                                                      pack(r.right - 20, r.bottom - 14));
        const u8 retained_strength = g_app.preferences.gradient.strength;
        released_effect =
            released_effect && g_app.preferences_request_revision == effect_write &&
            application_theme_editor_window_message(WM_LBUTTONUP, 0, pack(r.right - 20, r.bottom - 14)) &&
            g_app.preferences_request_revision == effect_write + 1 && application_settings_synchronize() &&
            await_save_retained();
        released_effect = released_effect &&
                          ui_theme_editor_focus(editor, UiThemeEditorElement::effect_comets) &&
                          application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
                          application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
                          g_app.preferences.gradient.effect == BackgroundEffect::comets &&
                          g_app.preferences.gradient.strength ==
                              background_effect_settings_defaults(BackgroundEffect::comets).strength &&
                          application_settings_synchronize() && await_save_retained() &&
                          ui_theme_editor_focus(editor, UiThemeEditorElement::effect_stars) &&
                          application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
                          application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
                          g_app.preferences.gradient.effect == BackgroundEffect::stars &&
                          g_app.preferences.gradient.strength == retained_strength &&
                          g_app.preferences.effect_profiles[0].strength == retained_strength &&
                          application_settings_synchronize() && await_save_retained();
    }
    check(released_effect, "theme editor host: sixty-four effect previews write once on release and "
                           "subtype switches restore saved profile");
    const auto rapid_effect_write = g_app.preferences_request_revision;
    const auto click_effect = [&](UiThemeEditorElement element, UINT press) noexcept {
        if (!editor || !ui_theme_editor_focus(editor, element))
            return false;
        const auto* effect_node = ui_theme_editor_node(*editor, element);
        if (!effect_node)
            return false;
        const auto location = pack((effect_node->rectangle.left + effect_node->rectangle.right) * .5f,
                                   (effect_node->rectangle.top + effect_node->rectangle.bottom) * .5f);
        return application_theme_editor_window_message(press, MK_LBUTTON, location) &&
               GetCapture() == editor_window &&
               application_theme_editor_window_message(WM_LBUTTONUP, 0, location) &&
               GetCapture() != editor_window;
    };
    check(click_effect(UiThemeEditorElement::effect_comets, WM_LBUTTONDOWN) &&
              g_app.preferences.gradient.effect == BackgroundEffect::comets &&
              application_settings_synchronize() && await_save_retained() &&
              click_effect(UiThemeEditorElement::effect_stars, WM_LBUTTONDBLCLK) &&
              g_app.preferences.gradient.effect == BackgroundEffect::stars &&
              g_app.preferences_request_revision == rapid_effect_write + 2 &&
              application_settings_synchronize() && await_save_retained(),
          "theme editor host: native rapid effect presses activate and persist both release actions");
    const auto both_write_revision = g_app.preferences_request_revision;
    u8 both_record[kClientPreferencesRecordBytes]{};
    ClientPreferences both_decoded{};
    check(editor && ui_theme_editor_focus(editor, UiThemeEditorElement::effect_both) &&
              application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
              application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
              g_app.preferences.gradient.effect == BackgroundEffect::stars_and_comets &&
              g_app.preferences_request_revision == both_write_revision + 1 &&
              g_app.preferences.gradient.speed == 28 && g_app.preferences.gradient.strength == 18 &&
              g_app.preferences.gradient.density == 22 &&
              g_app.preferences.effect_profiles[2].speed == 28 &&
              g_app.preferences.effect_profiles[2].strength == 18 &&
              g_app.preferences.effect_profiles[2].density == 22 && application_settings_synchronize() &&
              await_save_retained() &&
              client_preferences_encode(g_app.preferences, {both_record, sizeof(both_record)})
                  .ok() &&
              client_preferences_decode({both_record, sizeof(both_record)}, &both_decoded)
                  .ok() &&
              client_preferences_equal(both_decoded, g_app.preferences) &&
              ui_theme_editor_focus(editor, UiThemeEditorElement::effect_stars) &&
              application_theme_editor_input(UiInputKind::key_down, VK_RETURN, false) &&
              application_theme_editor_input(UiInputKind::key_up, VK_RETURN, false) &&
              application_settings_synchronize() && await_save_retained(),
          "theme editor host: Stars and comets card commits one save with exact third-profile "
          "roundtrip");
    if (editor) {
        ui_theme_editor_focus(editor, UiThemeEditorElement::sv);
        test_modal_scroll_binding(application_theme_editor_scroll_binding(),
                                  "theme editor scrolling: controls use smooth frames and immediate "
                                  "preference with stable lifetime");
        const auto scrollbar = ui_theme_editor_scrollbar(*editor);
        const u64 thumb_revision = g_app.preferences_request_revision;
        const auto thumb_baseline = g_app.preferences;
        const float x = (scrollbar.thumb.left + scrollbar.thumb.right) * .5f;
        const float y = scrollbar.thumb.top + 4;
        bool thumb_drag = scrollbar.visible &&
                          application_theme_editor_window_message(WM_MOUSEMOVE, 0, pack(x, y)) &&
                          editor->widgets.hovered.slot ==
                              editor->elements[static_cast<u32>(UiThemeEditorElement::scrollbar)].slot &&
                          application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON, pack(x, y)) &&
                          GetCapture() == editor_window;
        thumb_drag =
            thumb_drag &&
            application_theme_editor_window_message(WM_MOUSEMOVE, MK_LBUTTON,
                                                    pack(x, scrollbar.track.bottom - 4)) &&
            ui_theme_editor_scrollbar(*editor).thumb.top > scrollbar.thumb.top &&
            application_theme_editor_window_message(WM_LBUTTONUP, 0, pack(x, scrollbar.track.bottom - 4));
        check(
            thumb_drag && GetCapture() != editor_window &&
                g_app.preferences_request_revision == thumb_revision &&
                client_preferences_equal(g_app.preferences, thumb_baseline),
            "theme editor host: real native thumb hover capture drag scrolls fields without theme writes");
    }
    auto editor_key = [](u32 key) noexcept {
        return application_theme_editor_input(UiInputKind::key_down, key, false) &&
               application_theme_editor_input(UiInputKind::key_up, key, false);
    };
    const u32 hex_pointer_opens = gradient_form.opens;
    const auto hex_pointer_baseline = g_app.preferences;
    bool hex_pointer = editor && editor_window && ui_theme_editor_focus(editor, UiThemeEditorElement::hex);
    const auto* hex_node = hex_pointer ? ui_theme_editor_node(*editor, UiThemeEditorElement::hex) : nullptr;
    const UiRect hex_rectangle =
        hex_node ? ui_rect_intersection(hex_node->rectangle, hex_node->clip_rectangle) : UiRect{};
    const LPARAM hex_position = pack((hex_rectangle.left + hex_rectangle.right) * .5f,
                                     (hex_rectangle.top + hex_rectangle.bottom) * .5f);
    hex_pointer = hex_pointer && ui_rect_has_area(hex_rectangle) &&
                  application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON, hex_position) &&
                  GetCapture() == editor_window && !gradient_form.active &&
                  gradient_form.opens == hex_pointer_opens &&
                  application_theme_editor_window_message(WM_LBUTTONUP, 0, hex_position) &&
                  GetCapture() != editor_window && gradient_form.active &&
                  gradient_form.opens == hex_pointer_opens + 1 && gradient_form.request.valid &&
                  gradient_form.request.valid(gradient_form.request.owner) &&
                  utf8_equal(gradient_form.request.initial, {editor->hex, 7}) &&
                  client_preferences_equal(hex_pointer_baseline, g_app.preferences);
    check(hex_pointer, "theme editor host: actual hex mouse release opens current scoped native edit "
                       "request once without changing draft");
    gradient_form.active = false; // The TextForm boundary models explicit Cancel.
    g_app.context_focus = nullptr;
    check(application_theme_editor_synchronize() && editor && g_app.context_focus == &editor->widgets &&
              client_preferences_equal(hex_pointer_baseline, g_app.preferences),
          "theme editor host: canceled hex request restores drawer focus without applying a color");
    const u32 hex_canceled_opens = gradient_form.opens;
    check(application_theme_editor_window_message(WM_LBUTTONDOWN, MK_LBUTTON, hex_position) &&
              application_theme_editor_window_message(WM_CANCELMODE, 0, 0) == false &&
              application_theme_editor_window_message(WM_LBUTTONUP, 0, hex_position) &&
              !gradient_form.active && gradient_form.opens == hex_canceled_opens &&
              GetCapture() != editor_window,
          "theme editor host: canceled hex pointer capture cannot open its editor on late release");
    check(application_theme_editor_window_message(WM_LBUTTONDBLCLK, MK_LBUTTON, hex_position) &&
              !gradient_form.active &&
              application_theme_editor_window_message(WM_LBUTTONUP, 0, hex_position) &&
              gradient_form.active && gradient_form.opens == hex_canceled_opens + 1,
          "theme editor host: native second-click message preserves release-only hex activation");
    gradient_form.active = false;
    check(editor && ui_theme_editor_focus(editor, UiThemeEditorElement::hex) && editor_key(VK_RETURN) &&
              gradient_form.active && gradient_form.request.valid &&
              gradient_form.request.valid(gradient_form.request.owner),
          "theme editor host: optional single-color form has current drawer authority");
    const auto old_hex = gradient_form.request;
    const auto before_hex = g_app.preferences;
    u32 swatch = 0;
    check(old_hex.valid && old_hex.save && !old_hex.save(old_hex.owner, utf8_literal("#12345")) &&
              !old_hex.save(old_hex.owner, utf8_literal("112233,445566")) && old_hex.check && old_hex.swatch &&
              !old_hex.check(old_hex.owner, utf8_literal("#12345"), &swatch) &&
              old_hex.check(old_hex.owner, utf8_literal("1a2"), &swatch) && swatch == 0x11AA22FFu &&
              client_preferences_equal(before_hex, g_app.preferences),
          "theme editor host: invalid RGB retains complete draft; the live check previews #RGB");
    check(old_hex.save && old_hex.save(old_hex.owner, utf8_literal("#123")) &&
              g_app.preferences.gradient.colors[editor ? editor->selected_stop : 0] == 0x112233 &&
              g_app.preferences.gradient.angle_degrees == 121 &&
              g_app.preferences.gradient.intensity == 41 && application_settings_synchronize() &&
              await_save_retained(),
          "theme editor host: optional hex changes only selected color and commits once");
    gradient_form.active = false;
    g_app.context_focus = nullptr; // Actual TextForm retires a now-stale saved callback.
    check(application_theme_editor_synchronize() && editor && g_app.context_focus == &editor->widgets &&
              old_hex.valid && !old_hex.valid(old_hex.owner),
          "theme editor host: saved form retires old callback and returns keyboard focus");
    const u64 first_editor_lifetime = g_app.theme_editor_lifetime_serial;
    application_theme_editor_close(true);
    check(application_theme_editor_open() && g_app.theme_editor_lifetime_serial > first_editor_lifetime &&
              old_hex.valid && !old_hex.valid(old_hex.owner),
          "theme editor host: close and reopen cannot retarget captured hex callback");
    editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
    const u32 before_light_forced = observed.forced_account_text_updates;
    check(editor && ui_theme_editor_focus(editor, UiThemeEditorElement::light) && editor_key(VK_RETURN) &&
              g_app.preferences.theme == ClientTheme::custom_light_gradient &&
              observed.forced_account_text_updates == before_light_forced &&
              application_theme_editor_publish_foreground() &&
              observed.forced_account_text_updates == before_light_forced + 1 &&
              application_settings_synchronize() && await_save_retained(),
          "theme editor host: Light segment commits actual light gradient mode");
    constexpr u32 close_keys[]{VK_RETURN, VK_SPACE, VK_ESCAPE};
    for (const u32 close_key : close_keys) {
        if (!application_theme_editor_active())
            application_theme_editor_open();
        editor = const_cast<UiThemeEditorScene*>(application_theme_editor_scene());
        const bool focus_apply = editor && ui_theme_editor_focus(editor, UiThemeEditorElement::apply);
        const u64 revision = g_app.preferences_request_revision;
        const bool closed = focus_apply &&
                            application_theme_editor_input(UiInputKind::key_down, close_key, false) &&
                            !application_theme_editor_active();
        check(closed && application_settings_input(UiInputKind::key_down, close_key, true) &&
                  application_settings_input(UiInputKind::key_up, close_key, false) &&
                  application_settings_active() && !application_theme_editor_active() &&
                  !g_app.settings_held_key && g_app.preferences_request_revision == revision,
              "theme editor host: terminating Return Space Escape tails cannot reopen or dismiss Settings");
    }
    check(application_theme_editor_open(), "theme editor host: stale-scope cleanup fixture opens");
    const auto newer_preferences = g_app.preferences;
    ++g_app.plugin_account_scope_serial;
    check(application_theme_editor_synchronize() && !g_app.theme_editor &&
              !g_app.renderer.theme_editor_scene &&
              client_preferences_equal(newer_preferences, g_app.preferences),
          "theme editor host: stale scope retires owner without overwriting newer preferences");
    --g_app.plugin_account_scope_serial;
    if (editor_window)
        DestroyWindow(editor_window);
    g_app.window = previous_window;
    g_app.renderer.dpi = previous_dpi;
    g_app.preferences.gradient = original_gradient;
    g_app.preferences.theme = ClientTheme::dark;
    check(ui_settings_scene_select(scene(), kAccessibilityPage) && application_settings_synchronize(),
          "theme editor host: controlled restored baseline republishes shared native bank");
    check(activate_control(kAppearancePage, cid("appearance.theme"), VK_RIGHT) && g_app.preferences.theme == ClientTheme::amoled &&
              await_save_retained(),
          "settings gradient: original palette and preferences can be restored through ordinary choice "
          "input");
    const u32 member_layouts = observed.layout_updates;
    check(activate_control(kAppearancePage, cid("appearance.member_list")) && !g_app.preferences.member_pane_visible &&
              !g_app.member_pane_visible && observed.layout_updates == member_layouts + 1,
          "settings: member-pane toggle updates visibility and synchronizes live shell geometry");
    check(await_save_retained() && !g_app.preferences_save_failed,
          "settings: member-pane save retains current Appearance scene");
    g_app.renderer.content_text_retention.valid = true;
    const u32 invalidations = g_app.renderer.content_text_retention.invalidations;
    const u32 motion_layouts = observed.layout_updates;
    const u32 motion_account_text = observed.account_text_updates;
    check(activate_control(kAccessibilityPage, cid("accessibility.reduced_motion")) && g_app.preferences.reduced_motion &&
              g_app.renderer.reduced_motion && g_app.scroll_host.immediate &&
              g_app.renderer.content_text_retention.invalidations > invalidations &&
              observed.layout_updates == motion_layouts &&
              observed.account_text_updates == motion_account_text,
          "settings: reduced motion applies immediate scrolling and animated text invalidation without "
          "hidden shell rebuild");
    check(await_save_retained(),
          "settings: Accessibility save completion preserves dormant Storage and current scene");
    check(activate_control(kAccessibilityPage, cid("accessibility.smooth_scroll")) && !g_app.preferences.smooth_scroll && g_app.scroll_host.immediate,
          "settings: smooth-scroll control applies while reduced motion remains enabled");
    check(await_save_retained(), "settings: smooth-scroll completion retains current scene");
    check(activate_control(kAccessibilityPage, cid("accessibility.reduced_motion")) && !g_app.preferences.reduced_motion &&
              !g_app.renderer.reduced_motion && g_app.scroll_host.immediate && await_save(),
          "settings: disabling reduced motion retains disabled smooth-scroll policy");
    check(activate_control(kAccessibilityPage, cid("accessibility.smooth_scroll")) && g_app.preferences.smooth_scroll && !g_app.scroll_host.immediate &&
              await_save(),
          "settings: enabling smooth scrolling restores animated scroll policy");
    check(observed.layout_updates == motion_layouts && observed.account_text_updates == motion_account_text,
          "settings: motion and scrolling-only changes never rebuild hidden shell or account text");
    const auto before_notifications = g_app.preferences;
    const u32 setup_before = observed.notifications_setups;
    const u32 refresh_before = observed.notifications_refreshes;
    check(ui_settings_scene_select(scene(), kNotificationsPage) && application_settings_synchronize() &&
              ui_page_registry_find(*application_settings_pages(), kNotificationsPage) &&
              scene()->content.controls.count == 11 &&
              utf8_equal(scene()->content.title, utf8_literal("Notifications")) &&
              observed.notifications_refreshes == refresh_before + 1 &&
              observed.notifications_setups == setup_before &&
              client_preferences_equal(before_notifications, g_app.preferences),
          "settings: stable Notifications page opens with read-only status and no setup or preference "
          "change");
    const auto* desktop = ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.desktop")});
    const auto* sounds = ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.message_sounds")});
    const auto* windows = ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.windows")});
    check(desktop && sounds && windows && desktop->kind == UiSettingsControlKind::toggle &&
              desktop->checked && sounds->kind == UiSettingsControlKind::toggle && sounds->checked &&
              windows->kind == UiSettingsControlKind::action &&
              utf8_equal(windows->action_label, utf8_literal("Open settings")),
          "settings: desktop and sound choices expose independent enabled controls and explicit Windows "
          "action");
    const u32 settled_refreshes = observed.notifications_refreshes;
    check(application_settings_synchronize() && application_settings_synchronize() &&
              observed.notifications_refreshes == settled_refreshes &&
              observed.notifications_setups == setup_before,
          "settings: retained Notifications synchronization does not poll or enroll repeatedly");
    check(activate_control(kNotificationsPage, cid("notifications.desktop")) && !g_app.preferences.desktop_enabled &&
              g_app.preferences.message_sounds_enabled && await_save_retained() &&
              client_preferences_load_file(cleanup.path, &loaded).ok() &&
              client_preferences_equal(loaded, g_app.preferences),
          "settings: desktop toggle queues existing writer independently while sound remains enabled");
    check(activate_control(kNotificationsPage, cid("notifications.message_sounds")) && !g_app.preferences.desktop_enabled &&
              !g_app.preferences.message_sounds_enabled && await_save_retained(),
          "settings: sound toggle saves independently of disabled desktop notifications");
    check(activate_control(kNotificationsPage, cid("notifications.desktop")) && g_app.preferences.desktop_enabled &&
              !g_app.preferences.message_sounds_enabled && await_save_retained(),
          "settings: desktop notifications can be enabled while message sound remains disabled");
    check(activate_control(kNotificationsPage, cid("notifications.message_sounds")) && g_app.preferences.message_sounds_enabled && await_save_retained(),
          "settings: sound choice can be restored independently");
    const auto* selected_sounds =
        ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.selected_channel_sounds")});
    const auto* volume = ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.volume")});
    bool volume_labels_valid = volume && volume->kind == UiSettingsControlKind::slider &&
                               volume->selected_choice == 100 && volume->choices.count == 201;
    for (u32 i = 0; volume_labels_valid && i <= 200; ++i) {
        char expected[5]{};
        u32 bytes = 0;
        if (i >= 100)
            expected[bytes++] = static_cast<char>('0' + i / 100);
        if (i >= 10)
            expected[bytes++] = static_cast<char>('0' + (i / 10) % 10);
        expected[bytes++] = static_cast<char>('0' + i % 10);
        expected[bytes++] = '%';
        volume_labels_valid = utf8_equal(volume->choices[i], {expected, bytes});
    }
    check(selected_sounds && !selected_sounds->checked && volume_labels_valid,
          "settings: selected channel sound default is off and every exact volume label from 0 to 200 is "
          "present");
    check(activate_control(kNotificationsPage, cid("notifications.selected_channel_sounds")) && g_app.preferences.selected_message_sounds_enabled &&
              await_save_retained(),
          "settings: selected channel sounds toggles through keyboard and persists independently");
    check(activate_control(kNotificationsPage, cid("notifications.volume"), VK_END) && g_app.preferences.output_volume == 200 &&
              !g_app.popup_scene.active && await_save_retained() &&
              client_preferences_load_file(cleanup.path, &loaded).ok() &&
              client_preferences_equal(loaded, g_app.preferences),
          "settings: native volume slider End saves exact 200 percent and selected sounds");
    const u64 maximum_volume_revision = g_app.preferences_request_revision;
    check(activate_control(kNotificationsPage, cid("notifications.volume"), VK_RIGHT) && g_app.preferences.output_volume == 200 &&
              g_app.preferences_request_revision == maximum_volume_revision,
          "settings: native slider maximum clamps without duplicate write or wrap");
    check(activate_control(kNotificationsPage, cid("notifications.volume"), VK_HOME) && g_app.preferences.output_volume == 0 &&
              await_save_retained(),
          "settings: native volume slider Home commits exact mute");
    const HWND volume_previous_window = g_app.window;
    const u32 volume_previous_dpi = g_app.renderer.dpi;
    HWND volume_window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1000, 700, nullptr, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
    g_app.window = volume_window;
    g_app.renderer.dpi = 96;
    const bool desktop_before_rapid = g_app.preferences.desktop_enabled;
    const auto desktop_rapid_revision = g_app.preferences_request_revision;
    const auto click_desktop = [&](UINT press) noexcept {
        const UiSettingsTarget target{UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.desktop")};
        UiRect bounds{};
        auto* current = const_cast<UiSettingsScene*>(scene());
        if (!current || !ui_settings_scene_focus(current, target) ||
            !ui_settings_scene_control_anchor(*current, target, &bounds))
            return false;
        const auto location = pack((bounds.left + bounds.right) * .5f, (bounds.top + bounds.bottom) * .5f);
        return application_settings_window_message(press, MK_LBUTTON, location) &&
               application_settings_window_message(WM_LBUTTONUP, 0, location) &&
               application_settings_synchronize() && await_save_retained();
    };
    check(volume_window && click_desktop(WM_LBUTTONDOWN) &&
              g_app.preferences.desktop_enabled != desktop_before_rapid &&
              click_desktop(WM_LBUTTONDBLCLK) &&
              g_app.preferences.desktop_enabled == desktop_before_rapid &&
              g_app.preferences_request_revision == desktop_rapid_revision + 2 &&
              GetCapture() != volume_window,
          "settings host: rapid native toggle pair commits both clicks and restores initial value");
    check(ui_settings_scene_focus(const_cast<UiSettingsScene*>(scene()),
                                  {UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.volume")}),
          "settings host: rapid toggle fixture restores the following volume gesture geometry");
    constexpr UINT cancel_messages[]{WM_SIZE, WM_KILLFOCUS};
    for (const UINT cancel_message : cancel_messages) {
        const UiSettingsTarget volume_target{UiSettingsTargetKind::control, kNotificationsPage, cid("notifications.volume")};
        UiSettingsSliderGeometry geometry{};
        const auto baseline = g_app.preferences;
        const u64 revision = g_app.preferences_request_revision;
        bool gesture = volume_window && ui_settings_scene_focus(scene(), volume_target) &&
                       ui_settings_scene_slider_geometry(*scene(), volume_target, &geometry);
        if (gesture) {
            const float y = (geometry.track.top + geometry.track.bottom) * .5f;
            const float start_x = geometry.track.left + ui_rect_width(geometry.track) * .25f;
            gesture = application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, pack(start_x, y)) &&
                      GetCapture() == volume_window;
            const u32 gesture_builds = observed.text_builds;
            for (u32 i = 0; i < 64 && gesture; ++i)
                gesture = application_settings_window_message(WM_MOUSEMOVE, MK_LBUTTON,
                                                              pack(start_x + static_cast<float>(i), y));
            gesture = gesture && g_app.preferences.output_volume != baseline.output_volume &&
                      g_app.preferences_request_revision == revision &&
                      observed.text_builds == gesture_builds;
            application_settings_window_message(cancel_message, 0, 0);
            gesture = gesture && GetCapture() != volume_window &&
                      client_preferences_equal(g_app.preferences, baseline) &&
                      g_app.preferences_request_revision == revision;
        }
        check(gesture, "settings slider host: sixty-four actual captured moves roll back on resize or "
                       "focus loss without saves");
    }
    const auto before_preset_gesture = g_app.preferences;
    g_app.preferences.theme = ClientTheme::ocean;
    application_preferences_apply();
    const auto preset_baseline = g_app.preferences;
    const UiSettingsTarget direction_target{UiSettingsTargetKind::control, kAppearancePage, cid("appearance.gradient_direction")};
    UiSettingsSliderGeometry direction_geometry{};
    bool preset_cancel = volume_window && ui_settings_scene_select(scene(), kAppearancePage) &&
                         application_settings_synchronize() &&
                         ui_settings_scene_focus(scene(), direction_target) &&
                         ui_settings_scene_slider_geometry(*scene(), direction_target, &direction_geometry);
    const u64 preset_save_revision = g_app.preferences_request_revision;
    if (preset_cancel) {
        const float x = direction_geometry.track.right - 2;
        const float y = (direction_geometry.track.top + direction_geometry.track.bottom) * .5f;
        preset_cancel = application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, pack(x, y)) &&
                        g_app.preferences.theme == ClientTheme::custom_gradient;
        application_settings_window_message(WM_CANCELMODE, 0, 0);
        preset_cancel = preset_cancel && client_preferences_equal(g_app.preferences, preset_baseline) &&
                        g_app.preferences_request_revision == preset_save_revision &&
                        GetCapture() != volume_window;
    }
    check(preset_cancel, "settings slider host: canceled preset customization restores full original theme "
                         "and dormant gradient");
    g_app.preferences = before_preset_gesture;
    application_preferences_apply();
    ui_settings_scene_select(scene(), kNotificationsPage);
    application_settings_synchronize();
    if (volume_window)
        DestroyWindow(volume_window);
    g_app.window = volume_previous_window;
    g_app.renderer.dpi = volume_previous_dpi;
    check(!ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kNotificationsPage, 0xFFFFFF}),
          "settings: obsolete sound-pack selector is absent while original alerts retain volume and focus "
          "controls");
    const u64 setup_revision = g_app.preferences_request_revision;
    const auto setup_preferences = g_app.preferences;
    check(activate_control(kNotificationsPage, cid("notifications.setup")) && observed.notifications_setups == setup_before + 1 &&
              g_app.preferences_request_revision == setup_revision &&
              client_preferences_equal(setup_preferences, g_app.preferences),
          "settings: only explicit setup activation reaches notification seam and does not save or send a "
          "test toast");
    const auto native_snapshot = g_app.preferences;
    check(ui_settings_scene_select(scene(), kAppearancePage) && application_settings_synchronize() &&
              ui_settings_scene_focus(scene(), {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.member_list")}) &&
              application_settings_input(UiInputKind::key_down, VK_SPACE, false),
          "settings: Space activates focused native toggle once");
    const auto held_snapshot = g_app.preferences;
    check(!client_preferences_equal(native_snapshot, held_snapshot) &&
              application_settings_input(UiInputKind::key_down, VK_SPACE, true) &&
              application_settings_input(UiInputKind::character, ' ', false) &&
              application_settings_input(UiInputKind::key_up, VK_SPACE, false) &&
              client_preferences_equal(held_snapshot, g_app.preferences) && !g_app.settings_held_key &&
              await_save(),
          "settings: held activation key repeat, character and release tails cannot toggle twice");
    const auto selected = scene()->selected_page;
    const auto focus = scene()->focus;
    const u32 old_revision = scene()->revision;
    observed.viewport = {0, 0, 340, 210};
    check(application_settings_synchronize() && scene()->revision != old_revision &&
              ui_rect_width(scene()->viewport) == 340 && ui_rect_height(scene()->viewport) == 210 &&
              ui_page_key_equal(scene()->selected_page, selected) &&
              ui_settings_target_equal(scene()->focus, focus) &&
              g_app.renderer.interaction_text_settings_revision == scene()->revision,
          "settings: compact resize synchronizes retained layout and renderer revision without losing "
          "semantic focus");
    observed.viewport = {0, 0, 1000, 700};
    check(application_settings_synchronize(), "settings: full viewport restores after compact layout");
    cleanup.locked =
        CreateFileW(cleanup.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(cleanup.locked != INVALID_HANDLE_VALUE,
          "settings: private fixture lock injects deterministic save commit failure");
    if (cleanup.locked != INVALID_HANDLE_VALUE) {
        // The failure projects a retry notice, so the scene legitimately changes.
        check(activate_control(kAccessibilityPage, cid("accessibility.reduced_motion")) && await_save() &&
                  g_app.preferences_save_failed &&
                  !g_app.preferences_save_pending && g_app.preferences.reduced_motion,
              "settings: async save failure is surfaced while applied choice remains active");
        CloseHandle(cleanup.locked);
        cleanup.locked = INVALID_HANDLE_VALUE;
        const u64 retry = cid("advanced.preferences_retry");
        check(scene() && scene()->content.controls.count && scene()->content.controls[0].id == retry &&
                  scene()->content.controls[0].kind == UiSettingsControlKind::notice,
              "settings: a failed save shows a retry notice at the top of the current panel");
        check(activate_control(kAccessibilityPage, retry) && g_app.preferences_save_pending && scene() &&
                  ui_page_key_equal(scene()->selected_page, kAccessibilityPage) &&
                  scene()->content.controls[0].id != retry,
              "settings: retry queues the save and withdraws the notice");
        check(await_save() && scene() && scene()->content.controls[0].id != retry &&
                  !g_app.preferences_save_failed &&
                  client_preferences_load_file(cleanup.path, &loaded).ok() &&
                  client_preferences_equal(loaded, g_app.preferences),
              "settings: retried save completes and reaches the fixture file");
    }

    // Inject an owned completed slot, then exercise the actual runtime's unload
    // and reopen behavior. No choose() call, picker, or user account is involved.
    g_app.plugins = static_cast<ApplicationPlugins*>(
        VirtualAlloc(nullptr, sizeof(ApplicationPlugins), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.plugins != nullptr, "plugins: owned zeroed runtime allocated without starting picker");
    if (g_app.plugins) {
        // The ABI-4 runtime the loader needs. Memory-only: no marker file, so this
        // test never touches the real profile; the plugins owner frees it on release.
        auto* sdk = static_cast<ApplicationPluginsSdk*>(
            VirtualAlloc(nullptr, sizeof(ApplicationPluginsSdk), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (sdk) {
            sdk->runtime.ledger = &sdk->ledger;
            sdk->ledger_open = true;
        }
        g_app.plugins->sdk = sdk;
    }
    wchar_t dll_path[kNativePluginModuleMaxPathCharacters + 1]{};
    u32 dll_chars = 0;
    // DLL_PROCESS_DETACH may run at process exit after a failed release. Keep
    // the fixture's caller-owned audit alive for that entire lifetime.
    static FixtureAudit audit{};
    audit.pages = application_settings_pages();
    HMODULE pin = nullptr;
    if (g_app.plugins && fixture_path(dll_path, &dll_chars))
        pin = LoadLibraryExW(dll_path, nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(pin != nullptr, "plugins: actual fixture DLL loads beside settings runtime executable");
    if (pin) {
        using Configure = void(WINAPI*)(u32, u64, void*) noexcept;
        auto configure =
            reinterpret_cast<Configure>(GetProcAddress(pin, "maicord_plugin_fixture_configure"));
        check(configure != nullptr, "plugins: fixture configure export has expected contract");
        if (configure) {
            configure(0, fixture_owner, &audit);
            application_plugins_sdk_bind_host(*g_app.plugins, application_settings_plugin_host());
            const auto loaded_module = native_plugin_module_load(
                &g_app.plugins->modules[0], application_settings_plugin_host(), {dll_path, dll_chars}, MC_PERM_ALL);
            check(loaded_module.ok(),
                  "plugins: fixture loads through actual application settings plugin host");
        }
        FreeLibrary(pin);
        pin = nullptr;
        if (application_plugins_present(0)) {
            application_settings_refresh_pages();
            check(!application_plugins_busy() && !application_plugins_notification() &&
                      !application_plugins_present(8) && !application_plugins_unload(8),
                  "plugins: completed runtime reports bounded slots and no picker notification");
            test_menu_runtime(audit);
            const auto before_plugin = g_app.preferences;
            const u64 request_revision = g_app.preferences_request_revision;
            check(application_settings_open_plugin_page({fixture_owner, 1}) &&
                      activate_control({fixture_owner, 1}, 1) && scene() &&
                      scene()->content.controls.count == 1 && scene()->content.controls[0].checked &&
                      scene()->content_revision == 3 && /* register, first publish, toggle */
                      client_preferences_equal(before_plugin, g_app.preferences) &&
                      g_app.preferences_request_revision == request_revision,
                  "plugins: real Enter routes semantic activation into DLL and refreshes current scene "
                  "without native preference write");
            check(application_preferences_member_visibility(g_app.preferences.member_pane_visible) &&
                      g_app.preferences_save_pending && application_settings_synchronize() &&
                      await_save_retained(),
                  "plugins: native preference completion updates Storage status while retaining current "
                  "plugin scene");
            NativePluginUiToken stale{};
            check(native_plugin_host_settings_token(application_settings_plugin_host(), {fixture_owner, 1},
                                                    &stale)
                      .ok(),
                  "plugins: current fixture UI token captured before unload");
            observed.detaching_module = &g_app.plugins->modules[0];
            observed.audit = &audit;
            check(application_plugins_unload(0) && application_settings_active() && scene() &&
                      scene()->selected_page.owner_id == 1 && !application_plugins_present(0) &&
                      !ui_page_registry_find(*audit.pages, {fixture_owner, 1}) &&
                      observed.detached_live_code && observed.detached_live_state &&
                      observed.detached_active_scene && observed.detached_live_menu_labels &&
                      audit.stops == 1 && audit.stop_after_detach &&
                      audit.stop_without_pages && audit.stop_services_inactive &&
                      audit.stop_menu_services_inactive &&
                      audit.dll_detaches == 1 && audit.dll_after_stop &&
                      GetModuleHandleW(L"MaiCordPluginFixture.dll") == nullptr &&
                      !g_app.plugins->modules[0].state && !application_settings_plugin_host()->state_bytes,
                  "plugins: actual runtime detaches active UI while code/state are live, unloads DLL, then "
                  "reopens native settings");
            const UiSettingsActivation obsolete{{UiSettingsTargetKind::control, {fixture_owner, 1}, 1},
                                                UiSettingsControlKind::toggle,
                                                false,
                                                0};
            check(native_plugin_host_activate_settings(application_settings_plugin_host(), stale, obsolete)
                          .error == NativePluginError::stale,
                  "plugins: unloaded fixture token cannot route into freed code");
            observed.detaching_module = nullptr;
            observed.audit = nullptr;
            test_popup_unload({dll_path, dll_chars});
        }
    }
    // Choosing a DLL without a manifest is a Developer Mode action.
    g_app.preferences.developer_mode = true;
    // A failed DLL start detaches the active settings scene before picker
    // release. Preserve the original modal intent across a nonblocking retry.
    if (g_app.plugins && dll_chars) {
        static FixtureAudit retry_audit{};
        retry_audit.pages = application_settings_pages();
        pin = LoadLibraryExW(dll_path, nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(pin != nullptr, "plugins: failed-start picker fixture pins real DLL for configuration");
        if (pin) {
            using Configure = void(WINAPI*)(u32, u64, void*) noexcept;
            auto configure =
                reinterpret_cast<Configure>(GetProcAddress(pin, "maicord_plugin_fixture_configure"));
            check(configure != nullptr, "plugins: retry fixture exposes configuration export");
            if (configure) {
                configure(3, fixture_owner, &retry_audit);
                picker_boundary.next_path = {dll_path, dll_chars};
                picker_boundary.release_timeouts = 1;
                const u32 path_reads = picker_boundary.path_reads;
                observed.detaching_module = &g_app.plugins->modules[0];
                observed.audit = &retry_audit;
                check(application_settings_active() && application_plugins_choose() &&
                          application_plugins_busy(),
                      "plugins: deterministic completed picker starts through actual choose controller");
                const HANDLE notification = application_plugins_notification();
                application_plugins_poll();
                check(!application_settings_active() && application_plugins_busy() &&
                          g_app.plugins->picker_consumed && g_app.plugins->reopen_settings &&
                          application_plugins_notification() == notification && notification &&
                          WaitForSingleObject(notification, 0) == WAIT_OBJECT_0 &&
                          retry_audit.queries == 1 && retry_audit.starts == 1 && retry_audit.stops == 1 &&
                          picker_boundary.path_reads == path_reads + 1,
                      "plugins: failed DLL start detaches modal and first picker release timeout retains "
                      "original reopen intent");
                application_plugins_poll();
                check(application_settings_active() && !application_plugins_busy() &&
                          !application_plugins_notification() && !g_app.plugins->reopen_settings &&
                          retry_audit.queries == 1 && retry_audit.starts == 1 && retry_audit.stops == 1 &&
                          picker_boundary.path_reads == path_reads + 1,
                      "plugins: second poll releases picker and reopens original modal without querying or "
                      "starting DLL twice");
                observed.detaching_module = nullptr;
                observed.audit = nullptr;
            }
            FreeLibrary(pin);
            pin = nullptr;
            check(retry_audit.dll_detaches == 1 && retry_audit.dll_after_stop,
                  "plugins: failed-start retry fixture frees actual DLL after its single stop");
        }
        // A module's operation guard deterministically rejects shutdown after
        // picker release has succeeded. That partial shutdown must not leave
        // selecting/closing latched or prevent starting another selection.
        picker_boundary.next_path = {dll_path, dll_chars};
        picker_boundary.release_timeouts = 0;
        check(application_plugins_choose(), "plugins: deterministic picker begins before partial shutdown");
        g_app.plugins->picker_consumed = true;
        g_app.plugins->reopen_settings = true;
        g_app.plugins->modules[0].busy = true;
        auto* runtime_memory = g_app.plugins;
        const u32 releases = picker_boundary.releases;
        check(!application_plugins_release() && g_app.plugins == runtime_memory &&
                  !application_plugins_busy() && !g_app.plugins->picker.runtime &&
                  !application_plugins_notification() && !g_app.plugins->picker_consumed &&
                  !g_app.plugins->reopen_settings && picker_boundary.releases == releases + 1,
              "plugins: successful picker shutdown clears selection state even when busy module prevents "
              "runtime release");
        check(application_plugins_choose() && application_plugins_busy(),
              "plugins: a new selection can start after partial shutdown module failure");
        g_app.plugins->modules[0].busy = false;
        // Cancellation after publication cannot change the immutable success
        // batch. A timed-out shutdown must mark it abandoned before polling.
        static FixtureAudit abandoned_audit{};
        abandoned_audit.pages = application_settings_pages();
        pin = LoadLibraryExW(dll_path, nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(pin != nullptr, "plugins: abandoned picker fixture pins DLL for zero-load audit");
        if (pin) {
            using Configure = void(WINAPI*)(u32, u64, void*) noexcept;
            auto configure =
                reinterpret_cast<Configure>(GetProcAddress(pin, "maicord_plugin_fixture_configure"));
            check(configure != nullptr, "plugins: abandoned picker fixture exposes configuration export");
            if (configure) {
                configure(0, fixture_owner, &abandoned_audit);
                application_settings_close(false);
                g_app.plugins->reopen_settings = true;
                picker_boundary.release_timeouts = 1;
                const u32 reads = picker_boundary.path_reads;
                const HANDLE abandoned_notification = application_plugins_notification();
                check(!application_plugins_release() && g_app.plugins->picker_consumed &&
                          !g_app.plugins->reopen_settings && application_plugins_busy() &&
                          native_file_picker_status(&g_app.plugins->picker).ok() &&
                          application_plugins_notification() == abandoned_notification,
                      "plugins: timed-out shutdown abandons already published success before requesting "
                      "picker cancellation");
                application_plugins_poll();
                check(!application_settings_active() && !application_plugins_busy() &&
                          !application_plugins_notification() && !g_app.plugins->picker.runtime &&
                          abandoned_audit.queries == 0 && abandoned_audit.starts == 0 &&
                          abandoned_audit.stops == 0 && picker_boundary.path_reads == reads &&
                          !application_plugins_present(0) &&
                          !application_settings_plugin_host()->active_count,
                      "plugins: poll after abandoned shutdown only joins picker and never loads DLL or "
                      "reopens modal");
            }
            FreeLibrary(pin);
            pin = nullptr;
            check(abandoned_audit.dll_detaches == 1 &&
                      GetModuleHandleW(L"MaiCordPluginFixture.dll") == nullptr,
                  "plugins: abandoned selection leaves no DLL reference after test configuration pin is "
                  "released");
        }
        check(application_settings_open(),
              "settings: native modal can reopen after abandoned plugin selection");
    }
    if (dll_chars && g_app.plugins) {
        static FixtureAudit changed_scope_audit{};
        changed_scope_audit.pages = application_settings_pages();
        HMODULE scope_pin = LoadLibraryExW(dll_path, nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto configure = scope_pin
                                   ? reinterpret_cast<native_plugin_fixture::Configure>(
                                         GetProcAddress(scope_pin, "maicord_plugin_fixture_configure"))
                                   : nullptr;
        check(configure != nullptr, "plugins: manual rollback fixture configures known native DLL");
        if (configure) {
            changed_scope_audit.start_callback_hook = manual_scope_change;
            configure(0, fixture_owner, &changed_scope_audit);
            picker_boundary.next_path = {dll_path, dll_chars};
            picker_boundary.release_timeouts = 0;
            const u64 serial = g_app.plugin_account_scope_serial;
            check(application_plugins_choose(),
                  "plugins: manual scope-change fixture starts deterministic picker");
            application_plugins_poll();
            check(g_app.plugin_account_scope_serial != serial && changed_scope_audit.queries == 1 &&
                      changed_scope_audit.starts == 1 && changed_scope_audit.stops == 1 &&
                      !g_app.plugins->modules[0].enabled && !application_plugins_present(0) &&
                      !application_plugins_busy() && !application_settings_active() &&
                      !utf8_equal(application_plugins_status(), utf8_literal("Plugin enabled.")),
                  "plugins: changed scope rolls back native load and reports cancellation instead of false "
                  "Loaded status");
            changed_scope_audit.start_callback_hook = nullptr;
            check(application_settings_open(),
                  "plugins: native Settings reopens after scope-change rollback");
            static FixtureAudit failed_rollback_audit{};
            failed_rollback_audit.pages = application_settings_pages();
            failed_rollback_audit.start_callback_hook = manual_scope_change;
            failed_rollback_audit.stop_callback_hook = manual_failed_free;
            configure(0, fixture_owner, &failed_rollback_audit);
            check(application_plugins_choose(),
                  "plugins: manual failed-rollback fixture starts deterministic picker");
            application_plugins_poll();
            check(manual_rollback_reference && failed_rollback_audit.stops == 1 &&
                      !g_app.plugins->modules[0].enabled && application_plugins_present(0) &&
                      g_app.plugins->modules[0].state && !application_plugins_busy() &&
                      utf8_equal(application_plugins_status(),
                                 utf8_literal("Couldn't unload the plugin. Try again.")),
                  "plugins: failed rollback preserves cleanup owner and reports unloading error instead of "
                  "Loaded");
            failed_rollback_audit.start_callback_hook = failed_rollback_audit.stop_callback_hook = nullptr;
            if (manual_rollback_reference) {
                g_app.plugins->modules[0].module = manual_rollback_reference;
                manual_rollback_reference = nullptr;
            }
            check(application_plugins_unload(0) && !application_plugins_present(0),
                  "plugins: explicit retry releases restored genuine reference after failed rollback");
            check(application_settings_open(),
                  "plugins: native modal reopens after rollback cleanup retry");
        }
        if (scope_pin)
            FreeLibrary(scope_pin);
    }
    auto* plugin_memory = g_app.plugins;
    check(application_plugins_release() && !g_app.plugins && (!plugin_memory || freed(plugin_memory)) &&
              application_plugins_release(),
          "plugins: runtime release frees owned VM and is idempotent");
    check(application_settings_input(UiInputKind::key_down, VK_ESCAPE, false) &&
              !application_settings_active() &&
              application_settings_input(UiInputKind::key_down, VK_ESCAPE, true) &&
              application_settings_input(UiInputKind::character, VK_ESCAPE, false) &&
              application_settings_input(UiInputKind::key_up, VK_ESCAPE, false) &&
              !application_settings_input(UiInputKind::key_down, 'A', false) && !g_app.settings_held_key,
          "settings: Escape close consumes held key tails before returning control to account input");
    const u64 ending_settings_lifetime = application_settings_lifetime();
    application_settings_release();
    check(!g_app.settings && !scene() && !g_app.settings_held_key && freed(settings_memory),
          "settings: final release clears controller pointer, renderer binding and held keys and frees VM");
    check(ending_settings_lifetime >= settings_lifetime &&
              application_settings_lifetime() == ending_settings_lifetime,
          "settings: releasing an owner retains its process lifetime serial");
    application_settings_release();
    check(!application_settings_window_message(WM_CONTEXTMENU, 0, 0),
          "settings: inactive controller no longer consumes context menus");
    // The production shell dispatcher previously activated the settings gear
    // from hover and omitted click. Exercise real layout and hit testing here.
    check(ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
              ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                           kGuildChannelShellLayoutMetrics, 1280, 700),
          "shell: actual bootstrap scene lays out settings gear, member toggle and account footer");
    const auto* gear =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::settings));
    check(gear && gear->visible, "shell: retained layout exposes visible settings gear");
    if (gear && gear->visible) {
        const LPARAM point = shell_point(UiSemantic::settings);
        const u32 suspends = observed.suspends;
        g_app.frame_pending = false;
        check(handle_shell_hover(point) && !application_settings_active() && !g_app.settings &&
                  observed.suspends == suspends && g_app.frame_pending,
              "shell: actual gear hover requests paint and never opens or allocates settings");
        check(handle_shell_click(point) && application_settings_active(),
              "shell: actual gear click opens account settings controller");
        check(application_settings_lifetime() > ending_settings_lifetime,
              "settings: recreated owner has a fresh lifetime even if its address is reused");
        custom_theme_projection = {utf8_literal("Original theme"), utf8_literal("Imported candidate"),
                                   utf8_literal("Theme ready to preview."), 1};
        application_settings_refresh_pages();
        const auto* palette =
            ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.theme")});
        const auto* import =
            ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.imported.import")});
        check(
            palette && palette->choices.count == 3 && import && import->enabled &&
                utf8_equal(import->description, utf8_literal("Original theme")),
            "custom theme settings: named current theme and native import preserve all eight builtin IDs");
        check(activate_control(kAppearancePage, cid("appearance.imported.import")) &&
                  custom_theme_last_action == ApplicationCustomThemeAction::import_file,
              "custom theme settings: actual keyboard control routes explicit Import to owner");
        custom_theme_projection.candidate_ready = true;
        ++custom_theme_projection.revision;
        application_settings_refresh_pages();
        check(activate_control(kAppearancePage, cid("appearance.imported.preview")) &&
                  custom_theme_last_action == ApplicationCustomThemeAction::preview,
              "custom theme settings: validated candidate exposes explicit Preview action");
        custom_theme_projection.previewing = true;
        ++custom_theme_projection.revision;
        application_settings_refresh_pages();
        palette = ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.theme")});
        check(palette && !palette->enabled &&
                  !ui_settings_scene_control(*scene(), {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.imported.preview")}) &&
                  activate_control(kAppearancePage, cid("appearance.imported.apply")) &&
                  custom_theme_last_action == ApplicationCustomThemeAction::apply,
              "custom theme settings: preview retires old Preview target and routes explicit Apply");
        check(activate_control(kAppearancePage, cid("appearance.imported.cancel")) &&
                  custom_theme_last_action == ApplicationCustomThemeAction::cancel,
              "custom theme settings: actual Cancel control routes complete rollback intent");
        const auto custom_previous_window = g_app.window;
        const auto custom_previous_dpi = g_app.renderer.dpi;
        const auto custom_previous_viewport = observed.viewport;
        HWND custom_window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, -16000, -16000, 1000, 700,
                                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_app.window = custom_window;
        g_app.renderer.dpi = 96;
        observed.viewport = {0, 0, 1000, 700};
        custom_theme_projection.previewing = false;
        ++custom_theme_projection.revision;
        custom_theme_pointer_transition = true;
        application_settings_refresh_pages();
        UiSettingsControlGeometry custom_geometry{};
        const UiSettingsTarget preview_target{UiSettingsTargetKind::control, kAppearancePage, cid("appearance.imported.preview")};
        const bool preview_geometry =
            custom_window && ui_settings_scene_focus(scene(), preview_target) &&
            ui_settings_scene_control_geometry(*scene(), preview_target, &custom_geometry);
        const auto custom_button = ui_rect_intersection(custom_geometry.field, custom_geometry.clip);
        const LPARAM custom_point =
            MAKELPARAM(static_cast<short>((custom_button.left + custom_button.right) * .5f),
                       static_cast<short>((custom_button.top + custom_button.bottom) * .5f));
        check(preview_geometry && ui_rect_has_area(custom_button) &&
                  application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, custom_point) &&
                  application_settings_window_message(WM_LBUTTONUP, 0, custom_point) &&
                  custom_theme_projection.previewing &&
                  custom_theme_last_action == ApplicationCustomThemeAction::preview,
              "custom theme settings: real pointer Preview release publishes Apply at the same button "
              "position");
        const auto actions_before_double_click = custom_theme_actions;
        const auto apply_hit =
            ui_settings_scene_hit_test(*scene(), (custom_button.left + custom_button.right) * .5f,
                                       (custom_button.top + custom_button.bottom) * .5f);
        check(apply_hit.control_id == cid("appearance.imported.apply") &&
                  application_settings_window_message(WM_LBUTTONDBLCLK, MK_LBUTTON, custom_point) &&
                  application_settings_window_message(WM_LBUTTONUP, 0, custom_point) &&
                  custom_theme_actions == actions_before_double_click &&
                  custom_theme_last_action == ApplicationCustomThemeAction::preview,
              "custom theme settings: Preview double-click tail cannot automatically activate replacement "
              "Apply");
        check(
            application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, custom_point) &&
                application_settings_window_message(WM_LBUTTONUP, 0, custom_point) &&
                custom_theme_actions == actions_before_double_click + 1 &&
                custom_theme_last_action == ApplicationCustomThemeAction::apply,
            "custom theme settings: fresh ordinary pointer press release still explicitly applies preview");
        custom_theme_pointer_transition = false;
        if (custom_window)
            DestroyWindow(custom_window);
        g_app.window = custom_previous_window;
        g_app.renderer.dpi = custom_previous_dpi;
        observed.viewport = custom_previous_viewport;
        custom_theme_projection.candidate_ready = custom_theme_projection.previewing = false;
        custom_theme_projection.save_failed = true;
        ++custom_theme_projection.revision;
        application_settings_refresh_pages();
        check(activate_control(kAppearancePage, cid("appearance.imported.retry_save")) &&
                  custom_theme_last_action == ApplicationCustomThemeAction::retry_save,
              "custom theme settings: unsaved applied theme exposes explicit Try again");
        custom_theme_projection = {};
        application_settings_refresh_pages();
        application_settings_release();
    }
    const u64 retained_lifetime = application_settings_lifetime();
    g_app.settings_lifetime_serial = ~u64{0};
    check(!g_app.settings && !application_settings_pages() && application_settings_lifetime() == ~u64{0},
          "settings: lifetime exhaustion refuses a new owner without wrapping");
    g_app.settings_lifetime_serial = retained_lifetime;
    const bool member_before = g_app.preferences.member_pane_visible;
    check(handle_shell_click(shell_point(UiSemantic::member_toggle)) &&
              g_app.preferences.member_pane_visible != member_before &&
              g_app.member_pane_visible == g_app.preferences.member_pane_visible && await_save() &&
              !g_app.preferences_save_failed,
          "shell: actual member-toggle click still applies and saves visibility to fixture");
    g_app.account_ui->chat_state.self_user_id = 123;
    UserRecord footer_users[1]{};
    u32 footer_order[1]{};
    u8 footer_names[64]{};
    check(user_store_initialize(&g_app.account_ui->user_store, {footer_users, 1},
                                        {footer_order, 1}, {footer_names, 64}) &&
              user_store_upsert(&g_app.account_ui->user_store, 123,
                                        utf8_literal("Native Account"), utf8_literal("native.user"))
                  .ok(),
          "shell: account footer owns a real copied self-user identity for precise pointer hits");
    const u32 profile_before = observed.profile_opens;
    const u32 menus_before = observed.account_menu_opens;
    const auto footer =
        ui_widget_manager_find(g_app.shell_scene.widgets, static_cast<u32>(UiSemantic::channel_footer) + 1);
    const auto* footer_node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, footer);
    const auto avatar_point =
        footer_node ? MAKELPARAM(static_cast<short>(footer_node->rectangle.left + 26),
                                 static_cast<short>(
                                     (footer_node->rectangle.top + footer_node->rectangle.bottom) * .5f))
                    : 0;
    check(footer_node && handle_shell_click(avatar_point) &&
              observed.account_menu_opens == menus_before + 1 && observed.profile_opens == profile_before &&
              observed.account_menu_source.slot == footer.slot &&
              observed.account_menu_source.generation == footer.generation && footer_node &&
              ui_rect_equal(observed.account_menu_anchor,
                            ui_rect_intersection(footer_node->rectangle, footer_node->clip_rectangle)),
          "shell: actual account-avatar click opens account menu at its clipped source anchor");
    const auto blank_point = footer_node ? MAKELPARAM(static_cast<short>(footer_node->rectangle.right - 48),
                                                      static_cast<short>(footer_node->rectangle.top + 2))
                                         : 0;
    check(footer_node && !handle_shell_click(blank_point) &&
              observed.account_menu_opens == menus_before + 1 && observed.profile_opens == profile_before,
          "shell: blank account-footer space cannot open an identity menu");
    auto pin_metrics = kGuildChannelShellLayoutMetrics;
    pin_metrics.flags |= ui_shell_layout_pins;
    ++pin_metrics.revision;
    check(ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, pin_metrics, 1280,
                                       700),
          "shell cursor: current channel Pins launcher initializes under explicit layout flag");
    const auto pin_target = ui_scene_header_geometry(g_app.shell_scene).pins;
    g_app.frame_pending = false;
    check(handle_shell_hover(MAKELPARAM(static_cast<short>((pin_target.left + pin_target.right) * .5f),
                                        static_cast<short>((pin_target.top + pin_target.bottom) * .5f))) &&
              g_app.frame_pending && !application_settings_active(),
          "shell hover: native Pins launcher requests its own paint without opening account Settings");
    check(set_client_cursor(static_cast<i32>((pin_target.left + pin_target.right) * .5f),
                            static_cast<i32>((pin_target.top + pin_target.bottom) * .5f)) &&
              GetCursor() == LoadCursorW(nullptr, IDC_HAND),
          "shell cursor: native Pins launcher presents a hand over its retained target");
    ui_scene_release(&g_app.shell_scene);
    g_app.context_focus = nullptr;
}
} // namespace settings_runtime_test

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
bool application_custom_theme_restore(const ClientThemeStorageSnapshot&, bool) noexcept {
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
}
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
    ++observed.requests_closed;
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
// Popover glue (application_plugins_surfaces.cpp is linked for real).
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
        ++observed.popup_dismissals;
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
bool application_message_forward_available(u64) noexcept {
    return false;
}
bool application_profile_open(u64 user, UiRect, ProfileAnchor anchor) noexcept {
    auto& seen = settings_runtime_test::observed;
    ++seen.profile_opens;
    seen.profile_user = user;
    seen.profile_anchor = anchor;
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

#include "application_hover_projection_runtime.hpp"
#include "application_window_resize_runtime.hpp"

namespace maicord::settings_runtime_test {
// Pure catalog and local-store contract; no controller or renderer state.
void test_settings_catalog() noexcept {
    const auto& schema = settings_catalog();
    check(ui_settings_schema_valid(schema) && schema.panels.count == kSettingsPanelCount,
          "settings catalog: schema keys are unique and every panel belongs to a section");
    bool ids = true, bindings = true;
    for (u32 p = 0; p < schema.panels.count; ++p)
        for (u32 n = 0; n < schema.panels[p].nodes.count; ++n) {
            UiSettingsSchemaLocation location{};
            ids = ids && settings_control_location(settings_control_id(p, n), &location) && location.panel == p &&
                  location.node == n;
            const auto binding = static_cast<SettingBinding>(schema.panels[p].nodes[n].binding);
            UiSettingsSchemaLocation found{};
            bindings = bindings && (binding == SettingBinding::none ||
                                    (settings_catalog_binding(binding, &found) && found.panel == p && found.node == n));
        }
    check(ids && bindings && !settings_control_location(0, nullptr) && cid("appearance.theme") &&
              !cid("appearance") && !cid("missing.key"),
          "settings catalog: control ids and bindings resolve uniquely to their node");
    ClientPreferences preferences = client_preferences_defaults();
    SettingValue value{};
    check(settings_store_read(preferences, SettingBinding::theme_base, &value) && value.choice == 1 &&
              settings_store_read(preferences, SettingBinding::theme_color, &value) && value.choice == kUiSettingsNoChoice &&
              settings_store_write(&preferences, SettingBinding::theme_color, {false, 1}) &&
              preferences.theme == ClientTheme::sunset &&
              settings_store_read(preferences, SettingBinding::theme_base, &value) && value.choice == kUiSettingsNoChoice &&
              !settings_store_write(&preferences, SettingBinding::theme_base, {false, 3}),
          "settings store: theme palettes map onto ClientTheme and reject out-of-range choices");
    preferences = client_preferences_defaults();
    check(settings_store_write(&preferences, SettingBinding::developer_mode, {true, 0}) && preferences.developer_mode &&
              settings_store_write(&preferences, SettingBinding::gradient_intensity, {false, 55}) &&
              preferences.gradient.intensity == 55 && preferences.theme == ClientTheme::custom_gradient &&
              settings_store_write(&preferences, SettingBinding::output_volume, {false, 200}) &&
              !settings_store_write(&preferences, SettingBinding::output_volume, {false, 201}) &&
              !settings_store_write(&preferences, SettingBinding::customize_theme, {true, 0}) &&
              settings_binding_backend(SettingBinding::developer_mode) == SettingBackend::local &&
              settings_binding_backend(SettingBinding::custom_import) == SettingBackend::action,
          "settings store: local bindings write preferences; actions have no stored value");
    // Every theme swatch previews its own theme: flat themes as their own
    // surface (one repeated stop), colour themes as their own stops and
    // Custom as the user's.
    preferences = client_preferences_defaults();
    preferences.gradient.color_count = 2;
    preferences.gradient.colors[0] = 0x123456;
    preferences.gradient.colors[1] = 0x654321;
    preferences.gradient.colors[2] = preferences.gradient.colors[3] = preferences.gradient.colors[4] = 0;
    BackgroundTheme swatches[8]{};
    bool resolved = true;
    for (u32 i = 0; i < 8; ++i)
        resolved = resolved && settings_theme_swatch(preferences, i < 3 ? SettingBinding::theme_base
                                                                      : SettingBinding::theme_color,
                                                     i < 3 ? i : i - 3, &swatches[i]);
    const auto luma = [](u32 rgb) noexcept { return ((rgb >> 16) & 255) * 3 + ((rgb >> 8) & 255) * 6 + (rgb & 255); };
    bool flat = resolved, distinct = resolved;
    for (u32 i = 0; i < 3; ++i)
        flat = flat && swatches[i].color_count == 2 && swatches[i].colors[0] == swatches[i].colors[1];
    for (u32 i = 3; i < 8; ++i)
        flat = flat && swatches[i].colors[0] != swatches[i].colors[1];
    // Custom light shows the same stops as Custom; every other pair differs.
    for (u32 i = 0; i < 7; ++i)
        for (u32 j = i + 1; j < 7; ++j)
            distinct = distinct && !bytes_equal(swatches[i].colors, swatches[j].colors, sizeof(swatches[i].colors));
    check(flat && distinct && luma(swatches[0].colors[0]) > luma(swatches[1].colors[0]) &&
              luma(swatches[1].colors[0]) > luma(swatches[2].colors[0]) && swatches[6].colors[0] == 0x123456 &&
              swatches[6].colors[1] == 0x654321 && swatches[7].colors[0] == 0x123456 &&
              !settings_theme_swatch(preferences, SettingBinding::theme_base, 3, &swatches[0]) &&
              !settings_theme_swatch(preferences, SettingBinding::member_list, 0, &swatches[0]),
          "settings catalog: theme swatches resolve to their own theme, Light > Dark > AMOLED, Custom to the user's stops");
    const auto navigation = settings_catalog_navigation(true);
    bool pages = navigation.count == settings_catalog_navigation(false).count + 1 &&
                 navigation[navigation.count - 1].kind == UiSettingsNavKind::action &&
                 navigation[navigation.count - 1].destructive;
    u32 panel_pages = 0;
    for (u32 i = 0; i < navigation.count; ++i)
        if (navigation[i].kind == UiSettingsNavKind::page) {
            ++panel_pages;
            pages = pages && navigation[i].page.owner_id == kSettingsNativeOwner &&
                    navigation[i].page.page_id >= 1 && navigation[i].page.page_id <= kSettingsPanelCount;
        }
    UiSettingsSearchHit hits[8]{};
    const u32 developer = ui_settings_schema_search(schema, utf8_literal("copy id"), {hits, 8});
    check(pages && panel_pages == kSettingsPanelCount && developer >= 1 &&
              settings_control_id(hits[0].panel, hits[0].node) == cid("advanced.developer_mode"),
          "settings catalog: sidebar lists every panel once; search finds Developer mode by its purpose");
}
// Plugin slot popovers: row projection, opening at the anchor, link rows
// through the link trust path, inert rows, and dismissal when the runtime's
// rows change (owner unload) or the account scope ends.
void test_plugin_popover() noexcept {
    static PopupItem items[16]{};
    static PopupRowWidget rows[16]{};
    static UiWidget widgets[34]{};
    static UiLayoutNode nodes[34]{};
    const bool owned_plugins = !g_app.plugins, owned_ui = !g_app.account_ui;
    if (owned_plugins)
        g_app.plugins = static_cast<ApplicationPlugins*>(
            VirtualAlloc(nullptr, sizeof(ApplicationPlugins), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (owned_ui)
        g_app.account_ui = static_cast<AccountUiRuntime*>(
            VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const bool owned_sdk = g_app.plugins && !g_app.plugins->sdk;
    if (owned_sdk)
        g_app.plugins->sdk = static_cast<ApplicationPluginsSdk*>(
            VirtualAlloc(nullptr, sizeof(ApplicationPluginsSdk), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const bool ready = g_app.plugins && g_app.plugins->sdk && g_app.account_ui && !g_app.popup_scene.active &&
                       popup_scene_initialize(&g_app.popup_scene, {items, 16}, {rows, 16}, {widgets, 34}, {nodes, 34});
    check(ready, "plugin popover: fixture initializes");
    if (ready) {
        auto& rt = g_app.plugins->sdk->runtime;
        const auto node = [&](u32 index, u32 kind, const char* text, u64 action, u32 tone = 0,
                              const char* value = "") noexcept {
            auto& out = rt.popover[index];
            out = {};
            out.kind = static_cast<u8>(kind);
            out.tone = static_cast<u8>(tone);
            out.icon = kPluginSdkNoIcon;
            out.action_id = action;
            while (text[out.text_bytes]) out.text[out.text_bytes] = text[out.text_bytes], ++out.text_bytes;
            while (value[out.value_bytes]) out.value[out.value_bytes] = value[out.value_bytes], ++out.value_bytes;
        };
        node(0, MC_NODE_LABEL, "Status: idle", 0);
        node(1, MC_NODE_SEPARATOR, "", 0);
        node(2, MC_NODE_BUTTON, "Refresh", 7);
        node(3, MC_NODE_LINK, "Docs", 0, 0, "https://example.com/docs");
        node(4, MC_NODE_BUTTON, "Remove", 9, MC_TONE_CRITICAL);
        const auto arm = [&]() noexcept {
            rt.popover_count = 5;
            rt.popover_owner = 0x77;
            ++rt.popover_revision;
        };
        arm();
        PopupItem projected[8]{};
        const u32 count = application_plugins_popover_items(rt.popover, rt.popover_count, {projected, 8});
        check(count == 4 && !(projected[0].flags & popup_item_enabled) &&
                  projected[1].flags == (popup_item_enabled | popup_item_separator_before) &&
                  projected[1].action == kApplicationPluginPopoverActionFirst + 2 &&
                  (projected[2].flags & popup_item_enabled) &&
                  projected[2].presentation.icon_plus_one == static_cast<u8>(UiIcon::link) + 1 &&
                  projected[3].flags == (popup_item_enabled | popup_item_danger),
              "plugin popover: separators fold, labels inert, links and buttons enabled, critical tone is danger");
        const UiRect anchor{100, 100, 132, 132};
        const bool opened = application_plugins_popover_open(anchor) && g_app.popup_scene.active &&
                            g_app.popup_owner_kind == PopupOwnerKind::plugin_popover &&
                            g_app.popup_scene.item_count == 4 &&
                            g_app.popup_scene.items[0].label.data != rt.popover[0].text &&
                            utf8_equal(g_app.popup_scene.items[2].label, utf8_literal("Docs"));
        check(opened && application_plugins_popover_action(kApplicationPluginPopoverActionFirst + 3) &&
                  !application_plugins_popover_action(kApplicationPluginPopupActionFirst),
              "plugin popover: opens as its own popup owner with copied labels and a distinct action range");
        const u32 urls = observed.opened_urls;
        application_plugins_popover_activate(kApplicationPluginPopoverActionFirst + 3);
        check(!g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::none &&
                  observed.opened_urls == urls + 1,
              "plugin popover: a link row dismisses the popup and opens through the link trust path");
        application_plugins_popover_open(anchor);
        application_plugins_popover_activate(kApplicationPluginPopoverActionFirst + 0);
        check(!g_app.popup_scene.active && observed.opened_urls == urls + 1,
              "plugin popover: a label row is inert");
        application_plugins_popover_open(anchor);
        // No binding owns 0x77: the button's handler can't run and nothing reopens.
        application_plugins_popover_activate(kApplicationPluginPopoverActionFirst + 2);
        check(!g_app.popup_scene.active, "plugin popover: a button whose owner is gone only dismisses");
        application_plugins_popover_open(anchor);
        rt.popover_count = 0; // the owner unloaded
        ++rt.popover_revision;
        application_plugins_popover_poll();
        check(!g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::none,
              "plugin popover: closes when the runtime clears its owner's rows");
        arm();
        application_plugins_popover_open(anchor);
        ++g_app.plugin_account_scope_serial;
        application_plugins_popover_poll();
        check(!g_app.popup_scene.active && rt.popover_count == 0,
              "plugin popover: account scope invalidation closes it and drops the rows");
        check(!application_plugins_popover_open(anchor), "plugin popover: nothing to show without rows");
        popup_scene_release(&g_app.popup_scene);
        g_app.popup_scene = {};
    }
    if (owned_sdk && g_app.plugins->sdk) {
        VirtualFree(g_app.plugins->sdk, 0, MEM_RELEASE);
        g_app.plugins->sdk = nullptr;
    }
    if (owned_plugins && g_app.plugins) {
        VirtualFree(g_app.plugins, 0, MEM_RELEASE);
        g_app.plugins = nullptr;
    }
    if (owned_ui && g_app.account_ui) {
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
        g_app.account_ui = nullptr;
    }
}
} // namespace maicord::settings_runtime_test

extern "C" __declspec(noreturn) void WINAPI maicord_settings_runtime_entry() noexcept {
    using namespace maicord::settings_runtime_test;
    test_settings_catalog();
    test_controller();
    maicord::test_application_plugin_navigation_runtime(check);
    maicord::test_application_settings_choice_runtime(check);
    maicord::test_application_plugin_catalog_runtime(check);
    maicord::test_application_plugins_read_runtime(check);
    maicord::test_application_custom_theme(check);
    maicord::app_detail::test_application_user_settings_runtime(check);
    maicord::app_detail::test_application_plugins_actions_runtime(check);
    test_retained_hover_projection();
    test_window_resize_lifecycle();
    test_plugin_popover();
    constexpr char pass[] = "PASS application settings runtime\n";
    constexpr char fail[] = "FAIL application settings runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}

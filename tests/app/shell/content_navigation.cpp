// Exercise the publicly exposed shipping navigator, including its real Core
// selection and channel authority. Surrounding presentation/history are thin
// deterministic boundaries; no cloned navigation helper is used.
#include "maicord/platform/window/win32.hpp"
#include "../../../src/app/shell/content_input.cpp"
#include "../../../src/app/shell/tabs.cpp"
#include "../../support/window_fixture.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "../../../src/app/shell/content_point.hpp"
#include "../../support/test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "../../support/application_content_input_inactive.hpp"
#include "media_page_fake.hpp"

namespace maicord::content_navigation_test {
using namespace app_detail;
u32 failures, history_calls, selections, stops, friends_shown;
bool friends_visible;
bool text_ok = true, history_ok = true, resident_jump;
bool wrong_active, wrong_selected;
MessageTextHit native_hit{};
// The inline Save button as the downloads owner reports it; tests move it to
// model content scrolling under a still pointer.
UiRect save_button{};
u32 save_presses;
MessageRecord source_records[2]{};
u32 source_order[2]{};
u8 source_text[512]{};
u32 typed_links{}, captured_sources{};
ApplicationMessageNavigationSource captured_source{};
MessageLink captured_target{};
AccountUiRuntime* ui;
GuildRecord guilds[2];
u16 guild_order[2];
constexpr u32 kChannelSlots = 32;
ChannelRecord channels[kChannelSlots];
u16 channel_order[kChannelSlots];
ChannelMemory tab_memory;
u64 tab_profile;
RoleRecord roles[4];
PermissionOverwriteRecord overwrites[4];
u8 names[2048];
constexpr u64 rights = kPermissionViewChannel | kPermissionReadMessageHistory |
                       kPermissionSendMessages | kPermissionSendMessagesInThreads;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool reset() noexcept {
    SecureZeroMemory(&g_app, sizeof(g_app));
    application_tabs_reset();
    media_page_fake::reset();
    tab_memory = {};
    tab_profile = 9;
    history_calls = selections = stops = friends_shown = 0;
    friends_visible = false;
    home_shows_conversation = false;
    native_hit = {};
    typed_links = captured_sources = 0;
    captured_source = {};
    captured_target = {};
    text_ok = history_ok = true;
    resident_jump = wrong_active = wrong_selected = false;
    g_app.account_ui = ui;
    g_app.account.instance = ui;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 9;
    g_app.plugin_account_scope_serial = 7;
    SecureZeroMemory(guilds, sizeof(guilds));
    SecureZeroMemory(channels, sizeof(channels));
    SecureZeroMemory(roles, sizeof(roles));
    SecureZeroMemory(overwrites, sizeof(overwrites));
    auto& state = ui->chat_state;
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, kChannelSlots},
                                        {channel_order, kChannelSlots}, {names, sizeof(names)}) ||
        !chat_state_store_bind_permissions(&state, {roles, 4}, {overwrites, 4}) ||
        !chat_state_store_upsert_guild(&state,
                                          {10, utf8_literal("Guild"), GuildMutationKind::create, 99})
             .ok() ||
        !chat_state_store_upsert_role(&state, {10, 10, rights, {}}) ||
        !chat_state_store_set_self_roles(&state, 10, 99, 5, {}))
        return false;
    const ChannelInput inputs[]{{20, 10, 0, utf8_literal("Parent"), 0, 0, 0},
                                       {21, 10, 0, utf8_literal("Other"), 0, 0, 0},
                                       {30, 10, 20, utf8_literal("Public thread"), 0, 0, 11},
                                       {31, 10, 20, utf8_literal("Private thread"), 0, 0, 12},
                                       {40, 10, 0, utf8_literal("Forum"), 0, 0, 15}};
    for (const auto& input : inputs)
        if (!chat_state_store_upsert_channel(&state, input).ok())
            return false;
    // Sixteen more text channels for the tab limit.
    for (u64 id = 100; id < 116; ++id)
        if (!chat_state_store_upsert_channel(&state, {id, 10, 0, utf8_literal("General"), 0, 0, 0}).ok())
            return false;
    // Core publishes role/overwrite resolution separately from channel insertion,
    // including inherited thread permissions. Resolve the real fixture before
    // asking Core to select its initial readable channel.
    chat_state_store_recompute_guild_permissions(&state, 10);
    if (!chat_state_store_select_channel(&state, 21).ok())
        return false;
    ui->active_channel_id = 21;
    return message_store_initialize(&g_app.message_view.store, {source_records, 2}, {source_order, 2},
                                    {source_text, sizeof(source_text)});
}
// Regression: cursor resolution once called the Save button's press handler,
// which took SetCapture. Windows stops sending WM_SETCURSOR to a capturing
// window, so the hand stuck everywhere until the next click.
void cursor_tests() noexcept {
    check(reset(), "content cursor: fixture resets");
    g_app.window = win32_window(CreateWindowExW(0, L"STATIC", L"cursor fixture", WS_POPUP, 0, 0, 800, 600, nullptr,
                                   nullptr, nullptr, nullptr));
    g_app.message_scene.active = true;
    g_app.message_scene.viewport = {0, 0, 800, 600};
    save_button = {100, 100, 130, 130};
    save_presses = 0;
    UiCursor cursor = UiCursor::arrow;
    check(application_message_content_cursor(115, 115, &cursor) && cursor == UiCursor::hand,
          "content cursor: inline Save button presents the hand");
    check(!save_presses && GetCapture() == nullptr && !g_app.message_scene.widgets.captured.generation &&
              !g_app.message_scene.widgets.focused.generation,
          "content cursor: hovering Save never presses, captures the pointer or moves focus");
    cursor = UiCursor::arrow;
    check(!application_message_content_cursor(300, 300, &cursor) && cursor == UiCursor::arrow,
          "content cursor: leaving the Save button releases the hand to the shell's arrow or I-beam");
    save_button = {100, 160, 130, 190};
    check(!application_message_content_cursor(115, 115, &cursor) && cursor == UiCursor::arrow,
          "content cursor: content scrolled away under a still pointer releases the hand");
    check(application_message_content_cursor(115, 170, &cursor) && cursor == UiCursor::hand &&
              !save_presses,
          "content cursor: the moved Save button owns the hand at its new position only");
    g_app.message_scene.active = false;
    check(!application_message_content_cursor(115, 170, &cursor),
          "content cursor: a hidden message scene claims no cursor");
    if (GetCapture() == win32_hwnd(g_app.window))
        ReleaseCapture();
    DestroyWindow(win32_hwnd(g_app.window));
    g_app.window = nullptr;
    save_button = {};
}
// Home, opened from the rail with focus on its button, reopens the view it
// showed when a server was opened from it.
bool select_home() noexcept {
    g_app.context_focus = &g_app.shell_scene.widgets;
    return activate_focused_navigation();
}
// The system turns WM_RBUTTONUP's client point into WM_CONTEXTMENU's screen point; the window backend hands the
// default procedure the client point as sent (the content shift happens on the event the application sees), so the
// screen point maps back to the content point under the caption.
LPARAM context_screen_point;
LRESULT CALLBACK context_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_CONTEXTMENU) {
        context_screen_point = lparam;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void context_point_tests() noexcept {
    check(reset(), "context point: fixture resets");
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = context_window;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MaiCordContextPointTest";
    RegisterClassExW(&type);
    g_app.window = win32_window(CreateWindowExW(0, type.lpszClassName, L"", WS_POPUP, 40, 60, 800, 600, nullptr, nullptr,
                                   type.hInstance, nullptr));
    g_app.renderer.caption_height = 31;
    const LPARAM client = MAKELPARAM(120, 200); // Pixels in the window's client area, caption rows included.
    context_screen_point = 0;
    if (g_app.window)
        SendMessageW(win32_hwnd(g_app.window), WM_RBUTTONUP, 0, client);
    POINT point{static_cast<short>(LOWORD(context_screen_point)), static_cast<short>(HIWORD(context_screen_point))};
    const bool converted = context_screen_point && ScreenToClient(win32_hwnd(g_app.window), &point);
    application_client_to_content(&point);
    check(converted && point.x == 120 && point.y == 200 - 31,
          "context point: the client point given to DefWindowProc maps back to the content point");
    g_app.renderer.caption_height = 0;
    if (g_app.window)
        DestroyWindow(win32_hwnd(g_app.window));
    g_app.window = nullptr;
    UnregisterClassW(type.lpszClassName, type.hInstance);
}
void home_tests() noexcept {
    check(reset() && chat_state_store_upsert_channel(&ui->chat_state, {50, 0, 0, utf8_literal("DM"), 0, 0, 3}).ok(),
          "home navigation: conversation fixture");
    check(select_home() && friends_shown == 1 && application_friends_active(),
          "home navigation: Home without an earlier conversation shows Friends");
    check(chat_state_store_select_channel(&ui->chat_state, 50).ok() && (application_friends_hide(), true) &&
              select_content_guild(10) && select_home() && friends_shown == 1 && !friends_visible &&
              ui->chat_state.home_selected && ui->chat_state.selected_channel_id == 50,
          "home navigation: leaving a conversation for a server returns Home to that conversation");
    check(application_friends_show() && select_content_guild(10) && !friends_visible && select_home() &&
              friends_shown == 3 && application_friends_active(),
          "home navigation: leaving Friends for a server returns Home to Friends");
}
// The tab model over the real store and navigator: what comes back, what a click, a pin and the keys do.
u64 tab_clock = 200'000;
const UiTabStripView* tab_view(UiTabHit hovered = {}, u64 advance = 1) noexcept {
    tab_clock += advance;
    return application_tabs_view(hovered, {}, tab_clock);
}
u64 memory_tab(u32 i) noexcept {
    return tab_memory.tabs[i].channel_id;
}
ChannelMemoryTab saved_tab(u64 channel, u64 guild, const char* label) noexcept {
    ChannelMemoryTab tab{channel, guild, 0, {}};
    while (label[tab.label_bytes])
        tab.label[tab.label_bytes] = label[tab.label_bytes], ++tab.label_bytes;
    return tab;
}
bool tab_channels(const u64* expected, u32 count) noexcept {
    u32 live = 0;
    for (u32 i = 0; i < application_tabs_count(); ++i)
        if (const u64 id = application_tabs_key(i)) {
            if (live >= count || id != expected[live])
                return false;
            ++live;
        }
    if (live != count || tab_memory.tab_count != count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (memory_tab(i) != expected[i])
            return false;
    return true;
}
void tabs_tests() noexcept {
    check(reset(), "tabs: fixture resets");
    ui->chat_state.self_user_id = 99;
    g_app.preferences.tabs_enabled = true;
    const ChannelMemoryTab saved[]{saved_tab(20, 10, "Parent"), saved_tab(21, 10, "Other"), saved_tab(777, 10, "Old")};
    channel_memory_set_tabs(&tab_memory, {saved, 3});
    check(!application_tabs_open_channel(30, true) && application_tabs_visible(),
          "tabs: nothing changes before the saved tabs are adopted");
    const UiTabStripView* view = tab_view();
    const u64 adopted[]{20, 21};
    check(view && view->count == 2 && utf8_equal(view->items[0].label, utf8_literal("Parent")) &&
              utf8_equal(view->items[1].label, utf8_literal("Other")) && !(view->items[0].flags & kUiTabActive) &&
              (view->items[1].flags & kUiTabActive) && tab_channels(adopted, 2),
          "tabs: the saved tabs come back with their labels, the active one marked, the vanished channel dropped");
    // Hovering a tab ramps its hover amount on the shared transition, and the amount keeps falling after the pointer
    // leaves, so the close button fades instead of popping. "+" hovers no tab.
    const UiTabHit over{UiTabHitKind::tab, 0};
    tab_view(over);
    const float rising = tab_view(over, 85)->items[0].hover;
    const float full = tab_view(over, 315)->items[0].hover;
    tab_view({}, 1); // The pointer leaves: the fade-out starts from the settled amount.
    const float fading = tab_view({}, 85)->items[0].hover;
    const float gone = tab_view({}, 515)->items[0].hover;
    check(rising > 0 && rising < 1 && full == 1 && fading > 0 && fading < 1 && gone == 0 &&
              tab_view({UiTabHitKind::plus, 0}, 100)->items[0].hover == 0,
          "tabs: the hover amount ramps in and out over the shared transition, and \"+\" hovers no tab");
    const u64 three[]{20, 21, 30};
    check(application_tabs_open_channel(30, true) && application_tabs_open_channel(30, true) &&
              ui->chat_state.selected_channel_id == 21 && tab_view()->count == 3 && tab_channels(three, 3) &&
              !(tab_view()->items[2].flags & (kUiTabOpening | kUiTabClosing)),
          "tabs: a background tab opens once, persists and leaves the selection alone");
    check(!application_tabs_open_channel(40, true) && !application_tabs_open_channel(31, true) &&
              !application_tabs_open_channel(999, true) && tab_channels(three, 3),
          "tabs: a forum, an unreachable thread and an unknown channel cannot become tabs");
    check(application_tabs_open_channel(30, false) && ui->chat_state.selected_channel_id == 30 &&
              (tab_view()->items[2].flags & kUiTabActive) && !(tab_view()->items[1].flags & kUiTabActive) &&
              !application_tabs_pin_current() && !tab_view()->plus_enabled,
          "tabs: showing a tab selects its channel, and the pin is off while the channel is already a tab");
    check(application_navigate_known_message(100, 0) && tab_view()->plus_enabled &&
              !(tab_view()->items[2].flags & kUiTabActive) && application_tabs_pin_current() &&
              tab_view()->count == 4 && !tab_view()->plus_enabled && (tab_view()->items[3].flags & kUiTabActive),
          "tabs: \"+\" pins the channel that is selected");
    const u64 after_close[]{20, 21, 30};
    check(application_tabs_close(3) && ui->chat_state.selected_channel_id == 30 && tab_view()->count == 3 &&
              tab_channels(after_close, 3),
          "tabs: closing the active tab shows its left neighbour when it is the last");
    const u64 after_middle[]{20, 30};
    check(application_navigate_known_message(21, 0) && application_tabs_close(1) &&
              ui->chat_state.selected_channel_id == 30 && tab_view()->count == 2 && tab_channels(after_middle, 2),
          "tabs: closing the active tab shows its right neighbour");
    check(!application_tabs_close(5) && application_tabs_activate(0) && ui->chat_state.selected_channel_id == 20 &&
              application_tabs_menu_action(popup_action_tab_close_others, application_tabs_key(0)) &&
              tab_view()->count == 1 && tab_memory.tab_count == 1,
          "tabs: close others keeps one tab and shows it when the active one went");
    // Sixteen tabs fit; the seventeenth is refused with a toast.
    bool filled = true;
    for (u64 id = 100; id < 115; ++id)
        filled = filled && application_tabs_open_channel(id, true);
    check(filled && tab_view()->count == kUiTabCapacity && tab_memory.tab_count == kUiTabCapacity &&
              !g_app.toasts.count && !application_tabs_open_channel(115, true) && g_app.toasts.count == 1 &&
              tab_view()->count == kUiTabCapacity,
          "tabs: the seventeenth tab is refused with a toast");
    check(application_tabs_move(0, 5) && application_tabs_key(5) == 20 && memory_tab(5) == 20 &&
              memory_tab(0) == 100 && !application_tabs_move(5, 5) && !application_tabs_move(5, 16),
          "tabs: moving a tab reorders and persists");
    const u32 dragged = application_tabs_drag_begin(5);
    application_tabs_drag_to(40);
    check(dragged == 5 && tab_view()->drag_left == 40 && (tab_view()->items[5].flags & kUiTabDragging) &&
              application_tabs_move(5, 2) && application_tabs_drag_index() == 2 && (tab_view()->items[2].flags & kUiTabDragging) &&
              !(tab_view()->items[5].flags & kUiTabDragging),
          "tabs: a dragged tab follows the pointer through its swaps");
    application_tabs_drag_end();
    check(!(tab_view()->items[2].flags & kUiTabDragging) && memory_tab(2) == 20 &&
              application_tabs_menu_action(popup_action_tab_close_right, application_tabs_key(10)) &&
              tab_view()->count == 11 && tab_memory.tab_count == 11,
          "tabs: a dropped tab settles, and close to the right ends the strip");
    // A deleted channel closes its tab.
    const u64 doomed = memory_tab(4);
    check(chat_state_store_remove_channel(&ui->chat_state, doomed) && tab_view()->count == 10 &&
              !application_tabs_contains(doomed) && tab_memory.tab_count == 10,
          "tabs: a channel that is gone closes its tab");
    // The keys act on the tabs, and fall through while the strip is hidden.
    using A = ShortcutAction;
    check(application_navigate_known_message(memory_tab(0), 0) && application_tabs_shortcut(A::tab_next, false) &&
              ui->chat_state.selected_channel_id == memory_tab(1) && application_tabs_shortcut(A::tab_previous, false) &&
              application_tabs_shortcut(A::tab_previous, false) &&
              ui->chat_state.selected_channel_id == memory_tab(9) && application_tabs_shortcut(A::tab_third, false) &&
              ui->chat_state.selected_channel_id == memory_tab(2) && application_tabs_shortcut(A::tab_last, false) &&
              ui->chat_state.selected_channel_id == memory_tab(9) && application_tabs_shortcut(A::tab_eighth, true) &&
              ui->chat_state.selected_channel_id == memory_tab(9),
          "tabs: Ctrl+Tab cycles, Ctrl+1..9 pick a tab and held keys act once");
    const u64 closing_channel = ui->chat_state.selected_channel_id;
    check(application_tabs_shortcut(A::tab_close, false) && tab_view()->count == 9 &&
              !application_tabs_contains(closing_channel) && application_navigate_known_message(115, 0) &&
              application_tabs_shortcut(A::tab_new, false) && tab_view()->count == 10 && application_tabs_contains(115),
          "tabs: Ctrl+W closes the active tab and Ctrl+T pins the current channel");
    // Navigation, even to the channel that is selected, hides a page and keeps its tab.
    check(application_tabs_open_media(media_page_fake::source(MediaPageKind::audio, "song"), false) &&
              media_page_fake::shown != 0 && tab_view()->count == 11 &&
              application_navigate_known_message(ui->chat_state.selected_channel_id, 0) && !media_page_fake::shown &&
              media_page_fake::open_pages() == 1 && tab_memory.tab_count == 10 && application_tabs_close(10) &&
              tab_view()->count == 10 && !media_page_fake::open_pages(),
          "tabs: navigation hides a media page and keeps its tab, which closes with its page");
    const u32 kept = application_tabs_count();
    g_app.preferences.tabs_enabled = false;
    check(!application_tabs_shortcut(A::tab_next, false) && !tab_view() && !application_tabs_visible() &&
              !application_tabs_open_channel(20, true) && !application_tabs_close(0) && tab_memory.tab_count == kept,
          "tabs: with the setting off the strip is hidden, the keys fall through and the saved tabs stay");
    g_app.preferences.tabs_enabled = true;
    check(tab_view() && tab_view()->count == kept, "tabs: turning the setting back on shows the tabs again");
    tab_profile = 10;
    tab_memory = {};
    check(tab_view() && tab_view()->count == 0, "tabs: another account starts without the previous account's tabs");
    g_app.preferences.tabs_enabled = false;
    application_tabs_reset();
}
// READY lists a guild as unavailable before its channels arrive: a tab is dropped only when its channel is certainly
// gone.
void tab_pruning_tests() noexcept {
    check(reset(), "tab pruning: fixture resets");
    auto& state = ui->chat_state;
    state.self_user_id = 99;
    g_app.preferences.tabs_enabled = true;
    check(chat_state_store_upsert_channel(&state, {50, 0, 0, utf8_literal("Chat"), 0, 0, 3}).ok() &&
              chat_state_store_upsert_guild(&state, {11, utf8_literal("Guild 2"), GuildMutationKind::unavailable, 99}).ok(),
          "tab pruning: a conversation and an unavailable guild exist");
    const ChannelMemoryTab saved[]{saved_tab(20, 10, "Parent"), saved_tab(300, 11, "Lost"), saved_tab(301, 10, "Gone"),
                                   saved_tab(50, 0, "Chat"), saved_tab(400, 12, "Left")};
    channel_memory_set_tabs(&tab_memory, {saved, 5});
    const UiTabStripView* view = tab_view();
    check(view && view->count == 3 && utf8_equal(view->items[1].label, utf8_literal("Lost")) &&
              (view->items[1].flags & kUiTabMissing) && !(view->items[0].flags & kUiTabMissing) &&
              !(view->items[2].flags & kUiTabMissing) && application_tabs_key(2) == 50 &&
              tab_memory.tab_count == 3 && tab_memory.tabs[1].channel_id == 300 && tab_memory.tabs[1].guild_id == 11 &&
              tab_memory.tabs[1].label_bytes == 4,
          "tab pruning: after READY a channel of an unavailable guild stays, dimmed under its saved label, while a "
          "channel missing from an available guild or from a guild the account left is dropped");
    check(chat_state_store_upsert_guild(&state, {11, utf8_literal("Guild 2"), GuildMutationKind::create, 99}).ok() &&
              chat_state_store_upsert_role(&state, {11, 11, rights, {}}) &&
              chat_state_store_set_self_roles(&state, 11, 99, 99, {}) &&
              chat_state_store_upsert_channel(&state, {300, 11, 0, utf8_literal("Late"), 0, 0, 0}).ok() &&
              chat_state_store_upsert_channel(&state, {310, 11, 0, utf8_literal("Second"), 0, 0, 0}).ok(),
          "tab pruning: the guild's GUILD_CREATE brings its channels");
    chat_state_store_recompute_guild_permissions(&state, 11);
    view = tab_view();
    check(view->count == 3 && utf8_equal(view->items[1].label, utf8_literal("Late")) &&
              !(view->items[1].flags & kUiTabMissing) && tab_memory.tabs[1].label_bytes == 4 &&
              tab_memory.tabs[1].label[0] == 'L' && tab_memory.tabs[1].label[1] == 'a',
          "tab pruning: a late channel takes over the tab, which is no longer dimmed, and the file the new label");
    check(application_tabs_open_channel(310, true) && tab_view()->count == 4 &&
              chat_state_store_remove_channel(&state, 300) && tab_view()->count == 3 &&
              !application_tabs_contains(300) && application_tabs_contains(310),
          "tab pruning: CHANNEL_DELETE closes the tab of an available guild's channel");
    check(chat_state_store_remove_guild(&state, 11, true) && tab_view()->count == 3 &&
              (tab_view()->items[2].flags & kUiTabMissing) &&
              utf8_equal(tab_view()->items[2].label, utf8_literal("Second")),
          "tab pruning: an unavailable GUILD_DELETE keeps the tab, dimmed under its label");
    check(chat_state_store_remove_guild(&state, 11, false) && tab_view()->count == 2 &&
              !application_tabs_contains(310) && tab_memory.tab_count == 2,
          "tab pruning: GUILD_DELETE closes the tabs of the guild");
    check(chat_state_store_remove_channel(&state, 50) && tab_view()->count == 1 && !application_tabs_contains(50) &&
              application_tabs_contains(20) && tab_memory.tab_count == 1,
          "tab pruning: a conversation the account lost closes its tab");
    g_app.preferences.tabs_enabled = false;
    application_tabs_reset();
}
// The unread dot, the tooltip's server name, and a media tab's whole life through the strip.
void tab_polish_tests() noexcept {
    check(reset() && chat_state_store_upsert_channel(&ui->chat_state, {50, 0, 0, utf8_literal("DM"), 0, 0, 3}).ok(),
          "tab polish: fixture resets");
    auto& state = ui->chat_state;
    state.self_user_id = 99;
    g_app.preferences.tabs_enabled = true;
    const ChannelMemoryTab saved[]{saved_tab(20, 10, "Parent"), saved_tab(21, 10, "Other")};
    channel_memory_set_tabs(&tab_memory, {saved, 2});
    const UiTabStripView* view = tab_view();
    check(view && view->count == 2 && !(view->items[0].flags & kUiTabUnread) && !(view->items[1].flags & kUiTabUnread),
          "tab polish: read channels have no dot");
    // The sidebar's unread state marks the tab, except the one that shows.
    check(chat_state_store_note_message(&state, 20, 5000, 7, false, false) &&
              chat_state_store_note_message(&state, 21, 5001, 7, false, false) &&
              (tab_view()->items[0].flags & kUiTabUnread) && !(tab_view()->items[1].flags & kUiTabUnread) &&
              (tab_view()->items[1].flags & kUiTabActive),
          "tab polish: an unread channel's tab carries the dot, and the tab that shows never does");
    check(application_tabs_activate(0) && !(tab_view()->items[0].flags & kUiTabUnread) &&
              (tab_view()->items[1].flags & kUiTabUnread),
          "tab polish: the dot moves with the tab that shows");
    check(chat_state_store_note_message(&state, 21, 5002, 7, false, true) && !(tab_view()->items[1].flags & kUiTabUnread),
          "tab polish: reading the channel clears its dot");
    check(chat_state_store_note_message(&state, 21, 5003, 7, false, false) && (tab_view()->items[1].flags & kUiTabUnread) &&
              chat_state_store_set_channel_mute(&state, 21, true, false) && !(tab_view()->items[1].flags & kUiTabUnread),
          "tab polish: a muted channel keeps no dot");

    // The tooltip names the server after "#channel"; a conversation has none, and the hover alone asks for it.
    const bool parent_tip = tab_view({UiTabHitKind::tab, 0})->items[0].tooltip.bytes != 0;
    const UiTabStripView* hovered = tab_view({UiTabHitKind::tab, 1});
    check(parent_tip && utf8_equal(hovered->items[1].tooltip, utf8_literal("#Other \xC2\xB7 Guild")) &&
              !hovered->items[0].tooltip.bytes && !tab_view()->items[1].tooltip.bytes,
          "tab polish: the hovered channel tab's tooltip adds its server's name");
    check(application_tabs_open_channel(50, true) && !tab_view({UiTabHitKind::tab, 2})->items[2].tooltip.bytes &&
              utf8_equal(tab_view({UiTabHitKind::tab, 2})->items[2].label, utf8_literal("DM")),
          "tab polish: a conversation's tooltip is its label alone");

    // A media tab: the tooltip adds the line under its title, and the tab opens, hides on a channel switch, returns and
    // closes with its page.
    MediaPageSource clip = media_page_fake::source(MediaPageKind::video, "clip.mp4");
    clip.source = utf8_literal("#general \xC2\xB7 Guild \xC2\xB7 Maj");
    const u32 media_at = application_tabs_count();
    check(application_tabs_open_media(clip, false) && media_page_fake::shown != 0 && tab_view()->count == media_at + 1 &&
              (tab_view()->items[media_at].flags & kUiTabActive) && !(tab_view()->items[0].flags & kUiTabActive) &&
              utf8_equal(tab_view({UiTabHitKind::tab, static_cast<u8>(media_at)})->items[media_at].tooltip,
                         utf8_literal("clip.mp4 \xC2\xB7 #general \xC2\xB7 Guild \xC2\xB7 Maj")),
          "tab probe: a media tab opens, shows its page and names its source in the tooltip");
    check(application_tabs_activate(0) && !media_page_fake::shown && ui->chat_state.selected_channel_id == 20 &&
              (tab_view()->items[0].flags & kUiTabActive) && !(tab_view()->items[media_at].flags & kUiTabActive) &&
              media_page_fake::open_pages() == 1,
          "tab probe: switching channel hides the page and keeps its tab");
    check(application_tabs_activate(media_at) && media_page_fake::shown != 0 &&
              ui->chat_state.selected_channel_id == 20 && (tab_view()->items[media_at].flags & kUiTabActive) &&
              !(tab_view()->items[0].flags & kUiTabActive),
          "tab probe: returning to the tab shows the page again over the same channel");
    // Closing the page that shows shows its neighbour, as closing a channel tab does.
    check(application_tabs_move(media_at, 1) && application_tabs_close(1) && !media_page_fake::shown &&
              !media_page_fake::open_pages() && tab_view()->count == media_at &&
              ui->chat_state.selected_channel_id == 21 && (tab_view()->items[1].flags & kUiTabActive),
          "tab probe: closing the active page closes it and shows its neighbour tab");
    // "Open in new tab" on media that already has a tab shows that tab instead of adding a second one for its page.
    check(application_tabs_open_media(clip, true) && application_tabs_open_media(clip, true) &&
              tab_view()->count == media_at + 1 && media_page_fake::open_pages() == 1,
          "tab probe: the same media opens one tab");
    check(application_tabs_open_media(clip, false) && media_page_fake::shown != 0 && tab_view()->count == media_at + 1,
          "tab probe: opening it again shows its tab");
    check(application_tabs_close(media_at) && tab_view()->count == media_at && !media_page_fake::open_pages(),
          "tab probe: closing it leaves no tab without a page");
    // A drag follows its tab, not its position: a tab before it closing (a deleted channel) shifts its index.
    check(application_tabs_drag_begin(2) == 2 && application_tabs_drag_index() == 2 &&
              chat_state_store_remove_channel(&state, 20) && tab_view()->count == 2 &&
              application_tabs_drag_index() == 1 && application_tabs_key(1) == 50 &&
              (tab_view()->items[1].flags & kUiTabDragging) && !(tab_view()->items[0].flags & kUiTabDragging),
          "tab probe: a dragged tab stays the dragged one when the tabs before it change");
    application_tabs_drag_end();
    check(application_tabs_drag_index() >= kUiTabCapacity && !(tab_view()->items[1].flags & kUiTabDragging),
          "tab probe: ending the drag leaves none");
    g_app.preferences.tabs_enabled = false;
    application_tabs_reset();
}
void tests() noexcept {
    cursor_tests();
    context_point_tests();
    home_tests();
    tabs_tests();
    tab_pruning_tests();
    tab_polish_tests();
    check(reset(), "content navigation: real Core fixture initializes");
    g_app.pending_message_jump_channel_id = 21;
    g_app.pending_message_jump_message_id = 800;
    check(application_navigate_known_message(30, 0) && ui->active_channel_id == 30 &&
              ui->chat_state.selected_channel_id == 30 && selections == 1 && history_calls == 1 &&
              !g_app.pending_message_jump_channel_id && !g_app.pending_message_jump_message_id,
          "content navigation: zero-message public thread opens Core and active channel and requests "
          "channel history");
    check(application_navigate_known_message(30, 0) && history_calls == 1,
          "content navigation: repeated zero-message selection succeeds without duplicate history request");

    check(reset(), "content navigation: private fixture resets");
    check(!application_navigate_known_message(31, 0) && ui->active_channel_id == 21 &&
              ui->chat_state.selected_channel_id == 21 && !history_calls && !selections,
          "content navigation: unknown private membership cannot fabricate successful thread selection");
    check(chat_state_store_set_thread_self_membership(&ui->chat_state, 31, 5, true, true) &&
              application_navigate_known_message(31, 0) && ui->active_channel_id == 31 &&
              ui->chat_state.selected_channel_id == 31,
          "content navigation: authoritative private self membership admits real zero-message selection");

    check(reset(), "content navigation: unreadable fixture resets");
    check(chat_state_store_upsert_role(&ui->chat_state, {10, 10, kPermissionViewChannel, {}}),
          "content navigation: read-history permission removed");
    chat_state_store_recompute_guild_permissions(&ui->chat_state, 10);
    check(!application_navigate_known_message(30, 0) && !selections && !history_calls &&
              ui->active_channel_id == 21,
          "content navigation: unreadable thread cannot report success or request history");

    check(reset() && chat_state_store_upsert_channel(
                         &ui->chat_state, {30, 10, 999, utf8_literal("Missing parent"), 0, 0, 11})
                         .ok(),
          "content navigation: stale parent fixture");
    check(!application_navigate_known_message(30, 0) && !history_calls && !selections,
          "content navigation: thread with missing parent authority cannot open");
    check(
        !application_navigate_known_message(999, 0) && !application_navigate_known_message(40, 0),
        "content navigation: missing channel and forum parent do not masquerade as message-channel opens");

    check(reset(), "content navigation: projection failure fixture resets");
    text_ok = false;
    check(
        !application_navigate_known_message(30, 0) && stops == 1 && !g_app.account.instance,
        "content navigation: presentation failure with retained Core and active IDs cannot report success");
    check(reset(), "content navigation: mismatched active fixture resets");
    wrong_active = true;
    check(!application_navigate_known_message(30, 0),
          "content navigation: active-channel postcondition is required");
    check(reset(), "content navigation: mismatched Core selection fixture resets");
    wrong_selected = true;
    check(!application_navigate_known_message(30, 0),
          "content navigation: Core selected-channel postcondition is required");

    check(reset(), "content navigation: message jump fixture resets");
    check(application_navigate_known_message(30, 900) && ui->active_channel_id == 30 &&
              ui->chat_state.selected_channel_id == 30 && g_app.pending_message_jump_channel_id == 30 &&
              g_app.pending_message_jump_message_id == 900 && history_calls >= 1,
          "content navigation: nonzero message jump preserves pending identity and authenticated history "
          "machinery");
    check(reset(), "content navigation: history failure fixture resets");
    history_ok = false;
    check(!application_navigate_known_message(30, 0) && history_calls == 1,
          "content navigation: channel-only history admission failure cannot report success");
    check(reset(), "content navigation: message history failure fixture resets");
    history_ok = false;
    check(!application_navigate_known_message(30, 901) && !g_app.pending_message_jump_channel_id &&
              !g_app.pending_message_jump_message_id,
          "content navigation: failed message-history admission clears pending jump and returns false");
    check(reset(), "content navigation: resident jump fixture resets");
    resident_jump = true;
    check(application_navigate_known_message(30, 902) && history_calls == 1 &&
              !g_app.pending_message_jump_channel_id && !g_app.pending_message_jump_message_id,
          "content navigation: resident jump settles pending identity without around-history request");
    check(reset(), "content navigation: recognized permalink routing fixture resets");
    check(application_open_content_url(utf8_literal("https://discord.com/channels/10/999/900")) &&
              !selections && !history_calls,
          "content navigation: uncached internal link is consumed without external confirmation");
    check(application_open_content_url(utf8_literal("https://discord.com/channels/999/30/900")) &&
              !selections && !history_calls,
          "content navigation: wrong-guild internal link is consumed without external confirmation");
    check(chat_state_store_upsert_role(&ui->chat_state, {10, 10, kPermissionViewChannel, {}}),
          "content navigation: denied permalink authority changes");
    chat_state_store_recompute_guild_permissions(&ui->chat_state, 10);
    check(application_open_content_url(utf8_literal("https://discord.com/channels/10/30/900")) &&
              !selections && !history_calls,
          "content navigation: recognized denied internal link cannot fall through to external opener");
    check(reset(), "content navigation: actual semantic dispatch fixture resets");
    const auto url = utf8_literal("https://discord.com/channels/10/999/900");
    const auto inserted = message_store_upsert(&g_app.message_view.store, {909, 21, 99, 1000, 0, url, 0});
    const auto* resident =
        inserted.ok() ? message_store_resolve(g_app.message_view.store, inserted.handle) : nullptr;
    check(resident != nullptr, "content navigation: source permalink message is resident");
    if (resident) {
        native_hit = {resident->id,
                      900,
                      resident->revision,
                      0,
                      static_cast<u16>(url.bytes),
                      MessageMarkdownSemanticKind::message_link,
                      true};
        check(handle_content_click(window_test::pointer_down(window_test::x_of(0), window_test::y_of(0))) && typed_links == 1 && captured_sources == 1 &&
                  captured_source.channel_id == 21 && captured_source.message_id == resident->id &&
                  captured_source.handle.slot == inserted.handle.slot &&
                  captured_source.handle.generation == inserted.handle.generation &&
                  captured_source.revision == resident->revision && captured_target.channel_id == 999 &&
                  captured_target.message_id == 900 && !selections,
              "content navigation: real hit dispatch captures exact source handle before uncached target "
              "hydration");
        native_hit.kind = MessageMarkdownSemanticKind::link;
        check(handle_content_click(window_test::pointer_down(window_test::x_of(0), window_test::y_of(0))) && typed_links == 2,
              "content navigation: masked ordinary link uses the same internal typed owner");
        ++native_hit.message_revision;
        check(
            handle_content_click(window_test::pointer_down(window_test::x_of(0), window_test::y_of(0))) && typed_links == 2 && captured_sources == 2,
            "content navigation: stale semantic revision cannot start navigation or external confirmation");
    }
}
} // namespace maicord::content_navigation_test

namespace maicord::app_detail {
Application g_app{};
// The complete shipping content-input TU also contains unrelated menus,
// attachment commands and hit testing. These boundaries stay inactive here.
bool application_message_navigation_source(u64 channel_id, u64 message_id, MessageHandle handle,
                                           u32 revision,
                                           ApplicationMessageNavigationSource* output) noexcept {
    using namespace content_navigation_test;
    ++captured_sources;
    if (!output)
        return false;
    *output = {};
    output->channel_id = channel_id;
    output->message_id = message_id;
    output->handle = handle;
    output->revision = revision;
    captured_source = *output;
    return true;
}
bool application_message_navigation_link(const ApplicationMessageNavigationSource& source,
                                         const MessageLink& target) noexcept {
    ++content_navigation_test::typed_links;
    content_navigation_test::captured_source = source;
    content_navigation_test::captured_target = target;
    return false; // A denied shared owner remains a consumed internal click.
}
bool application_friends_show() noexcept {
    using namespace content_navigation_test;
    ++friends_shown;
    friends_visible = chat_state_store_select_home(&ui->chat_state).ok();
    return friends_visible;
}
bool application_friends_show_requests() noexcept {
    return application_friends_show();
}
ChannelMemoryEntry application_channel_memory_recall(u64) noexcept {
    return {};
}
// No game mention is clicked here (src/app/chat/game_profile.hpp).
bool application_game_profile_open(u64) noexcept {
    return false;
}
bool refresh_message_composer_ui() noexcept {
    return false;
}
bool application_downloads_save_target(float x, float y) noexcept {
    return ui_rect_contains(content_navigation_test::save_button, x, y);
}
bool application_downloads_save_press(float, float) noexcept {
    ++content_navigation_test::save_presses;
    if (g_app.window)
        SetCapture(win32_hwnd(g_app.window));
    return true;
}
const Theme& application_theme() noexcept {
    static const Theme value{};
    return value;
}
Utf8View application_message_mutation_notice_text() noexcept {
    return {};
}
void application_message_mutation_notice_begin(ApplicationMessageMutationKind, u64, u64) noexcept {
}
void application_message_mutation_notice_pending(ApplicationMessageMutationKind, u64, u64) noexcept {
}
void application_message_mutation_notice_succeeded(ApplicationMessageMutationKind, u64, u64) noexcept {
}
void application_message_mutation_notice_failed(ApplicationMessageMutationKind, u64, u64,
                                                const RestClientResponse&) noexcept {
}
bool application_friends_active() noexcept {
    return content_navigation_test::friends_visible && content_navigation_test::ui->chat_state.home_selected;
}
void application_friends_hide() noexcept {
    content_navigation_test::friends_visible = false;
}
bool synchronize_navigation_view() noexcept {
    return true;
}
bool synchronize_selected_channel() noexcept {
    using namespace content_navigation_test;
    ++selections;
    ui->active_channel_id = wrong_active ? 21 : ui->chat_state.selected_channel_id;
    if (wrong_selected)
        chat_state_store_select_channel(&ui->chat_state, 21);
    return true;
}
bool synchronize_shell_layout() noexcept {
    return true;
}
bool refresh_account_text(bool) noexcept {
    return content_navigation_test::text_ok;
}
bool application_stop_account() noexcept {
    ++content_navigation_test::stops;
    g_app.account.instance = nullptr;
    g_app.account.status.running = false;
    // Real account stop can retain the UI/Core projection until its caller
    // explicitly releases it. Preserve those IDs to expose false-success bugs.
    return true;
}
bool pump_message_history() noexcept {
    ++content_navigation_test::history_calls;
    return content_navigation_test::history_ok;
}
bool scroll_to_resident_message(u64) noexcept {
    return content_navigation_test::resident_jump;
}
bool application_request_frame() noexcept {
    return true;
}
// Category chevrons settle at once here (the shared scheduler is covered by the surface motion host test).
float application_motion_tick(UiMotionTransition*, const UiMotionScope&, float target, u64,
                              const UiMotionStyle&) noexcept {
    return target;
}
bool application_motion_hover_tick(UiMotionHover* hover, u64 id, u64 now, const UiMotionStyle& style) noexcept {
    return ui_motion_hover_tick(hover, id, now, style, false);
}
// The account's channel memory, as the real one holds its tabs.
u64 application_channel_memory_profile() noexcept {
    return content_navigation_test::tab_profile;
}
ConstSlice<ChannelMemoryTab> application_channel_memory_tabs() noexcept {
    return {content_navigation_test::tab_memory.tabs, content_navigation_test::tab_memory.tab_count};
}
void application_channel_memory_set_tabs(ConstSlice<ChannelMemoryTab> tabs) noexcept {
    if (content_navigation_test::tab_profile)
        channel_memory_set_tabs(&content_navigation_test::tab_memory, tabs);
}
} // namespace maicord::app_detail

namespace maicord {
MessageTextHit renderer_content_hit_test(const D3D11Renderer&, int, int) noexcept {
    return content_navigation_test::native_hit;
}
RendererNavigationHit renderer_navigation_hit_test(const D3D11Renderer&, int, int) noexcept {
    return {};
}
bool renderer_guild_rail_hit_test(const D3D11Renderer&, int, int) noexcept {
    return false;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_content_navigation_entry() noexcept {
    using namespace maicord;
    content_navigation_test::ui = static_cast<app_detail::AccountUiRuntime*>(VirtualAlloc(
        nullptr, sizeof(app_detail::AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!content_navigation_test::ui)
        ExitProcess(1);
    content_navigation_test::tests();
    VirtualFree(content_navigation_test::ui, 0, MEM_RELEASE);
    ExitProcess(content_navigation_test::failures ? 1 : 0);
}

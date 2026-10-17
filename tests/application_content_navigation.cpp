// Exercise the publicly exposed shipping navigator, including its real Core
// selection and channel authority. Surrounding presentation/history are thin
// deterministic boundaries; no cloned navigation helper is used.
#include "../src/app/application_content_input.cpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord::content_navigation_test {
using namespace app_detail;
u32 failures, history_calls, refreshes, selections, stops, frames;
bool navigation_ok = true, selection_ok = true, text_ok = true, history_ok = true, resident_jump;
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
ChannelRecord channels[8];
u16 channel_order[8];
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
    history_calls = refreshes = selections = stops = frames = 0;
    native_hit = {};
    typed_links = captured_sources = 0;
    captured_source = {};
    captured_target = {};
    navigation_ok = selection_ok = text_ok = history_ok = true;
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
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
                                        {channel_order, 8}, {names, sizeof(names)}) ||
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
    g_app.window = CreateWindowExW(0, L"STATIC", L"cursor fixture", WS_POPUP, 0, 0, 800, 600, nullptr,
                                   nullptr, nullptr, nullptr);
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
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
    save_button = {};
}
void tests() noexcept {
    cursor_tests();
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
        check(handle_content_click(0) && typed_links == 1 && captured_sources == 1 &&
                  captured_source.channel_id == 21 && captured_source.message_id == resident->id &&
                  captured_source.handle.slot == inserted.handle.slot &&
                  captured_source.handle.generation == inserted.handle.generation &&
                  captured_source.revision == resident->revision && captured_target.channel_id == 999 &&
                  captured_target.message_id == 900 && !selections,
              "content navigation: real hit dispatch captures exact source handle before uncached target "
              "hydration");
        native_hit.kind = MessageMarkdownSemanticKind::link;
        check(handle_content_click(0) && typed_links == 2,
              "content navigation: masked ordinary link uses the same internal typed owner");
        ++native_hit.message_revision;
        check(
            handle_content_click(0) && typed_links == 2 && captured_sources == 2,
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
// application_content_input.cpp opens the server header popout.
bool application_guild_menu_toggle(bool) noexcept {
    return false;
}
bool application_friends_show() noexcept {
    return false;
}
bool application_private_channel_row_close(u64, float, float) noexcept {
    return false;
}
bool application_private_channel_requests_mode() noexcept {
    return false;
}
void application_private_channel_set_requests_mode(bool) noexcept {}
bool application_message_action_bar_activate(const MessageActionBarHit&, bool) noexcept {
    return false;
}
bool application_message_forward_open(u64) noexcept {
    return false;
}
bool application_guild_card_open(u64, UiRect) noexcept {
    return false;
}
bool application_profile_open(u64, UiRect, ProfileAnchor) noexcept {
    return false;
}
bool application_plugins_slot_action(u64, NativePluginSdkSlotTarget, u64) noexcept {
    return false;
}
bool application_plugins_slot_activate(u64, NativePluginSdkSlotTarget, u64, UiRect) noexcept {
    return false;
}
bool application_plugins_render_click(const MessageTextHit&, UiRect) noexcept {
    return false;
}
bool open_reaction_picker(u64, UiRect) noexcept {
    return false;
}
float client_dip_x(LPARAM) noexcept {
    return 0;
}
float client_dip_y(LPARAM) noexcept {
    return 0;
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
        SetCapture(g_app.window);
    return true;
}
bool application_poll_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
bool application_video_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
bool application_gif_favorite_star_target(float, float) noexcept {
    return false;
}
bool application_image_viewer_open_at(float, float, bool) noexcept {
    return false;
}
bool application_video_open_focused() noexcept {
    return false;
}
bool application_composer_fill_command(Utf8View) noexcept {
    return true;
}
bool begin_message_reply(u64) noexcept {
    return false;
}
bool queue_message_reaction(u64, u32) noexcept {
    return false;
}
bool queue_message_reaction_emoji(u64, u64, Utf8View, bool) noexcept {
    return false;
}
bool application_quick_switcher_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool dismiss_popup() noexcept {
    return false;
}
bool open_message_context_popup(u64, UiRect) noexcept {
    return false;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
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
    return false;
}
void application_friends_hide() noexcept {
}
bool application_thread_browser_active() noexcept {
    return false;
}
bool application_thread_browser_open(u64) noexcept {
    return false;
}
void application_thread_browser_close() noexcept {
}
void application_read_state_activity() noexcept {
}
bool synchronize_navigation_view() noexcept {
    return content_navigation_test::navigation_ok;
}
bool synchronize_selected_channel() noexcept {
    using namespace content_navigation_test;
    ++selections;
    if (!selection_ok)
        return false;
    ui->active_channel_id = wrong_active ? 21 : ui->chat_state.selected_channel_id;
    if (wrong_selected)
        chat_state_store_select_channel(&ui->chat_state, 21);
    return true;
}
bool synchronize_shell_layout() noexcept {
    return true;
}
bool refresh_account_text(bool) noexcept {
    ++content_navigation_test::refreshes;
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
bool animate_guild_rail_scroll_from(float) noexcept {
    return true;
}
bool application_request_frame() noexcept {
    ++content_navigation_test::frames;
    return true;
}
bool application_guild_invite_open_url(Utf8View) noexcept {
    return false;
}
bool application_link_open(Utf8View, bool) noexcept {
    return true;
}
bool application_link_channel_trusted(u64) noexcept {
    return true;
}
bool application_guild_folders_toggle(u64) noexcept {
    return false;
}
bool application_navigation_drag_press(LPARAM) noexcept {
    return false;
}
bool application_guild_lifecycle_open_create() noexcept {
    return false;
}
bool application_guild_community_open_events(u64) noexcept {
    return false;
}
bool application_invite_card_click(u64, UiWidgetHandle, float, float) noexcept {
    return false;
}
bool application_message_component_click(u64, UiWidgetHandle, UiRect) noexcept {
    return false;
}
bool application_pinned_messages_toggle() noexcept {
    return true;
}
bool refresh_account_frame(bool) noexcept {
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {}
bool application_voice_join(u64, u64) noexcept {
    return true;
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

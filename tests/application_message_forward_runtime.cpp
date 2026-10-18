#include "application_shortcuts.hpp"
#include "application_downloads_inactive.hpp"
#include "application_inbox_inactive.hpp"
#include "application_poll_inactive.hpp"
#include "application_member_moderation_inactive.hpp"
#include "application_guild_invite_inactive.hpp"
// Shipping forwarding host and production input router, exercised with a hidden
// native window and authoritative model/outbox storage. No network is created.
#include "../src/app/application_message_forward.cpp"
#include "application_input_routing.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_private_conversation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "test_output.hpp"

#include <Windows.h>

namespace maicord::forward_runtime_test {
using namespace app_detail;
u32 failures, underlying_input, suspends, synchronizations, scoped_shortcuts;
bool settings, theme_editor;
bool scoped_shortcut_allowed = true;
GuildRecord guilds[2];
u16 guild_order[2];
ChannelRecord channels[8];
u16 channel_order[8];
u8 names[2048];
UserRecord users[4];
u32 user_order[4];
u8 user_names[512];
RelationshipRecord relationships[4];
RelationshipStore relationship_store;
MessageRecord records[16];
u32 order[16], heights[16];
u64 prefixes[16];
u8 text[4096];
UiVariableList list;
MessageRichRecord rich_records[4];
u16 rich_order[2];
u8 rich_text[2048];
MessageDraftRecord draft_records[4];
u8 draft_text[512], request_text[kMessageComposerMaximumBytes];
char draft[256];
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
void modifiers(bool control = false, bool alt = false, bool shift = false) noexcept {
    BYTE clean[256]{};
    clean[VK_CONTROL] = control ? 0x80 : 0;
    clean[VK_SHIFT] = shift ? 0x80 : 0;
    clean[VK_MENU] = alt ? 0x80 : 0;
    check(SetKeyboardState(clean) != FALSE, "Forward runtime keyboard state");
}
bool channel(u64 id, Utf8View name) noexcept {
    auto& state = g_app.account_ui->chat_state;
    ChannelInput input{};
    input.id = id;
    input.guild_id = 10;
    input.name = name;
    if (!chat_state_store_upsert_channel(&state, input).ok())
        return false;
    auto* c = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id));
    c->permissions_resolved = true;
    c->actions = channel_action_view | channel_action_read_history |
                 channel_action_send_messages;
    ++state.permission_mutations;
    return true;
}
bool edit(Utf8View value) noexcept {
    const auto target = application_message_forward_edit_target();
    u32 length = 0;
    u16 converted[128]{};
    if (value.bytes > 128 || text_input_length(target, &length) != TextInputResult::unchanged)
        return false;
    for (u32 i = 0; i < value.bytes; ++i)
        converted[i] = static_cast<u8>(value.data[i]);
    TextInputChange change{};
    const auto result = text_input_replace(target, 0, length, {converted, value.bytes}, &change);
    return (result == TextInputResult::changed || result == TextInputResult::unchanged) &&
           application_message_forward_edit_changed(true);
}
#include "application_scoped_shortcut_test.inc"
void run() noexcept {
    BYTE saved[256]{};
    const bool keyboard_saved = GetKeyboardState(saved) != FALSE;
    modifiers();
    g_app.window = CreateWindowExW(0, L"STATIC", L"Forward isolated fixture", WS_POPUP, 0, 0, 1200, 900,
                                   nullptr, nullptr, nullptr, nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui, "Forward runtime hidden window and scoped account");
    if (!g_app.window || !g_app.account_ui)
        return;
    SetFocus(g_app.window);
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.renderer.width = 1200;
    g_app.renderer.height = 900;
    g_app.account.instance = &relationship_store;
    g_app.account.status.profile_id = 4;
    g_app.plugin_account_scope_serial = 7;
    auto& state = g_app.account_ui->chat_state;
    auto& ui = *g_app.account_ui;
    const bool ready =
        chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
                                       {channel_order, 8}, {names, sizeof(names)}) &&
        user_store_initialize(&ui.user_store, {users, 4}, {user_order, 4},
                                      {user_names, sizeof(user_names)}) &&
        relationship_store_initialize(&relationship_store, {relationships, 4}) &&
        chat_state_store_upsert_guild(
            &state, {10, utf8_literal("Workbench"), GuildMutationKind::create, 3})
            .ok() &&
        channel(101, utf8_literal("source")) && channel(102, utf8_literal("general")) &&
        channel(103, utf8_literal("engineering")) &&
        user_store_upsert(&ui.user_store, 4, utf8_literal("Alice")).ok() &&
        message_rich_store_initialize(&ui.message_rich_store, {rich_records, 4}, {rich_order, 2},
                                      {rich_text, sizeof(rich_text)}) &&
        message_store_initialize(&g_app.message_view.store, {records, 16}, {order, 16},
                                 {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 16}, {prefixes, 16}, 0, 100) &&
        message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store, &list, 101,
                                    100, true) &&
        message_composer_initialize(&g_app.message_composer, {draft, sizeof(draft)}) &&
        message_draft_store_initialize(&ui.message_drafts, {draft_records, 4},
                                       {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&ui.message_composer_controller, &g_app.message_composer,
                                               &ui.message_drafts, &g_app.message_view.timeline,
                                               {request_text, sizeof(request_text)}) &&
        message_composer_restore(&g_app.message_composer, 101, utf8_literal("Retained draft"), 14, 14) &&
        message_store_upsert(&g_app.message_view.store,
                             {500, 101, 4, 1, 0, utf8_literal("Original source"), 0})
            .ok();
    check(ready, "Forward runtime authoritative storage initializes");
    if (!ready)
        return;
    check(ui_variable_list_initialize(&list, {heights, 16}, {prefixes, 16}, g_app.message_view.store.count,
                                      100),
          "Forward runtime resident list tracks the authoritative source fixture");
    state.self_user_id = 3;
    state.selected_guild_id = 10;
    state.selected_channel_id = ui.active_channel_id = 101;
    g_app.text_form = reinterpret_cast<ApplicationTextForm*>(&relationship_store);
    check(application_message_forward_available(500),
          "Forward runtime inactive retained form owner does not block forwarding");
    g_app.text_form = nullptr;
    check(!application_message_forward_owner_bytes() && application_message_forward_available(500),
          "Forward runtime allocation is lazy and eligible source is admitted");
    check(application_message_forward_open(500) && application_message_forward_active() &&
              application_message_forward_edit_focused() && suspends && synchronizations &&
              g_app.renderer.message_forward_scene,
          "Forward runtime shipping modal opens a native document");
    if (!application_message_forward_active())
        return;
    modifiers(false, false, true);
    underlying_input = 0;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) &&
              !application_message_forward_active() && !scoped_shortcuts && !underlying_input &&
              route_application_input(UiInputKind::key_down, VK_ESCAPE, true) &&
              route_application_input(UiInputKind::key_up, VK_ESCAPE) && !scoped_shortcuts,
          "Scoped input: actual Forward modal closes on Shift Escape without admitting read work");
    modifiers();
    check(application_message_forward_open(500) && application_message_forward_active(),
          "Forward runtime: reopen after scoped-input priority check");
    if (!application_message_forward_active())
        return;
    auto* allocation = g_app.message_forward;
    auto* q = g_app.message_forward;
    const auto stale_edit = application_message_forward_edit_target();
    const auto* accepted_bank = q->scene.accepted;
    check(edit(utf8_literal("general")) && q->scene.accepted_count == 1 &&
              q->scene.accepted[0].channel_id == 102,
          "Forward runtime native query publishes send-capable cached destinations");
    const u32 rebuilds = q->controller.candidates.rebuild_count;
    check(application_message_forward_synchronize() && q->controller.candidates.rebuild_count == rebuilds &&
              q->scene.accepted == accepted_bank,
          "Forward runtime unchanged query avoids scans and result reallocations");
    key(VK_RETURN);
    check(q->controller.destination_count == 1 && !ui.message_composer_controller.request_count,
          "Forward runtime Enter selects a destination without sending");
    key(VK_TAB);
    const auto caption_target = application_message_forward_edit_target();
    check(application_message_forward_caption_focused() && !text_input_ready(stale_edit) &&
              edit(utf8_literal("Caption")),
          "Forward runtime Tab switches the native document and isolates the optional caption");
    underlying_input = 0;
    route_application_input(UiInputKind::system_character, 'x');
    check(!underlying_input &&
              same(message_composer_text(g_app.message_composer), utf8_literal("Retained draft")),
          "Forward runtime modal never mutates the underlying draft");
    key(VK_RETURN, true);
    check(q->controller.committed && ui.message_composer_controller.request_count == 1 &&
              !text_input_ready(caption_target) && !application_message_forward_edit_model(),
          "Forward runtime explicit send freezes and queues the isolated request");
    key(VK_RETURN, true);
    check(ui.message_composer_controller.request_count == 1,
          "Forward runtime duplicate send cannot duplicate admission");
    MessageComposerSubmit submit{};
    check(message_composer_controller_prepare_submit(&ui.message_composer_controller,
                                                     clock_monotonic_milliseconds(), &submit)
                  .ok() &&
              submit.channel_id == 102 && submit.forward_reference.message_id == 500 &&
              submit.forward_reference.channel_id == 101 && submit.forward_reference.guild_id == 10 &&
              same(submit.content, utf8_literal("Caption")),
          "Forward runtime owned network request carries exact origin and caption");
    const auto nonce = submit.nonce;
    application_message_forward_close();
    ui.active_channel_id = 103;
    state.selected_channel_id = 103;
    ++state.selection_revision;
    check(application_message_forward_synchronize() && message_forward_pending(q->controller) &&
              ui.message_composer_controller.request_count == 1,
          "Forward runtime closing and channel navigation preserve queued work");
    check(message_composer_controller_mark_submitted(&ui.message_composer_controller, nonce).ok() &&
              message_composer_controller_observe_http(&ui.message_composer_controller, 403,
                                                       MessageSendHttpDisposition::terminal_failure,
                                                       clock_monotonic_milliseconds())
                  .ok(),
          "Forward runtime mock transport rejection is tracked");
    application_message_forward_synchronize();
    ui.active_channel_id = 101;
    state.selected_channel_id = 101;
    ++state.selection_revision;
    check(application_message_forward_open(500) && application_message_forward_active() &&
              q->controller.destinations[0].state == MessageForwardDestinationState::failed &&
              q == allocation,
          "Forward runtime failed progress reopens for explicit retry without allocation");
    key(VK_RETURN, true);
    check(q->controller.destinations[0].nonce == nonce &&
              q->controller.destinations[0].state == MessageForwardDestinationState::queued &&
              ui.message_composer_controller.request_count == 1,
          "Forward runtime retry keeps the original idempotent nonce");
    check(message_composer_controller_mark_submitted(&ui.message_composer_controller, nonce).ok() &&
              message_composer_controller_observe_http(&ui.message_composer_controller, 200,
                                                       MessageSendHttpDisposition::success,
                                                       clock_monotonic_milliseconds())
                  .ok(),
          "Forward runtime mock successful transport is tracked");
    GatewayMessageEvent response{};
    response.id = 600;
    response.channel_id = 102;
    response.author_id = 3;
    response.fields = gateway_message_field_id | gateway_message_field_channel_id |
                      gateway_message_field_author_id | gateway_message_field_content |
                      gateway_message_field_timestamp | gateway_message_field_nonce;
    response.nonce = nonce;
    response.content = utf8_literal("Caption");
    response.timestamp_ms = clock_unix_milliseconds();
    const auto source_count = g_app.message_view.store.count;
    check(
        message_composer_controller_observe_response(&ui.message_composer_controller, nonce, response, 101)
            .ok(),
        "Forward runtime authoritative destination response confirms delivery");
    MessageSendCompletion completion{};
    while (message_composer_controller_take_completion(&ui.message_composer_controller, &completion))
        application_message_forward_completion(completion);
    application_message_forward_synchronize();
    check(q->controller.destinations[0].state == MessageForwardDestinationState::sent &&
              g_app.message_view.store.count == source_count &&
              !ui.message_composer_controller.request_count,
          "Forward runtime destination confirmation never fabricates a row in the source timeline");
    application_message_forward_close();
    check(application_message_forward_open(500) && !q->controller.committed &&
              !q->controller.destination_count && q == allocation && edit(utf8_literal("general")),
          "Forward runtime completed source can be forwarded again in retained storage");
    key(VK_RETURN);
    key(VK_RETURN, true);
    check(q->controller.committed && ui.message_composer_controller.request_count == 1,
          "Forward runtime new explicit forward after completion receives its own request");
    const auto cancel_bounds = q->scene.cancel_rectangle;
    const LPARAM cancel_point =
        MAKELPARAM(static_cast<i16>((cancel_bounds.left + cancel_bounds.right) * .5f),
                   static_cast<i16>((cancel_bounds.top + cancel_bounds.bottom) * .5f));
    application_message_forward_window_message(WM_LBUTTONDOWN, MK_LBUTTON, cancel_point);
    application_message_forward_window_message(WM_LBUTTONUP, 0, cancel_point);
    check(!application_message_forward_active() && !ui.message_composer_controller.request_count,
          "Forward runtime Cancel remaining explicitly discards unsent work");
    check(application_message_forward_open(500) && edit(utf8_literal("general")),
          "Forward runtime explicit cancellation unlocks a fresh forward");
    key(VK_RETURN);
    key(VK_RETURN, true);
    const auto hit = q->scene.panel;
    const LPARAM point = MAKELPARAM(static_cast<i16>(hit.left + 5), static_cast<i16>(hit.top + 5));
    application_message_forward_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point);
    application_message_forward_close();
    check(application_message_forward_window_message(WM_LBUTTONUP, 0, point) &&
              GetCapture() != g_app.window,
          "Forward runtime close releases pointer capture and consumes its release tail");
    application_message_forward_open(500);
    ++g_app.plugin_account_scope_serial;
    check(application_message_forward_synchronize() && !application_message_forward_active() &&
              !ui.message_composer_controller.request_count && !application_message_forward_edit_target(),
          "Forward runtime account epoch closes modal and cancels only unsent work");
    const auto forwarded = message_store_upsert(&g_app.message_view.store, {501, 101, 4, 1, 0, {}, 0});
    check(ui_variable_list_initialize(&list, {heights, 16}, {prefixes, 16}, g_app.message_view.store.count,
                                      100),
          "Forward runtime resident list tracks the retained snapshot fixture");
    GatewayMessageEvent forwarded_event{};
    forwarded_event.id = 501;
    forwarded_event.fields =
        gateway_message_field_message_reference | gateway_message_field_message_snapshots;
    forwarded_event.reference = {};
    forwarded_event.reference.type = 1;
    forwarded_event.reference.channel_id = 777;
    forwarded_event.reference.message_id = 888;
    forwarded_event.snapshot.present = true;
    forwarded_event.snapshot.content = utf8_literal("Authorless retained child");
    forwarded_event.snapshot.fields = gateway_message_field_content;
    const auto* forwarded_record =
        forwarded.ok() ? message_store_resolve(g_app.message_view.store, forwarded.handle) : nullptr;
    check(forwarded_record && message_rich_store_apply(&ui.message_rich_store, 501,
                                                       forwarded_record->revision, forwarded_event)
                                  .ok(),
          "Forward runtime retained snapshot is admitted into the rich store");
    check(application_message_forward_open(501) && application_message_forward_active(),
          "Forward runtime empty outer snapshot source opens");
    check(same({q->controller.source.preview, q->controller.source.preview_bytes},
               utf8_literal("Forwarded message\n> Authorless retained child")),
          "Forward runtime empty outer snapshot preview retains authorless child text");
    check(q->controller.source.reference.message_id == 501 &&
              q->controller.source.reference.channel_id == 101 &&
              q->controller.source.reference.guild_id == 10,
          "Forward runtime snapshot preview preserves the outer origin reference");
    check(same({q->controller.source.author, q->controller.source.author_bytes}, utf8_literal("Alice")),
          "Forward runtime snapshot preview preserves the outer author");
    const auto snapshot_document = application_message_forward_edit_target();
    check(message_store_delete(&g_app.message_view.store, 501) &&
              message_store_upsert(&g_app.message_view.store,
                                   {501, 101, 4, 2, 0, utf8_literal("Replacement"), 0})
                  .ok() &&
              application_message_forward_synchronize() && !application_message_forward_active() &&
              !text_input_ready(snapshot_document),
          "Forward runtime replaced source generation retires the captured snapshot document");
    check(application_message_forward_open(501) && application_message_forward_active(),
          "Forward runtime authoritative replacement can open a new captured source");
    const auto release_panel = g_app.message_forward->scene.panel;
    application_message_forward_window_message(
        WM_LBUTTONDOWN, MK_LBUTTON,
        MAKELPARAM(static_cast<i16>(release_panel.left + 5), static_cast<i16>(release_panel.top + 5)));
    application_message_forward_release();
    check(!application_message_forward_owner_bytes() && !g_app.renderer.message_forward_scene &&
              !application_message_forward_caret_wake(),
          "Forward runtime release retires borrowed UI storage and wake");
    check(!application_message_forward_window_message(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1)) &&
              !application_message_forward_window_message(WM_LBUTTONUP, 0, MAKELPARAM(1, 1)),
          "Forward runtime a fresh pointer gesture retires an abandoned close tail");
    // scoped_shortcut_routing() runs in the Quick switcher runtime against the
    // same production router; this file keeps only the Forward modal case.
    message_composer_release(&g_app.message_composer);
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    if (keyboard_saved)
        check(SetKeyboardState(saved) != FALSE, "Forward runtime keyboard state restores");
}
} // namespace maicord::forward_runtime_test
namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position)
        return false;
    *position = 0;
    return true;
}
u64 text_edit_signature(Utf8View text) noexcept {
    return hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
}
void renderer_bind_focus_owner(D3D11Renderer* r, const UiWidgetManager* owner) noexcept {
    if (r)
        r->focus_owner = owner;
}
bool clipboard_write_utf8(HWND, Utf8View) noexcept {
    return true;
}
ClipboardTextResult clipboard_read_utf8(HWND, Slice<char>) noexcept {
    return {0, ClipboardTextError::unavailable};
}
namespace app_detail {
bool handle_thread_header_key(u32, bool, bool) noexcept {
    return false;
}
void application_quick_switcher_close() noexcept {
}
bool application_quick_switcher_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool refresh_account_text(bool) noexcept {
    return true;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1200, 900};
}
void application_composer_text_input_suspend() noexcept {
    ++forward_runtime_test::suspends;
}
bool application_composer_text_input_synchronize() noexcept {
    ++forward_runtime_test::synchronizations;
    return true;
}
void application_composer_text_input_notify_layout() noexcept {
}
bool application_composer_text_input_composing() noexcept {
    return false;
}
void application_sync_attachment_drop_target() noexcept {
}
void application_video_blur() noexcept {
}
bool synchronize_shell_layout() noexcept {
    return true;
}
void application_friends_hide() noexcept {
}
void application_pinned_messages_close() noexcept {
}
bool application_search_close_panel() noexcept {
    return true;
}
void application_account_menu_clear() noexcept {
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
void end_message_composer_pointer_selection() noexcept {
}
void end_message_text_selection() noexcept {
}
bool clear_message_text_selection(bool) noexcept {
    return true;
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    return true;
}
void application_media_picker_close() noexcept {
}
void application_settings_close(bool) noexcept {
}
bool application_profile_close() noexcept {
    return true;
}
bool queue_private_conversation(PrivateConversationOperation, u64) noexcept {
    return true;
}
bool application_navigate_notification(u64 channel_id, u64) noexcept {
    auto& state = g_app.account_ui->chat_state;
    const auto* c = chat_state_store_find_channel(state, channel_id);
    if (!c)
        return false;
    state.selected_channel_id = channel_id;
    state.selected_guild_id = c->guild_id;
    ++state.selection_revision;
    return true;
}
bool select_content_guild(u64) noexcept {
    return true;
}
bool application_forum_form_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_forum_form_active() noexcept {
    return false;
}
bool application_text_form_input(UiInputKind, u32, bool) noexcept {
    return g_app.renderer.text_form && g_app.renderer.text_form->scene &&
           g_app.renderer.text_form->scene->active;
}
bool application_theme_editor_input(UiInputKind, u32, bool) noexcept {
    return forward_runtime_test::theme_editor;
}
bool application_theme_editor_active() noexcept {
    return forward_runtime_test::theme_editor;
}
bool application_settings_input(UiInputKind, u32, bool) noexcept {
    return forward_runtime_test::settings;
}
bool application_settings_active() noexcept {
    return forward_runtime_test::settings;
}
bool application_media_picker_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_media_picker_active() noexcept {
    return false;
}
bool application_pinned_messages_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_pinned_messages_active() noexcept {
    return false;
}
bool application_thread_browser_consume_key_tail(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_thread_browser_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_search_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_search_edit_focused() noexcept {
    return false;
}
bool application_search_active() noexcept {
    return false;
}
bool application_friends_requests_active() noexcept {
    return false;
}
bool application_friends_active() noexcept {
    return false;
}
const RelationshipStore* application_friends_relationships() noexcept {
    return &forward_runtime_test::relationship_store;
}
bool application_friends_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_scoped_read_shortcut() noexcept {
    ++forward_runtime_test::scoped_shortcuts;
    return forward_runtime_test::scoped_shortcut_allowed;
}
bool application_account_menu_key(u32) noexcept {
    return false;
}
bool application_voice_message_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_login_handle_key(WPARAM, bool) noexcept {
    return true;
}
bool application_login_handle_character(WPARAM) noexcept {
    return true;
}
bool application_account_switch_login_escape() noexcept {
    return false;
}
bool handle_reaction_viewer_key(WPARAM) noexcept {
    return true;
}
bool handle_popup_key(WPARAM) noexcept {
    return false;
}
bool application_profile_key(u32, bool, bool) noexcept {
    return true;
}
bool application_profile_character(u32) noexcept {
    return true;
}
bool handle_shortcut_key(u32, bool) noexcept {
    ++forward_runtime_test::underlying_input;
    return false;
}
ShortcutAction application_shortcut_action(u32 key) noexcept {
    return shortcut_first({ShortcutFocus::content, false}, shortcut_chord_from_keyboard(key));
}
bool application_shortcuts_overlay_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_custom_keybind_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_message_navigation_cancel() noexcept {
    return false;
}
bool application_unread_banner_escape() noexcept {
    return false;
}
bool application_received_attachment_activate_focused() noexcept {
    return false;
}
bool handle_content_focus_key(u32) noexcept {
    ++forward_runtime_test::underlying_input;
    return false;
}
bool handle_composer_button_key(u32, bool, bool) noexcept {
    return false;
}
bool application_attachment_strip_key(u32) noexcept {
    return false;
}
bool handle_composer_character(WPARAM) noexcept {
    ++forward_runtime_test::underlying_input;
    return true;
}
bool handle_composer_key(WPARAM) noexcept {
    ++forward_runtime_test::underlying_input;
    return true;
}
bool application_message_action_bar_modifiers_changed() noexcept {
    return false;
}
bool handle_message_key(WPARAM) noexcept {
    ++forward_runtime_test::underlying_input;
    return true;
}
bool copy_message_text_selection() noexcept {
    return false;
}
float client_dip_x(LPARAM v) noexcept {
    return static_cast<i16>(LOWORD(v));
}
float client_dip_y(LPARAM v) noexcept {
    return static_cast<i16>(HIWORD(v));
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_message_forward_runtime_entry() noexcept {
    maicord::forward_runtime_test::run();
    ExitProcess(maicord::forward_runtime_test::failures ? 1 : 0);
}

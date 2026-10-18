#include "application_shortcuts.hpp"
#include "application_downloads_inactive.hpp"
#include "application_inbox_inactive.hpp"
#include "application_poll_inactive.hpp"
#include "application_member_moderation_inactive.hpp"
#include "application_guild_invite_inactive.hpp"
// Exercise the shipping modal host and production input router with a hidden
// native window and authoritative model stores. Navigation and TSF are observed
// at their host boundaries; no account traffic or renderer is created.
#include "application_quick_switcher.hpp"
#include "application_input_routing.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_state.hpp"
#include "application_profile.hpp"
#include "application_private_conversation.hpp"
#include "application_private_channel.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/ui/presence.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::quick_switcher_runtime_test {
using namespace app_detail;
u32 failures, underlying_input, suspends, synchronizations, private_requests, channel_routes, scoped_shortcuts;
u64 private_user, routed_channel;
u32 group_creates, group_adds;
u64 group_channel, group_people[9];
u32 group_count;
PrivateConversationOperation private_operation{};
bool private_allowed = true, settings, theme_editor;
bool scoped_shortcut_allowed = true;
bool pending_message_link;
u32 link_cancellations;
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
PrivateChannelStore private_store;
PrivateChannelRecord private_records[4];
u64 recipients[16];
char draft[256];
UiSettingsScene settings_scene_fixture;
TextFormScene form_scene_fixture;
TextFormSurface form_surface_fixture;

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
    check(SetKeyboardState(clean) != FALSE, "Quick runtime: controlled native modifiers");
}
bool channel(u64 id, Utf8View name, u8 type = 0, u64 guild = 10) noexcept {
    auto& state = g_app.account_ui->chat_state;
    ChannelInput input{};
    input.id = id;
    input.guild_id = guild;
    input.name = name;
    input.type = type;
    if (!chat_state_store_upsert_channel(&state, input).ok())
        return false;
    auto* c = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id));
    c->permissions_resolved = true;
    c->actions = channel_action_view | channel_action_read_history;
    ++state.permission_mutations;
    return true;
}
bool query(Utf8View value) noexcept {
    const auto target = application_quick_switcher_edit_target();
    u32 length = 0;
    u16 converted[128]{};
    if (value.bytes > 128 || text_input_length(target, &length) != TextInputResult::unchanged)
        return false;
    for (u32 i = 0; i < value.bytes; ++i) {
        if (static_cast<u8>(value.data[i]) >= 128)
            return false;
        converted[i] = static_cast<u8>(value.data[i]);
    }
    TextInputChange change{};
    const auto result = text_input_replace(target, 0, length, {converted, value.bytes}, &change);
    return (result == TextInputResult::changed || result == TextInputResult::unchanged) &&
           application_quick_switcher_edit_changed(true);
}
bool open_key() noexcept {
    modifiers(true);
    const bool opened = route_application_input(UiInputKind::key_down, 'K');
    route_application_input(UiInputKind::key_up, 'K');
    modifiers();
    return opened;
}
#include "application_scoped_shortcut_test.inc"
u32 checked_count() noexcept {
    const auto* scene = g_app.renderer.quick_switcher_scene;
    return scene && scene->content ? scene->content->checked.count : 0;
}
void picker() noexcept {
    const u64 strangers[] = {6, 7, 8, 9, 10, 11, 12, 13};
    check(channel(202, {}, 3, 0) &&
              private_channel_store_replace(&private_store, {202, 3, {strangers, 8}, {}, true, true, false}),
          "Picker: nine-member group fixture");
    const auto* scene = g_app.renderer.quick_switcher_scene;
    const bool opened = application_quick_switcher_open_picker(0) && application_quick_switcher_active() &&
                        (scene = g_app.renderer.quick_switcher_scene) != nullptr;
    check(opened && scene->content->picker && scene->accepted_count == 2 && !scene->content->can_confirm &&
              scene->accepted[0].user_id == 4 && scene->accepted[1].user_id == 5 &&
              same(scene->content->hint, utf8_literal("You can add 9 more friends.")),
          "Picker: new message lists every friend by name with nine places");
    if (!opened)
        return;
    key(VK_RETURN);
    check(checked_count() == 1 && scene->content->checked[0] == 4 && scene->content->can_confirm &&
              same(scene->content->action, utf8_literal("Create DM")) && application_quick_switcher_active(),
          "Picker: Enter toggles the highlighted friend and keeps the picker open");
    key(VK_DOWN);
    key(VK_RETURN);
    check(checked_count() == 2 && same(scene->content->action, utf8_literal("Create group DM")) &&
              same(scene->content->status, utf8_literal("2 selected")) &&
              same(scene->content->hint, utf8_literal("You can add 7 more friends.")),
          "Picker: a second friend turns the action into a group DM");
    key(VK_RETURN);
    check(checked_count() == 1 && same(scene->content->action, utf8_literal("Create DM")),
          "Picker: toggling again removes the friend");
    check(query(utf8_literal("bob")) && scene->accepted_count == 1, "Picker: search narrows friends");
    key(VK_RETURN);
    check(checked_count() == 2 && !application_quick_switcher_edit_model()->bytes && scene->accepted_count == 2,
          "Picker: choosing a searched friend clears the search");
    const u32 creates = group_creates;
    key(VK_RETURN, true);
    check(group_creates == creates + 1 && group_count == 2 && group_people[0] == 4 && group_people[1] == 5 &&
              !application_quick_switcher_active(),
          "Picker: Ctrl Enter creates the group from every chosen friend");
    const bool dm = application_quick_switcher_open_picker(201) && (scene = g_app.renderer.quick_switcher_scene);
    check(dm && scene->accepted_count == 2 && !scene->accepted[0].enabled &&
              same(scene->accepted[0].detail, utf8_literal("Already in the group")) && scene->accepted[1].enabled &&
              same(scene->content->hint, utf8_literal("You can add 8 more friends.")),
          "Picker: current DM members are shown but cannot be chosen");
    key(VK_RETURN);
    const u32 adds = group_adds;
    check(checked_count() == 1 && scene->content->checked[0] == 5 &&
              same(scene->content->action, utf8_literal("Create group DM")),
          "Picker: adding to a DM starts a group");
    key(VK_RETURN, true);
    check(group_adds == adds + 1 && group_channel == 201 && group_count == 1 && group_people[0] == 5,
          "Picker: confirmation adds the chosen friend to the conversation");
    const bool group = application_quick_switcher_open_picker(202) && (scene = g_app.renderer.quick_switcher_scene);
    check(group && same(scene->content->hint, utf8_literal("You can add 1 more friend.")), "Picker: one place left");
    key(VK_RETURN);
    key(VK_DOWN);
    key(VK_RETURN);
    check(checked_count() == 1 && scene->content->checked[0] == 4 &&
              same(scene->content->hint, utf8_literal("Groups are limited to 10 members.")) &&
              same(scene->content->action, utf8_literal("Add")),
          "Picker: selection stops at the group limit");
    application_quick_switcher_close();
    const u64 full[] = {6, 7, 8, 9, 10, 11, 12, 13, 14};
    check(private_channel_store_replace(&private_store, {202, 3, {full, 9}, {}, true, true, false}) &&
              application_quick_switcher_open_picker(202) && !application_quick_switcher_active(),
          "Picker: a full group does not open the picker");
}
void run() noexcept {
    BYTE saved[256]{};
    const bool saved_keyboard = GetKeyboardState(saved) != FALSE;
    modifiers();
    g_app.window = CreateWindowExW(0, L"STATIC", L"Quick isolated fixture", WS_POPUP, 0, 0, 1200, 900,
                                   nullptr, nullptr, nullptr, nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui, "Quick runtime: hidden host and scoped account fixture");
    if (!g_app.window || !g_app.account_ui)
        return;
    SetFocus(g_app.window);
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.renderer.width = 1200;
    g_app.renderer.height = 900;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 4;
    g_app.account.instance = &relationship_store;
    g_app.plugin_account_scope_serial = 7;
    auto& state = g_app.account_ui->chat_state;
    auto& people = g_app.account_ui->user_store;
    const bool ready =
        chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
                                       {channel_order, 8}, {names, sizeof(names)}) &&
        user_store_initialize(&people, {users, 4}, {user_order, 4},
                                      {user_names, sizeof(user_names)}) &&
        relationship_store_initialize(&relationship_store, {relationships, 4}) &&
        private_channel_store_initialize(&private_store, {private_records, 4}, {recipients, 16}) &&
        chat_state_store_upsert_guild(
            &state, {10, utf8_literal("Workbench"), GuildMutationKind::create, 3})
            .ok() &&
        channel(101, utf8_literal("general")) && channel(102, utf8_literal("engineering")) &&
        channel(103, utf8_literal("current")) && channel(201, {}, 1, 0) &&
        user_store_upsert(&people, 4, utf8_literal("Alice"), utf8_literal("alice")).ok() &&
        user_store_upsert(&people, 5, utf8_literal("Bob"), utf8_literal("bob")).ok() &&
        message_composer_initialize(&g_app.message_composer, {draft, sizeof(draft)}) &&
        message_composer_restore(&g_app.message_composer, 103, utf8_literal("Retained draft"), 14, 14);
    check(ready, "Quick runtime: authoritative guild channels friends and composer initialize");
    if (!ready)
        return;
    state.self_user_id = 3;
    state.private_channels = &private_store;
    state.selected_guild_id = 10;
    state.selected_channel_id = 103;
    g_app.message_composer.focused = true;
    RelationshipRecord alice{};
    alice.user_id = 4;
    alice.type = RelationshipType::friend_;
    RelationshipRecord bob = alice;
    bob.user_id = 5;
    const u64 alice_recipient[] = {4};
    check(relationship_store_upsert(&relationship_store, alice).ok() &&
              relationship_store_upsert(&relationship_store, bob).ok() &&
              private_channel_store_replace(&private_store,
                                            {201, 0, {alice_recipient, 1}, {}, true, false, false}),
          "Quick runtime: scoped private recipients and friend routing");
    check(!application_quick_switcher_active() && !application_quick_switcher_owner_bytes() &&
              !application_quick_switcher_edit_target(),
          "Quick runtime: unopened modal owns no heap or editor");
    application_quick_switcher_note_channel(101);
    check(!application_quick_switcher_owner_bytes() && g_app.quick_switcher_recent_count == 1,
          "Quick runtime: navigation recency does not allocate the modal");
    check(open_key() && application_quick_switcher_active() && g_app.renderer.quick_switcher_scene &&
              application_quick_switcher_owner_bytes() && application_quick_switcher_edit_focused() &&
              text_input_ready(application_quick_switcher_edit_target()) && suspends && synchronizations,
          "Quick runtime: Ctrl K opens the shipping scene and owned native document");
    if (!application_quick_switcher_active())
        return;
    modifiers(false, false, true);
    underlying_input = 0;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) &&
              !application_quick_switcher_active() && !scoped_shortcuts && !underlying_input &&
              route_application_input(UiInputKind::key_down, VK_ESCAPE, true) &&
              route_application_input(UiInputKind::key_up, VK_ESCAPE) && !scoped_shortcuts,
          "Scoped input: actual Quick switcher closes on Shift Escape without admitting read work");
    modifiers();
    check(open_key() && application_quick_switcher_active(),
          "Quick runtime: reopen after scoped-input priority check");
    if (!application_quick_switcher_active())
        return;
    const auto* scene = g_app.renderer.quick_switcher_scene;
    const auto* allocation = g_app.quick_switcher;
    const auto* widget_bank = scene->widgets.storage.data;
    const auto* accepted_bank = scene->accepted;
    const u32 owner_bytes = application_quick_switcher_owner_bytes();
    check(scene->accepted_count == 1 && scene->accepted[0].channel_id == 101,
          "Quick runtime: empty query uses successful same account navigation recency");
    underlying_input = 0;
    check(route_application_input(UiInputKind::character, 'x') &&
              same(message_composer_text(*application_quick_switcher_edit_model()), utf8_literal("x")) &&
              route_application_input(UiInputKind::key_down, VK_F6) &&
              route_application_input(UiInputKind::key_up, VK_F6) &&
              route_application_input(UiInputKind::system_character, 'q') && !underlying_input &&
              same(message_composer_text(g_app.message_composer), utf8_literal("Retained draft")),
          "Quick runtime: modal query and unhandled keys cannot edit underlying composer");
    check(route_application_input(UiInputKind::character, 0xD83D) &&
              route_application_input(UiInputKind::character, 0xDE80) &&
              same(message_composer_text(*application_quick_switcher_edit_model()),
                   utf8_literal("x\xF0\x9F\x9A\x80")) &&
              !underlying_input,
          "Quick runtime: supplementary text stays in the modal UTF8 document");
    const u16 newline[] = {10};
    TextInputChange denied{};
    check(text_input_replace(application_quick_switcher_edit_target(), 0, 0, {newline, 1}, &denied) ==
              TextInputResult::invalid_input,
          "Quick runtime: native document rejects multiline query replacement");
    check(query(utf8_literal("#general")) && scene->accepted_count == 1 &&
              scene->accepted[0].channel_id == 101,
          "Quick runtime: native replace republishes actual channel results");
    const auto stale_document = application_quick_switcher_edit_target();
    auto* general = const_cast<ChannelRecord*>(chat_state_store_find_channel(state, 101));
    general->actions = 0;
    ++state.permission_mutations;
    const u32 before_routes = channel_routes;
    check(route_application_input(UiInputKind::key_down, VK_RETURN) && channel_routes == before_routes,
          "Quick runtime: live permission retirement fences captured selected channel");
    route_application_input(UiInputKind::key_up, VK_RETURN);
    general->actions = channel_action_view | channel_action_read_history;
    ++state.permission_mutations;
    if (!application_quick_switcher_active())
        open_key();
    check(query(utf8_literal("#general")) && route_application_input(UiInputKind::key_down, VK_RETURN) &&
              routed_channel == 101 && channel_routes == before_routes + 1 &&
              !application_quick_switcher_active() && !text_input_ready(stale_document),
          "Quick runtime: accepted channel uses existing route and retires native document");
    underlying_input = 0;
    check(route_application_input(UiInputKind::character, VK_RETURN) &&
              route_application_input(UiInputKind::key_down, VK_RETURN, true) &&
              route_application_input(UiInputKind::key_up, VK_RETURN) && !underlying_input,
          "Quick runtime: activation Enter character repeat and release do not leak into composer");
    route_application_input(UiInputKind::character, 'z');
    check(underlying_input != 0, "Quick runtime: released tail restores ordinary input routing");
    // Alice is online on her phone only; Bob is idle on desktop.
    const bool presences =
        relationship_store_presence(&relationship_store, 4, RelationshipPresence::online).ok() &&
        relationship_store_presence(&relationship_store, 5, RelationshipPresence::idle).ok() &&
        user_store_set_client_status(&people, 4,
                                     client_status_with(0, ClientPlatform::mobile, ClientPlatformStatus::online)) &&
        user_store_set_client_status(&people, 5,
                                     client_status_with(0, ClientPlatform::desktop, ClientPlatformStatus::idle));
    const bool alice_query = presences && open_key() && query(utf8_literal("@alice"));
    check(alice_query && g_app.renderer.quick_switcher_scene && g_app.quick_switcher == allocation &&
              application_quick_switcher_owner_bytes() == owner_bytes &&
              g_app.renderer.quick_switcher_scene->widgets.storage.data == widget_bank &&
              g_app.renderer.quick_switcher_scene->accepted == accepted_bank,
          "Quick runtime: reopen reuses retained owner and widget result banks");
    const u32 alice_requests = private_requests;
    check(alice_query && g_app.renderer.quick_switcher_scene &&
              g_app.renderer.quick_switcher_scene->accepted_count == 1 &&
              g_app.renderer.quick_switcher_scene->accepted[0].kind ==
                  UiQuickSwitcherKind::private_channel &&
              g_app.renderer.quick_switcher_scene->accepted[0].channel_id == 201 &&
              g_app.renderer.quick_switcher_scene->accepted[0].presence == UiPresence::online &&
              g_app.renderer.quick_switcher_scene->accepted[0].mobile &&
              route_application_input(UiInputKind::key_down, VK_RETURN) && routed_channel == 201 &&
              private_requests == alice_requests,
          "Quick runtime: existing DM selection uses existing channel navigation and shows the phone status");
    // Retire the modal after a failed diagnostic so later independent route
    // assertions still evaluate the intended current document.
    if (application_quick_switcher_active())
        application_quick_switcher_close();
    route_application_input(UiInputKind::key_up, VK_RETURN);
    const u32 bob_requests = private_requests;
    check(open_key() && query(utf8_literal("@bob")) && g_app.renderer.quick_switcher_scene &&
              g_app.renderer.quick_switcher_scene->accepted_count == 1 &&
              g_app.renderer.quick_switcher_scene->accepted[0].kind == UiQuickSwitcherKind::user &&
              g_app.renderer.quick_switcher_scene->accepted[0].presence == UiPresence::idle &&
              !g_app.renderer.quick_switcher_scene->accepted[0].mobile &&
              route_application_input(UiInputKind::key_down, VK_RETURN) &&
              private_requests == bob_requests + 1 && private_user == 5 &&
              private_operation == PrivateConversationOperation::open,
          "Quick runtime: friend without DM uses scoped private conversation admission and shows the idle dot");
    route_application_input(UiInputKind::key_up, VK_RETURN);
    private_allowed = false;
    underlying_input = 0;
    const u32 refused_requests = private_requests;
    check(open_key() && query(utf8_literal("@bob")) &&
              route_application_input(UiInputKind::key_down, VK_RETURN) &&
              private_requests == refused_requests + 1 && !application_quick_switcher_active() &&
              !underlying_input,
          "Quick runtime: refused private admission consumes activation without underlying input");
    route_application_input(UiInputKind::key_up, VK_RETURN);
    private_allowed = true;
    picker();
    modifiers(true);
    MSG native_open{};
    native_open.hwnd = g_app.window;
    native_open.message = WM_KEYDOWN;
    native_open.wParam = 'K';
    const bool filtered_open = application_quick_switcher_filter(native_open);
    native_open.message = WM_KEYUP;
    application_quick_switcher_filter(native_open);
    modifiers();
    check(filtered_open && application_quick_switcher_active(),
          "Quick runtime: native TSF prefilter owns Ctrl K before document dispatch");
    const auto old_account_document = application_quick_switcher_edit_target();
    ++g_app.plugin_account_scope_serial;
    check(!text_input_ready(old_account_document) && application_quick_switcher_synchronize() &&
              !application_quick_switcher_active() && !g_app.renderer.quick_switcher_scene &&
              !g_app.quick_switcher_recent_count,
          "Quick runtime: account epoch retires editor scene and private MRU");
    g_app.login.visible = true;
    open_key();
    check(!application_quick_switcher_active(), "Quick runtime: login scope blocks Ctrl K allocation");
    g_app.login.visible = false;
    settings = true;
    settings_scene_fixture.active = true;
    g_app.renderer.settings_scene = &settings_scene_fixture;
    open_key();
    check(!application_quick_switcher_active(), "Quick runtime: settings modality owns Ctrl K");
    settings = false;
    settings_scene_fixture.active = false;
    g_app.renderer.settings_scene = nullptr;
    theme_editor = true;
    open_key();
    check(!application_quick_switcher_active(), "Quick runtime: theme editor modality owns Ctrl K");
    theme_editor = false;
    g_app.text_form = reinterpret_cast<ApplicationTextForm*>(&relationship_store);
    form_scene_fixture.active = true;
    form_surface_fixture.scene = &form_scene_fixture;
    g_app.renderer.text_form = &form_surface_fixture;
    open_key();
    check(!application_quick_switcher_active(), "Quick runtime: existing form owns Ctrl K");
    form_scene_fixture.active = false;
    open_key();
    check(application_quick_switcher_active(),
          "Quick runtime: inactive lazy form owner cannot block Ctrl K");
    application_quick_switcher_close();
    g_app.renderer.text_form = nullptr;
    g_app.text_form = nullptr;
    application_quick_switcher_release();
    check(!application_quick_switcher_owner_bytes() && !application_quick_switcher_edit_model() &&
              !application_quick_switcher_edit_target() && !g_app.renderer.quick_switcher_scene &&
              !application_quick_switcher_caret_wake() && !text_input_ready(old_account_document),
          "Quick runtime: release removes borrowed scene document and wake");
    pending_message_link = true;
    settings = true;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && pending_message_link &&
              !link_cancellations,
          "Input routing: foreground settings retains Escape before pending link cancellation");
    settings = false;
    g_app.message_edit.editor_active = true;
    route_application_input(UiInputKind::key_down, VK_ESCAPE);
    check(pending_message_link && !link_cancellations,
          "Input routing: active message editor retains its Escape semantics");
    g_app.message_edit.editor_active = false;
    underlying_input = 0;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE) && !pending_message_link &&
              link_cancellations == 1 && !underlying_input,
          "Input routing: unobscured link cancellation precedes ordinary pane focus");
    settings = true;
    check(route_application_input(UiInputKind::key_down, VK_ESCAPE, true) &&
              route_application_input(UiInputKind::key_up, VK_ESCAPE) && link_cancellations == 1 &&
              !underlying_input,
          "Input routing: cancellation owns repeated Escape and release after owner retirement");
    settings = false;
    scoped_shortcut_routing();
    message_composer_release(&g_app.message_composer);
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    if (saved_keyboard)
        check(SetKeyboardState(saved) != FALSE, "Quick runtime: keyboard state restores");
}
} // namespace maicord::quick_switcher_runtime_test

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
bool refresh_account_text(bool) noexcept {
    return true;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, 1200, 900};
}
void application_composer_text_input_suspend() noexcept {
    ++quick_switcher_runtime_test::suspends;
}
bool application_composer_text_input_synchronize() noexcept {
    ++quick_switcher_runtime_test::synchronizations;
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
bool application_private_channel_create(ConstSlice<u64> people) noexcept {
    ++quick_switcher_runtime_test::group_creates;
    quick_switcher_runtime_test::group_channel = 0;
    quick_switcher_runtime_test::group_count = people.count;
    copy_bytes(quick_switcher_runtime_test::group_people, people.data, people.count * sizeof(u64));
    return true;
}
bool application_private_channel_add(u64 channel_id, ConstSlice<u64> people) noexcept {
    ++quick_switcher_runtime_test::group_adds;
    quick_switcher_runtime_test::group_channel = channel_id;
    quick_switcher_runtime_test::group_count = people.count;
    copy_bytes(quick_switcher_runtime_test::group_people, people.data, people.count * sizeof(u64));
    return true;
}
bool queue_private_conversation(PrivateConversationOperation operation, u64 id) noexcept {
    ++quick_switcher_runtime_test::private_requests;
    quick_switcher_runtime_test::private_operation = operation;
    quick_switcher_runtime_test::private_user = id;
    return quick_switcher_runtime_test::private_allowed;
}
bool application_navigate_notification(u64 channel_id, u64) noexcept {
    ++quick_switcher_runtime_test::channel_routes;
    quick_switcher_runtime_test::routed_channel = channel_id;
    auto& state = g_app.account_ui->chat_state;
    const auto* c = chat_state_store_find_channel(state, channel_id);
    if (!c)
        return false;
    state.selected_channel_id = channel_id;
    state.selected_guild_id = c->guild_id;
    ++state.selection_revision;
    application_quick_switcher_note_channel(channel_id);
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
    return quick_switcher_runtime_test::theme_editor;
}
bool application_theme_editor_active() noexcept {
    return quick_switcher_runtime_test::theme_editor;
}
bool application_settings_input(UiInputKind, u32, bool) noexcept {
    return quick_switcher_runtime_test::settings;
}
bool application_settings_active() noexcept {
    return quick_switcher_runtime_test::settings;
}
bool application_media_picker_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_message_forward_active() noexcept {
    return false;
}
bool application_message_forward_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_media_picker_active() noexcept {
    return false;
}
bool application_pinned_messages_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_thread_browser_consume_key_tail(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_thread_browser_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_pinned_messages_active() noexcept {
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
    return &quick_switcher_runtime_test::relationship_store;
}
bool application_friends_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_scoped_read_shortcut() noexcept {
    ++quick_switcher_runtime_test::scoped_shortcuts;
    return quick_switcher_runtime_test::scoped_shortcut_allowed;
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
    ++quick_switcher_runtime_test::underlying_input;
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
bool application_unread_banner_escape() noexcept {
    return false;
}
bool application_message_navigation_cancel() noexcept {
    if (!quick_switcher_runtime_test::pending_message_link)
        return false;
    quick_switcher_runtime_test::pending_message_link = false;
    ++quick_switcher_runtime_test::link_cancellations;
    return true;
}
bool application_received_attachment_activate_focused() noexcept {
    return false;
}
bool handle_content_focus_key(u32) noexcept {
    ++quick_switcher_runtime_test::underlying_input;
    return false;
}
bool handle_composer_button_key(u32, bool, bool) noexcept {
    return false;
}
bool application_attachment_strip_key(u32) noexcept {
    return false;
}
bool handle_composer_character(WPARAM) noexcept {
    ++quick_switcher_runtime_test::underlying_input;
    return true;
}
bool handle_composer_key(WPARAM) noexcept {
    ++quick_switcher_runtime_test::underlying_input;
    return true;
}
bool application_message_action_bar_modifiers_changed() noexcept {
    return false;
}
bool handle_message_key(WPARAM) noexcept {
    ++quick_switcher_runtime_test::underlying_input;
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
bool application_voice_join(u64, u64) noexcept {
    return true;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_quick_switcher_runtime_entry() noexcept {
    maicord::quick_switcher_runtime_test::run();
    ExitProcess(maicord::quick_switcher_runtime_test::failures ? 1 : 0);
}

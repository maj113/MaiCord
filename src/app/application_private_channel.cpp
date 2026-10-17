#include "application_private_channel.hpp"
#include "application_private_conversation.hpp"
#include "application_quick_switcher.hpp"
#include "application_text_form.hpp"
#include "application_account_presentation.hpp"
#include "application_channel_session.hpp"
#include "application_composer.hpp"
#include "application_friends.hpp"
#include "application_profile.hpp"
#include "application_message_interaction.hpp"
#include "application_read_state.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "application_content_input.hpp"
#include "maicord/app/private_channel_actions.hpp"
#include "maicord/app/voice_socket.hpp"
#include "maicord/ui/private_channel_title.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
struct PrivateChannelOwner {
    u64 account, profile, self;
    PrivateChannelActionController controller;
    u64 form_serial;
    u64 ring_channel, ring_deadline_ms;
    bool form_silent;
    char form_context[256];
    PrivateCallPrompt prompt;
    bool prompt_shown;
    char prompt_title[128];
    bool requests_mode;
};
PrivateChannelOwner g_private{};

bool scope_current() noexcept {
    const u64 account = g_app.plugin_account_scope_serial, profile = g_app.account.status.profile_id;
    const u64 self = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
    if (g_private.account != account || g_private.profile != profile || g_private.self != self) {
        const u64 serial = g_private.form_serial;
        g_private = {};
        g_private.account = account;
        g_private.profile = profile;
        g_private.self = self;
        g_private.form_serial = serial;
    }
    return account && profile && self && g_app.account_ui && g_app.account.instance;
}
ChatStateStore& state() noexcept { return g_app.account_ui->chat_state; }
void wake() noexcept {
    if (g_app.window) PostMessageW(g_app.window, WM_NULL, 0, 0);
}
Utf8View title(u64 channel_id, Slice<char> storage) noexcept {
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    return channel ? private_channel_title(state(), &g_app.account_ui->user_store, *channel, storage) : Utf8View{};
}
bool navigation_changed() noexcept {
    application_friends_hide();
    application_read_state_activity();
    return synchronize_navigation_view() && synchronize_selected_channel() && synchronize_shell_layout() &&
           refresh_account_frame(true);
}

bool form_valid(TextFormOwnerKey key) noexcept {
    return scope_current() && key.scope == g_app.account.status.profile_id && key.collection == g_private.form_serial &&
           private_channel_is_group(state(), key.item);
}
bool rename_save(TextFormOwnerKey key, Utf8View name) noexcept {
    if (!form_valid(key) || !private_channel_begin_rename(&g_private.controller, state(), key.item, name)) return false;
    wake();
    return true;
}
bool leave_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key) ||
        !private_channel_begin_leave(&g_private.controller, state(), key.item, g_private.form_silent))
        return false;
    wake();
    return true;
}
TextFormOwnerKey next_form_key(u64 channel_id) noexcept {
    if (!++g_private.form_serial) ++g_private.form_serial;
    return {g_app.account.status.profile_id, g_private.form_serial, channel_id};
}
Utf8View error_text(const PrivateChannelActionController& c) noexcept {
    switch (c.request.kind) {
    case PrivateChannelActionKind::create: return utf8_literal("Couldn't create the group.");
    case PrivateChannelActionKind::add_recipient:
        return c.status_code == 403 ? utf8_literal("Couldn't add this person. They may not accept group invites.")
                                    : utf8_literal("Couldn't add people to the group.");
    case PrivateChannelActionKind::remove_recipient: return utf8_literal("Couldn't remove this member.");
    case PrivateChannelActionKind::rename: return utf8_literal("Couldn't rename the group.");
    case PrivateChannelActionKind::leave: return utf8_literal("Couldn't leave the group.");
    case PrivateChannelActionKind::accept_request: return utf8_literal("Couldn't accept the message request.");
    case PrivateChannelActionKind::ignore_request:
    case PrivateChannelActionKind::ignore_requests: return utf8_literal("Couldn't ignore the message request.");
    case PrivateChannelActionKind::acknowledge_safety: return utf8_literal("Couldn't dismiss the warning.");
    default: return utf8_literal("Something went wrong. Try again.");
    }
}
} // namespace

bool application_private_channel_pending(u64 now_ms) noexcept {
    if (!scope_current()) return false;
    // Ring once the voice connection is up, when the call exists server-side.
    if (g_private.ring_channel) {
        const auto& session = g_app.account_ui->voice_session;
        if (now_ms >= g_private.ring_deadline_ms)
            g_private.ring_channel = 0;
        else if (session.channel_id == g_private.ring_channel && session.socket_phase == VoiceSocketPhase::open &&
                 private_channel_begin_ring(&g_private.controller, state(), g_private.ring_channel, false))
            g_private.ring_channel = 0;
    }
    return private_channel_action_due(g_private.controller, now_ms);
}
bool application_private_channel_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_private_channel_pending(now_ms)) return false;
    auto& c = g_private.controller;
    // Membership may have changed while the request waited for the lane.
    const auto kind = c.request.kind;
    if (kind != PrivateChannelActionKind::create && kind != PrivateChannelActionKind::ignore_requests &&
        !private_channel_member_count(state(), c.request.channel_id)) {
        c = {};
        return false;
    }
    const auto submitted = g_app.account.api.private_channel(g_app.account.instance, &c.request, request_id);
    if (!submitted.ok()) {
        if (submitted.error == RestClientSubmitError::busy) {
            c.phase = PrivateChannelActionPhase::retry_wait;
            c.retry_at_ms = now_ms + 50;
        } else {
            c.phase = PrivateChannelActionPhase::failed;
            c.error = PrivateChannelActionError::transport;
            c.feedback_shown = false;
        }
        return false;
    }
    return private_channel_action_submitted(&c, request_id);
}
bool application_private_channel_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!scope_current() || response.request_id != g_private.controller.request_id) return true;
    auto& ui = *g_app.account_ui;
    const auto kind = g_private.controller.request.kind;
    const u64 selected = ui.chat_state.selected_channel_id;
    if (!private_channel_action_accept(&g_private.controller, &ui.chat_state, &ui.user_store, response, body,
                                       {ui.message_history_object_structurals, kMessageHistoryObjectStructuralCount},
                                       {reinterpret_cast<char*>(ui.event_payload_scratch),
                                        kGatewayClientDispatchPayloadCapacity},
                                       {ui.guild_id_scratch, kChatStateGuildCapacity}))
        return true;
    if (g_private.controller.phase == PrivateChannelActionPhase::failed) {
        log_u64(LogLevel::warning, "private", "group_action_http_status", g_private.controller.status_code);
        // Ringing is best effort: the call itself is already joined or declined.
        if (kind == PrivateChannelActionKind::ring || kind == PrivateChannelActionKind::stop_ringing) {
            g_private.controller = {};
            return true;
        }
        return application_private_channel_present_error();
    }
    if (g_private.controller.phase == PrivateChannelActionPhase::queued) {
        wake();
        return true;
    }
    if (kind == PrivateChannelActionKind::create || kind == PrivateChannelActionKind::leave ||
        kind == PrivateChannelActionKind::ignore_request || kind == PrivateChannelActionKind::ignore_requests ||
        kind == PrivateChannelActionKind::accept_request || selected != ui.chat_state.selected_channel_id) {
        // The last request handled returns the list to conversations.
        if (g_private.requests_mode && !application_private_channel_request_count())
            g_private.requests_mode = false;
        return navigation_changed();
    }
    return refresh_account_frame(true);
}

bool application_private_channel_create(ConstSlice<u64> users) noexcept {
    if (!scope_current() || !users.count) return false;
    if (users.count == 1) return queue_private_conversation(PrivateConversationOperation::open, users[0]);
    if (!private_channel_begin_create(&g_private.controller, state(), users)) return false;
    wake();
    return true;
}
bool application_private_channel_add(u64 channel_id, ConstSlice<u64> users) noexcept {
    if (!scope_current() || !private_channel_begin_add(&g_private.controller, state(), channel_id, users)) return false;
    wake();
    return true;
}
bool application_private_channel_call(u64 channel_id) noexcept {
    if (!scope_current() || !private_channel_member_count(state(), channel_id)) return false;
    const auto& session = g_app.account_ui->voice_session;
    if (session.channel_id == channel_id && session.phase != VoiceSetupPhase::idle) return true;
    if (!application_voice_join(0, channel_id)) return false;
    g_private.ring_channel = channel_id;
    g_private.ring_deadline_ms = clock_monotonic_milliseconds() + 15'000;
    return true;
}
bool application_private_channel_title_hit(float x, float y) noexcept {
    if (!g_app.account_ui || g_app.shell_scene.view != UiShellView::private_channels ||
        !private_channel_is_group(state(), state().selected_channel_id))
        return false;
    const auto* label = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::title_label));
    return label && ui_rect_contains(label->rectangle, x, y);
}
bool application_private_channel_picker(u64 channel_id) noexcept {
    if (!scope_current()) return false;
    if (channel_id && !private_channel_capacity_left(state(), channel_id)) return true;
    return application_quick_switcher_open_picker(channel_id);
}
bool application_private_channel_rename(u64 channel_id) noexcept {
    if (!scope_current() || !private_channel_is_group(state(), channel_id)) return false;
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Rename group");
    request.label = utf8_literal("Group name");
    request.accept = utf8_literal("Save");
    request.initial = chat_state_store_channel_name(state(), *channel);
    request.owner = next_form_key(channel_id);
    request.valid = form_valid;
    request.save = rename_save;
    request.maximum_codepoints = kPrivateChannelNameMaximumCodepoints;
    request.maximum_bytes = kPrivateChannelNameMaximumBytes;
    request.single_line = true;
    return application_text_form_open(request);
}
bool application_private_channel_leave(u64 channel_id, bool silent) noexcept {
    if (!scope_current() || !private_channel_is_group(state(), channel_id)) return false;
    char name[256];
    const auto group = title(channel_id, {name, sizeof(name)});
    ProtocolWriter context{{reinterpret_cast<u8*>(g_private.form_context), sizeof(g_private.form_context)}, 0};
    // A long generated name may not fit; the sentence still reads without it.
    if (!protocol_writer_append_c_string(&context, "Leave ") ||
        !protocol_writer_append_bytes(&context, reinterpret_cast<const u8*>(group.data), group.bytes) ||
        !protocol_writer_append_c_string(&context, "? You won't be able to rejoin unless someone adds you again.")) {
        context.used = 0;
        protocol_writer_append_c_string(&context, "You won't be able to rejoin unless someone adds you again.");
    }
    if (silent) protocol_writer_append_c_string(&context, " Other members won't be notified.");
    g_private.form_silent = silent;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Leave group");
    request.context = {g_private.form_context, context.used};
    request.accept = utf8_literal("Leave group");
    request.owner = next_form_key(channel_id);
    request.valid = form_valid;
    request.save = leave_save;
    request.confirmation_only = true;
    request.danger = true;
    return application_text_form_open(request);
}

u32 application_private_channel_menu_items(u64 channel_id, Slice<PopupItem> output) noexcept {
    if (!scope_current() || !output.data || !private_channel_member_count(state(), channel_id)) return 0;
    // A message request offers its two answers instead of group actions.
    const auto* record = state().private_channels ? private_channel_store_find(*state().private_channels, channel_id)
                                                  : nullptr;
    if (record && private_channel_is_request(*record)) {
        if (output.count < 2) return 0;
        output[0] = {utf8_literal("Accept request"), popup_action_private_accept_request, popup_item_enabled, 0};
        output[0].presentation = ui_menu_row_presentation(UiIcon::check);
        output[1] = {utf8_literal("Ignore request"), popup_action_private_ignore_request,
                     static_cast<u16>(popup_item_enabled | popup_item_danger), 0};
        output[1].presentation = ui_menu_row_presentation(UiIcon::close);
        return 2;
    }
    const bool group = private_channel_is_group(state(), channel_id);
    const bool add = private_channel_capacity_left(state(), channel_id) != 0;
    const u32 count = static_cast<u32>(add) + (group ? 2u : 0u);
    if (!count || output.count < count) return 0;
    u32 used = 0;
    if (add)
        output[used++] = {utf8_literal("Add people"), popup_action_private_add_people, popup_item_enabled, 0};
    if (group) {
        auto& rename = output[used++];
        rename = {utf8_literal("Rename group"), popup_action_private_rename, popup_item_enabled, 0};
        rename.presentation = ui_menu_row_presentation(UiIcon::edit);
        auto& leave = output[used++];
        leave = {utf8_literal("Leave group"), popup_action_private_leave,
                 static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before), 0};
        leave.presentation = ui_menu_row_presentation(UiIcon::log_out);
    }
    return used;
}
u32 application_private_channel_member_items(u64 user_id, Slice<PopupItem> output) noexcept {
    if (!scope_current() || !output.count || !output.data ||
        !private_channel_can_remove(state(), state().selected_channel_id, user_id))
        return 0;
    output[0] = {utf8_literal("Remove from group"), popup_action_private_remove_member,
                 static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before), 0};
    return 1;
}
bool application_private_channel_menu_action(u32 action, u64 owner_id, bool shift, bool* applied) noexcept {
    const auto* channel = g_app.account_ui ? chat_state_store_find_channel(state(), owner_id) : nullptr;
    const bool channel_owner = channel && channel_is_private(channel->type) &&
                               (g_app.popup_owner_kind == PopupOwnerKind::private_channel ||
                                g_app.popup_owner_kind == PopupOwnerKind::channel);
    if ((action == popup_action_private_call_join || action == popup_action_private_call_decline) &&
        g_app.popup_owner_kind == PopupOwnerKind::private_notice) {
        const u64 call = scope_current() ? g_private.prompt.channel_id : 0;
        g_private.prompt = {};
        if (!call || call != owner_id) {
            *applied = false;
            return true;
        }
        if (action == popup_action_private_call_decline) {
            *applied = private_channel_begin_ring(&g_private.controller, state(), call, true);
            if (*applied) wake();
        } else
            *applied = application_voice_join(0, call) && application_navigate_notification(call, 0);
        return true;
    }
    if (action == popup_action_private_notice_dismiss && g_app.popup_owner_kind == PopupOwnerKind::private_notice) {
        if (scope_current() && g_private.controller.phase == PrivateChannelActionPhase::failed)
            g_private.controller = {};
        *applied = true;
        return true;
    }
    if (action == popup_action_private_remove_member && g_app.popup_owner_kind == PopupOwnerKind::member) {
        *applied = scope_current() && private_channel_begin_remove(&g_private.controller, state(),
                                                                   state().selected_channel_id, owner_id);
        if (*applied) wake();
        return true;
    }
    if (channel_owner &&
        (action == popup_action_private_accept_request || action == popup_action_private_ignore_request)) {
        *applied = action == popup_action_private_accept_request ? application_private_channel_accept_request(owner_id)
                                                                 : application_private_channel_ignore_request(owner_id);
        return true;
    }
    if (!channel_owner || (action != popup_action_private_add_people && action != popup_action_private_rename &&
                           action != popup_action_private_leave))
        return false;
    // Forms and the picker replace the menu.
    if (!dismiss_popup()) {
        *applied = false;
        return true;
    }
    *applied = action == popup_action_private_add_people ? application_private_channel_picker(owner_id)
               : action == popup_action_private_rename   ? application_private_channel_rename(owner_id)
                                                         : application_private_channel_leave(owner_id, shift);
    return true;
}
const UiPrivateRowSource* application_private_channel_rows() noexcept {
    static UiPrivateRowSource source{};
    if (!g_app.account_ui) return nullptr;
    source = {&g_app.account_ui->chat_state, &g_app.account_ui->user_store, application_friends_relationships(),
              &g_app.account_ui->typing_store};
    return &source;
}
bool application_private_channel_row_close(u64 channel_id, float x, float y) noexcept {
    if (!scope_current()) return false;
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    if (!channel || !channel_is_private(channel->type)) return false;
    const auto& scene = g_app.shell_navigation_scene;
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        if (row.view.id != channel_id || row.view.mention_count) continue;
        const auto* layout = shell_navigation_scene_layout_node(scene, row);
        auto target = layout ? ui_private_row_close_rectangle(layout->rectangle) : UiRect{};
        target = {target.left - 4, target.top - 4, target.right + 4, target.bottom + 4};
        if (!ui_rect_contains(target, x, y)) return false;
        if (channel->type == 1)
            queue_private_conversation(PrivateConversationOperation::close, channel_id);
        else
            application_private_channel_leave(channel_id, false);
        return true;
    }
    return false;
}
bool application_private_channel_accept_request(u64 channel_id) noexcept {
    if (!scope_current() || !private_channel_begin_accept_request(&g_private.controller, state(), channel_id))
        return false;
    wake();
    return true;
}
bool application_private_channel_ignore_request(u64 channel_id) noexcept {
    if (!scope_current() || !private_channel_begin_ignore_request(&g_private.controller, state(), channel_id))
        return false;
    wake();
    return true;
}
bool application_private_channel_dismiss_safety(u64 channel_id) noexcept {
    if (!scope_current() || !private_channel_begin_dismiss_safety(&g_private.controller, state(), channel_id))
        return false;
    wake();
    return true;
}
void application_private_channel_navigation(NavigationView* view) noexcept {
    if (!view || !g_app.account_ui)
        return;
    static u32 published_store_revision;
    const auto* store = state().private_channels;
    const u32 count = application_private_channel_request_count();
    const bool mode = scope_current() && g_private.requests_mode && count;
    if (scope_current() && g_private.requests_mode && !count)
        g_private.requests_mode = false;
    const u32 revision = store ? store->revision : 0;
    if (view->requests_row != (count != 0) || view->requests_mode != mode || view->request_count != count ||
        published_store_revision != revision) {
        view->requests_row = count != 0;
        view->requests_mode = mode;
        view->request_count = count;
        published_store_revision = revision;
        ++view->requests_revision;
    }
}
bool application_private_channel_requests_mode() noexcept {
    return scope_current() && g_private.requests_mode;
}
void application_private_channel_set_requests_mode(bool requests) noexcept {
    if (!scope_current() || g_private.requests_mode == requests)
        return;
    g_private.requests_mode = requests;
    navigation_changed();
}
u32 application_private_channel_request_count() noexcept {
    if (!g_app.account_ui || !state().private_channels)
        return 0;
    return private_channel_store_request_count(*state().private_channels, false) +
           private_channel_store_request_count(*state().private_channels, true);
}
bool application_private_channel_request_busy(u64 channel_id) noexcept {
    if (!scope_current())
        return false;
    const auto& c = g_private.controller;
    const auto kind = c.request.kind;
    const bool request = kind == PrivateChannelActionKind::accept_request ||
                         kind == PrivateChannelActionKind::ignore_request ||
                         kind == PrivateChannelActionKind::acknowledge_safety;
    return request && c.request.channel_id == channel_id &&
           (c.phase == PrivateChannelActionPhase::queued || c.phase == PrivateChannelActionPhase::awaiting ||
            c.phase == PrivateChannelActionPhase::retry_wait);
}
void application_private_channel_gateway(const GatewayClientEvent& event) noexcept {
    if (!scope_current()) return;
    const u64 shown = g_private.prompt_shown ? g_private.prompt.channel_id : 0;
    if (!private_call_observe(&g_private.prompt, state(), event, g_app.account_ui->voice_session.channel_id)) return;
    // A prompt for a call that ended or was answered elsewhere closes itself.
    if (shown && g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::private_notice &&
        g_app.popup_scene.owner_id == shown)
        dismiss_popup();
    g_private.prompt_shown = false;
    application_request_frame();
}
bool application_private_channel_present_error() noexcept {
    if (!scope_current() || g_app.popup_scene.active || g_app.text_form ||
        (g_app.profile && g_app.profile->scene.active))
        return true;
    if (g_private.prompt.channel_id && !g_private.prompt_shown) {
        const u64 channel = g_private.prompt.channel_id;
        PopupItem items[] = {{title(channel, {g_private.prompt_title, sizeof(g_private.prompt_title)}), 0, 0, 0},
                             {utf8_literal("Join call"), popup_action_private_call_join, popup_item_enabled, 0},
                             {utf8_literal("Decline"), popup_action_private_call_decline,
                              static_cast<u16>(popup_item_enabled | popup_item_danger), 0}};
        // Menu rows carry only the first 62 icons; the call glyphs sit past that bank.
        items[0].detail = utf8_literal("Incoming call");
        const auto anchor = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar);
        if (!popup_scene_open(&g_app.popup_scene, PopupKind::popover, channel, {items, 3}, anchor,
                              application_viewport_dips(), 300.0f)
                 .ok())
            return false;
        set_popup_owner(PopupOwnerKind::private_notice);
        g_private.prompt_shown = true;
        return refresh_account_frame(true);
    }
    auto& c = g_private.controller;
    if (c.phase != PrivateChannelActionPhase::failed || c.feedback_shown) return true;
    const PopupItem items[] = {{error_text(c), 0, 0, 0},
                               {utf8_literal("Dismiss"), popup_action_private_notice_dismiss, popup_item_enabled, 0}};
    const auto anchor = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar);
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::popover, c.request.channel_id, {items, 2}, anchor,
                          application_viewport_dips(), 300.0f)
             .ok())
        return false;
    set_popup_owner(PopupOwnerKind::private_notice);
    c.feedback_shown = true;
    return refresh_account_frame(true);
}
} // namespace maicord::app_detail

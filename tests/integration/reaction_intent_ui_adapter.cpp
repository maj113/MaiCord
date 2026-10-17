#include "reaction_intent_ui_adapter.hpp"
#include "application_state.hpp"
#include "application_message_interaction.hpp"
#include "application_message_mutation_notice.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/model/reaction_preferences.hpp"

namespace maicord {
namespace {
using namespace app_detail;
struct Adapter {
    AccountUiRuntime* prior_ui;
    AccountHost prior_account;
    MessageViewRuntime prior_view;
    ApplicationMessageMutationNotice prior_notice;
    MessageHistoryRequestOwner prior_owner;
    HWND prior_window;
    u64 prior_scope, prior_request;
    AccountUiRuntime* ui;
    MessageRecord records[4];
    u32 order[4];
    u8 text[4096];
    MessageRichRecord rich[4];
    u16 rich_order[4];
    u8 rich_text[4096];
    CustomEmojiRecord emojis[2];
    u64 emoji_roles[1];
    u64 flight_id, flight_message, flight_emoji;
    bool flight_add;
    bool prior_login_visible;
    bool (*refresh)(void*) noexcept;
    void* context;
};
Adapter* adapter;

RestClientSubmitResult WINAPI submit(void*, u64, u64 message, u64 emoji, Utf8View,
                                     MessageReactionType, bool add, u64 request_id) noexcept {
    if (!adapter || adapter->flight_id)
        return {RestClientSubmitError::busy};
    adapter->flight_id = request_id;
    adapter->flight_message = message;
    adapter->flight_emoji = emoji;
    adapter->flight_add = add;
    return {};
}
} // namespace

bool reaction_intent_ui_adapter_begin(HWND window, bool (*native_refresh)(void*) noexcept,
                                      void* context) noexcept {
    if (adapter || application_message_reaction_observation_needed())
        return false;
    auto* next = static_cast<Adapter*>(
        VirtualAlloc(nullptr, sizeof(Adapter), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!next)
        return false;
    next->ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!next->ui) {
        VirtualFree(next, 0, MEM_RELEASE);
        return false;
    }
    next->prior_ui = g_app.account_ui;
    next->prior_account = g_app.account;
    next->prior_view = g_app.message_view;
    next->prior_notice = g_app.message_mutation_notice;
    next->prior_window = g_app.window;
    next->prior_scope = g_app.plugin_account_scope_serial;
    next->prior_owner = g_app.message_history_request_owner;
    next->prior_request = g_app.primary_request_id;
    next->prior_login_visible = g_app.login.visible;
    next->refresh = native_refresh;
    next->context = context;
    adapter = next;
    g_app.account_ui = next->ui;
    g_app.account = {};
    g_app.account.instance = next;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 20;
    g_app.account.api.message_reaction = submit;
    g_app.login.visible = false;
    g_app.window = window;
    g_app.plugin_account_scope_serial = 10;
    g_app.message_view = {};
    g_app.message_mutation_notice = {};
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.primary_request_id = 0;
    auto& ui = *next->ui;
    const bool initialized =
        chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                       {ui.guild_order_storage, 2}, {ui.channel_record_storage, 4},
                                       {ui.channel_order_storage, 4}, {ui.chat_state_name_storage, 4096});
    ui.chat_state.self_user_id = 5;
    const bool models_initialized =
        initialized && reaction_preferences_initialize(&ui.reaction_preferences) &&
        chat_state_store_upsert_guild(&ui.chat_state, {60, utf8_literal("Native reactions")}).ok() &&
        chat_state_store_upsert_channel(&ui.chat_state,
                                           {70, 0, 0, utf8_literal("Reaction DM"), 0, 0, 1})
            .ok() &&
        chat_state_store_bind_emojis_sparse(&ui.chat_state, {next->emojis, 2}, {next->emoji_roles, 1}) &&
        message_store_initialize(&g_app.message_view.store, {next->records, 4}, {next->order, 4},
                                 {next->text, sizeof(next->text)}) &&
        message_rich_store_initialize(&ui.message_rich_store, {next->rich, 4}, {next->rich_order, 4},
                                      {next->rich_text, sizeof(next->rich_text)});
    const bool selected =
        models_initialized && chat_state_store_select_channel(&ui.chat_state, 70).ok();
    if (selected)
        ui.active_channel_id = ui.chat_state.selected_channel_id;
    const CustomEmojiInput emojis[]{{101, utf8_literal("native_orbit"), {}, 0, custom_emoji_available},
                                     {102, utf8_literal("native_wave"), {}, 0, custom_emoji_available}};
    GatewayMessageEvent message{};
    message.id = 900;
    message.channel_id = 70;
    message.author_id = 5;
    message.timestamp_ms = 1000;
    message.content = utf8_literal("Retained reactions stay with your latest choice.");
    message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                     gateway_message_field_author_id | gateway_message_field_timestamp |
                     gateway_message_field_content | gateway_message_field_reactions;
    const GatewayMessageReaction orbit{101, utf8_literal("native_orbit"), 2, 2, 0, false, false, false};
    message.reactions = {&orbit, 1};
    const bool complete =
        selected && chat_state_store_replace_guild_emojis(&ui.chat_state, 60, {emojis, 2}) &&
        user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
                                      {ui.user_order_storage, 4}, {ui.user_name_storage, 4096}) &&
        user_store_upsert(&ui.user_store, 5, utf8_literal("Native reviewer")).ok() &&
        message_store_upsert(&g_app.message_view.store, {message.id, message.channel_id, message.author_id,
                                                         message.timestamp_ms, 0, message.content, 0})
            .ok();
    const auto found = message_store_find(g_app.message_view.store, 900);
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!complete || !record ||
        !message_rich_store_apply(&ui.message_rich_store, 900, record->revision, message).ok()) {
        reaction_intent_ui_adapter_end();
        return false;
    }
    return true;
}

void reaction_intent_ui_adapter_end() noexcept {
    if (!adapter)
        return;
    auto* prior = adapter;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.primary_request_id = 0;
    reset_message_reaction();
    reaction_preferences_release(&prior->ui->reaction_preferences);
    g_app.account_ui = prior->prior_ui;
    g_app.account = prior->prior_account;
    g_app.message_view = prior->prior_view;
    g_app.message_mutation_notice = prior->prior_notice;
    g_app.window = prior->prior_window;
    g_app.plugin_account_scope_serial = prior->prior_scope;
    g_app.message_history_request_owner = prior->prior_owner;
    g_app.primary_request_id = prior->prior_request;
    g_app.login.visible = prior->prior_login_visible;
    adapter = nullptr;
    VirtualFree(prior->ui, 0, MEM_RELEASE);
    VirtualFree(prior, 0, MEM_RELEASE);
}
bool reaction_intent_ui_adapter_active() noexcept {
    return adapter != nullptr;
}
bool reaction_intent_ui_adapter_refresh() noexcept {
    return !adapter || !adapter->refresh || adapter->refresh(adapter->context);
}
MessageStore* reaction_intent_ui_adapter_store() noexcept {
    return adapter ? &g_app.message_view.store : nullptr;
}
MessageRichStore* reaction_intent_ui_adapter_rich_store() noexcept {
    return adapter ? &adapter->ui->message_rich_store : nullptr;
}
ChatStateStore* reaction_intent_ui_adapter_state() noexcept {
    return adapter ? &adapter->ui->chat_state : nullptr;
}
UserStore* reaction_intent_ui_adapter_users() noexcept {
    return adapter ? &adapter->ui->user_store : nullptr;
}
ApplicationMessageReactionEnqueueResult reaction_intent_ui_adapter_add(u64 message, u64 emoji) noexcept {
    const auto* record = adapter ? chat_state_store_find_emoji(adapter->ui->chat_state, emoji) : nullptr;
    return record ? application_queue_message_reaction_emoji(message, emoji, chat_state_store_emoji_name(*record),
                                                             false)
                  : ApplicationMessageReactionEnqueueResult::invalid;
}
ApplicationMessageReactionEnqueueResult reaction_intent_ui_adapter_toggle(u64 message, u32 index) noexcept {
    return application_queue_message_reaction(message, index);
}
bool reaction_intent_ui_adapter_submit(u64 id) noexcept {
    if (!adapter || !application_message_reaction_submit(id, clock_monotonic_milliseconds()))
        return false;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::message_reaction;
    g_app.primary_request_id = id;
    return true;
}
bool reaction_intent_ui_adapter_response(u16 status, RestClientError error) noexcept {
    if (!adapter || !adapter->flight_id)
        return false;
    RestClientResponse response{};
    response.request_id = adapter->flight_id;
    response.status_code = status;
    response.error = error;
    adapter->flight_id = 0;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.primary_request_id = 0;
    return application_message_reaction_response(response);
}
void reaction_intent_ui_adapter_observe(const GatewayMessageReactionEvent& event) noexcept {
    if (!adapter)
        return;
    const auto found = message_store_find(g_app.message_view.store, event.message_id);
    const auto* message =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    bool applied = false;
    if (message && message->channel_id == event.channel_id) {
        applied = message_rich_store_mutate_reaction(
                      &adapter->ui->message_rich_store, event.message_id, message->revision,
                      {event.emoji_id, event.name, event.type,
                       static_cast<MessageRichReactionMutationKind>(event.kind),
                       event.user_id == adapter->ui->chat_state.self_user_id, event.animated})
                      .ok();
    }
    application_message_reaction_observe(event, found.handle, message ? message->revision : 0, applied);
}
bool reaction_intent_ui_adapter_synchronize() noexcept {
    return application_message_reaction_synchronize();
}
ApplicationMessageReactionMetrics reaction_intent_ui_adapter_metrics() noexcept {
    return application_message_reaction_metrics();
}
bool reaction_intent_ui_adapter_flight(u64* id, u64* message, u64* emoji, bool* add) noexcept {
    if (!adapter || !adapter->flight_id)
        return false;
    if (id)
        *id = adapter->flight_id;
    if (message)
        *message = adapter->flight_message;
    if (emoji)
        *emoji = adapter->flight_emoji;
    if (add)
        *add = adapter->flight_add;
    return true;
}

namespace app_detail {
void application_emoji_preferences_track_reaction(ReactionEmojiRef) noexcept {
}
// UiProbe links the shipping reaction owner, not the media picker controller;
// its status hook only refreshes the native projection here.
void application_media_picker_reaction_changed() noexcept {
    reaction_intent_ui_adapter_refresh();
}
} // namespace app_detail
} // namespace maicord

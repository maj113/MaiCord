// The application's side of the plugin message pipeline: gateway changes to hooks and the event feed,
// clicks, typing, the actions a plugin may take, and kept messages on screen.
#include "pipeline_host.hpp"
#include "ui_host.hpp"
#include "plugins.hpp"
#include "surface_host.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "maicord/protocol/user/relationships.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../shell/state.hpp"
#include "../shell/input.hpp"
#include "../shell/overlay_input.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../account/activity.hpp"
#include "../account/custom_status.hpp"
#include "../account/friends.hpp"
#include "../chat/channel_authority.hpp"
#include "../chat/read_state.hpp"
#include "../chat/scoped_read.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/reply_draft.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/messages/delete.hpp"
#include "../chat/messages/edit.hpp"
#include "../chat/messages/history.hpp"
#include "../chat/messages/reaction.hpp"
#include "../chat/messages/send.hpp"
#include "../chat/threads/actions.hpp"
#include "maicord/app/chat/composer/message_composer_controller.hpp"
#include "maicord/app/chat/messages/send_response.hpp"
#include "maicord/app/chat/messages/timeline.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/plugins/keep.hpp"
#include "maicord/app/plugins/pipeline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/user/relationship_store.hpp"
#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/protocol/gateway/reaction_events.hpp"
#include "maicord/protocol/gateway/typing_events.hpp"
#include "maicord/protocol/user/self_status_settings.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {

const AccountUiRuntime* ui_state() noexcept {
    return g_app.account_ui;
}
u64 self_id() noexcept {
    return g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
}
bool account_ready() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.login.visible &&
           g_app.account.status.required_action == AccountRequirement::none;
}
u64 guild_of_channel(const AccountUiRuntime& ui, u64 channel_id) noexcept {
    const auto* channel = channel_id ? chat_state_store_find_channel(ui.chat_state, channel_id) : nullptr;
    return channel ? channel->guild_id : 0;
}
// The server nickname when the member is resident, else the user's display name.
Utf8View display_name(const AccountUiRuntime& ui, u64 guild, u64 user) noexcept {
    if (const auto* member = guild && user ? guild_member_store_find(ui.guild_member_store, guild, user) : nullptr) {
        const auto nick = guild_member_store_nickname(*member);
        if (nick.bytes) return nick;
    }
    const auto* record = user ? user_store_find(ui.user_store, user) : nullptr;
    return record ? user_store_name(ui.user_store, *record) : Utf8View{};
}
bool user_is_bot(const AccountUiRuntime& ui, u64 user) noexcept {
    const auto* appearance = user ? user_appearance_find(ui.user_store.appearance, user) : nullptr;
    return appearance && (appearance->fields & appearance_bot) && appearance->bot;
}
u32 relationship_value(u64 user) noexcept {
    const auto* store = application_friends_relationships();
    const auto* record = store && user ? relationship_store_find(*store, user) : nullptr;
    if (!record) return MC_RELATIONSHIP_NONE;
    switch (record->type) {
    case RelationshipType::friend_: return MC_RELATIONSHIP_FRIEND;
    case RelationshipType::blocked: return MC_RELATIONSHIP_BLOCKED;
    case RelationshipType::incoming: return MC_RELATIONSHIP_INCOMING;
    case RelationshipType::outgoing: return MC_RELATIONSHIP_OUTGOING;
    default: return MC_RELATIONSHIP_NONE;
    }
}
u32 presence_value(GatewayClientMemberListStatus status) noexcept {
    switch (status) {
    case GatewayClientMemberListStatus::online: return MC_PRESENCE_ONLINE;
    case GatewayClientMemberListStatus::idle: return MC_PRESENCE_IDLE;
    case GatewayClientMemberListStatus::dnd: return MC_PRESENCE_DND;
    case GatewayClientMemberListStatus::offline: return MC_PRESENCE_OFFLINE;
    default: return MC_PRESENCE_UNKNOWN;
    }
}

// A resident message of the open channel with its text, or null.
const MessageRecord* resident(u64 channel_id, u64 message_id, Utf8View* content) noexcept {
    const auto& store = g_app.message_view.store;
    const auto found = message_store_find(store, message_id);
    const auto* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    if (!record || (channel_id && record->channel_id != channel_id)) return nullptr;
    if (content)
        *content = record->content_availability == MessageContentAvailability::available
                       ? message_store_content(store, found.handle)
                       : Utf8View{};
    return record;
}

// What hooks see of a resident message.
PluginMessage to_plugin(const MessageRecord& record, Utf8View content) noexcept {
    PluginMessage message{};
    message.message_id = record.id;
    message.channel_id = record.channel_id;
    message.author_id = record.author_id;
    message.timestamp_ms = record.timestamp_ms;
    message.edited_ms = record.edited_timestamp_ms;
    message.content = content;
    message.type = record.type;
    message.revision = record.revision;
    const AccountUiRuntime* ui = ui_state();
    if (!ui) return message;
    const auto* channel = chat_state_store_find_channel(ui->chat_state, record.channel_id);
    message.guild_id = channel ? channel->guild_id : 0;
    message.author_name = display_name(*ui, message.guild_id, record.author_id);
    message.relationship = relationship_value(record.author_id);
    u32 flags = 0;
    if (record.author_id && record.author_id == self_id()) flags |= MC_MESSAGE_OWN;
    if (user_is_bot(*ui, record.author_id)) flags |= MC_MESSAGE_BOT;
    if (record.edited_timestamp_ms) flags |= MC_MESSAGE_EDITED;
    if (record.presentation_flags & message_presentation_pinned) flags |= MC_MESSAGE_PINNED;
    if (message_type_is_system(record.type)) flags |= MC_MESSAGE_SYSTEM;
    if (record.delivery == MessageDeliveryState::retained) flags |= MC_MESSAGE_DELETED;
    if (record.type == kMessageTypeLocalNotice) flags |= MC_MESSAGE_LOCAL;
    if (channel && channel_is_private(channel->type)) flags |= MC_MESSAGE_PRIVATE;
    if (const auto* rich = message_rich_store_find(ui->message_rich_store, record.id);
        rich && rich->message_revision == record.revision) {
        message.reply_to = rich->reference.message_id;
        message.attachment_count = rich->attachment_count;
        message.embed_count = rich->embed_count;
        if (rich->reference.message_id) flags |= MC_MESSAGE_REPLY;
        if (rich->attachment_count || rich->embed_count) flags |= MC_MESSAGE_MEDIA;
    }
    message.flags = flags;
    return message;
}

// ---- Gateway: hooks and the event feed ------------------------------------------------------------

// Removals asked for here (application_plugins_own_removal), each matched once by the event confirming it.
struct OwnRemoval {
    u64 id, until;
    u32 kind;
};
OwnRemoval g_own_removals[4];
static_assert(static_cast<u32>(PluginOwnRemoval::channel) == MC_EVENT_CHANNEL_DELETE &&
              static_cast<u32>(PluginOwnRemoval::guild) == MC_EVENT_GUILD_DELETE &&
              static_cast<u32>(PluginOwnRemoval::relationship) == MC_EVENT_RELATIONSHIP_REMOVE);

bool own_removal(const PluginEvent& event) noexcept {
    const u64 id = event.kind == MC_EVENT_GUILD_DELETE     ? event.guild_id
                   : event.kind == MC_EVENT_CHANNEL_DELETE ? event.channel_id
                                                           : event.user_id;
    const u64 now = clock_monotonic_milliseconds();
    for (OwnRemoval& removal : g_own_removals)
        if (removal.kind == event.kind && removal.id == id && now < removal.until) {
            removal = {};
            return true;
        }
    return false;
}

// Queues an event for the feed; the common fields are filled here.
void push(PluginEvent event, const AccountUiRuntime* ui) noexcept {
    if (!(plugin_events_interest() & MC_EVENT_BIT(event.kind))) return;
    if (event.user_id && event.user_id == self_id()) event.flags |= MC_EVENT_OWN;
    if ((event.kind == MC_EVENT_GUILD_DELETE || event.kind == MC_EVENT_CHANNEL_DELETE ||
         event.kind == MC_EVENT_RELATIONSHIP_REMOVE) &&
        own_removal(event))
        event.flags |= MC_EVENT_OWN;
    if (!event.guild_id && ui) event.guild_id = guild_of_channel(*ui, event.channel_id);
    event.time_ms = clock_unix_milliseconds();
    plugin_events_push(event);
}

// Text, author and bot flag of a message about to change, from the cache.
void fill_from_cache(PluginEvent* event, const AccountUiRuntime* ui, bool with_text) noexcept {
    Utf8View content{};
    const MessageRecord* record = resident(event->channel_id, event->message_id, &content);
    if (!record) return;
    if (!event->user_id) event->user_id = record->author_id;
    if (ui && !event->name.bytes) event->name = display_name(*ui, event->guild_id, record->author_id);
    if (with_text && content.bytes) {
        event->text = content;
        event->flags |= MC_EVENT_TEXT;
    }
    if (event->name.bytes || event->text.bytes) event->flags |= MC_EVENT_CACHED;
    if (ui && user_is_bot(*ui, record->author_id)) event->flags |= MC_EVENT_BOT;
}

void deleted_message(u64 channel_id, u64 message_id, bool bulk, const AccountUiRuntime* ui) noexcept {
    Utf8View content{};
    const MessageRecord* record = resident(channel_id, message_id, &content);
    // Only a confirmed message with its text can stay on screen.
    if (record && record->delivery == MessageDeliveryState::authoritative && content.bytes &&
        plugin_hook_subscribed(PluginHook::message_deleted)) {
        PluginMessage message = to_plugin(*record, content);
        message.flags |= MC_MESSAGE_CACHED;
        plugin_dispatch_deleted(message);
    }
    PluginEvent event{};
    event.kind = MC_EVENT_MESSAGE_DELETE;
    event.channel_id = channel_id;
    event.message_id = message_id;
    event.guild_id = ui ? guild_of_channel(*ui, channel_id) : 0;
    if (bulk) event.flags |= MC_EVENT_BULK;
    if (plugin_events_interest() & MC_EVENT_BIT(MC_EVENT_MESSAGE_DELETE)) fill_from_cache(&event, ui, true);
    push(event, ui);
}

void message_changed(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui) noexcept {
    const bool create = event.dispatch_kind == GatewayClientDispatchKind::message_create;
    const u64 bit = MC_EVENT_BIT(create ? MC_EVENT_MESSAGE_CREATE : MC_EVENT_MESSAGE_UPDATE);
    const bool edit_hook = !create && plugin_hook_subscribed(PluginHook::message_edited);
    if (!(plugin_events_interest() & bit) && !edit_hook) return;
    GatewayMessageEvent message{};
    if (!gateway_client_event_as_message(event, payload, &message)) return;
    Utf8View before_content{};
    const MessageRecord* record = edit_hook ? resident(message.channel_id, message.id, &before_content) : nullptr;
    if (record && record->delivery == MessageDeliveryState::authoritative && before_content.bytes &&
        (message.fields & gateway_message_field_content) && message.content.bytes &&
        !utf8_equal(before_content, message.content)) {
        const PluginMessage before = to_plugin(*record, before_content);
        PluginMessage after = before;
        after.content = message.content;
        after.flags |= MC_MESSAGE_EDITED;
        after.edited_ms = message.edited_timestamp_ms ? message.edited_timestamp_ms : clock_unix_milliseconds();
        plugin_dispatch_edited(before, after);
    }
    if (!(plugin_events_interest() & bit)) return;
    PluginEvent out{};
    out.kind = create ? MC_EVENT_MESSAGE_CREATE : MC_EVENT_MESSAGE_UPDATE;
    out.channel_id = message.channel_id;
    out.message_id = message.id;
    out.guild_id = ui ? guild_of_channel(*ui, message.channel_id) : 0;
    if (message.fields & gateway_message_field_author_id) {
        out.user_id = message.author_id;
        out.name = message.author_name;
        if (message.author_bot) out.flags |= MC_EVENT_BOT;
    }
    if (create || (message.fields & gateway_message_field_content)) {
        out.text = message.content;
        out.flags |= MC_EVENT_TEXT;
    }
    if (!create && !out.user_id) fill_from_cache(&out, ui, false);
    push(out, ui);
}

void voice_changed(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui) noexcept {
    const auto& snapshot = event.voice_states;
    // A guild snapshot is state discovery, not a change.
    if (snapshot.replace || payload.count != snapshot.record_count * sizeof(GatewayClientVoiceStateRecord)) return;
    constexpr u32 kGatewayFlags = 0x7Fu;
    for (u32 i = 0; i < snapshot.record_count; ++i) {
        GatewayClientVoiceStateRecord record;
        copy_bytes(&record, payload.data + i * sizeof(record), sizeof(record));
        if (!record.user_id) continue;
        const auto* known = ui ? voice_state_store_find(ui->voice_states, record.guild_id, record.user_id) : nullptr;
        if ((known ? known->channel_id : 0) == record.channel_id &&
            (known ? known->flags & kGatewayFlags : 0) == (record.flags & kGatewayFlags))
            continue;
        PluginEvent out{};
        out.kind = MC_EVENT_VOICE_STATE;
        out.guild_id = record.guild_id;
        out.channel_id = record.channel_id;
        out.user_id = record.user_id;
        push(out, ui);
    }
}

void state_changed(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui) noexcept {
    using K = GatewayClientDispatchKind;
    const auto kind = event.dispatch_kind;
    const auto& state = event.state;
    if (!state.id) return;
    // A server going unavailable in an outage is not a removal.
    if (kind == K::guild_delete && (state.fields & gateway_client_state_field_unavailable) && state.unavailable) return;
    PluginEvent out{};
    Utf8View name{};
    if (gateway_client_event_state_name(event, payload, &name)) out.name = name;
    if (kind == K::guild_create || kind == K::guild_update || kind == K::guild_delete) {
        out.kind = kind == K::guild_create ? MC_EVENT_GUILD_CREATE
                   : kind == K::guild_update ? MC_EVENT_GUILD_UPDATE
                                             : MC_EVENT_GUILD_DELETE;
        out.guild_id = state.id;
        if (!out.name.bytes && ui)
            if (const auto* guild = chat_state_store_find_guild(ui->chat_state, state.id))
                out.name = chat_state_store_guild_name(ui->chat_state, *guild);
    } else {
        out.kind = kind == K::channel_create ? MC_EVENT_CHANNEL_CREATE
                   : kind == K::channel_update ? MC_EVENT_CHANNEL_UPDATE
                                               : MC_EVENT_CHANNEL_DELETE;
        out.channel_id = state.id;
        out.guild_id = state.guild_id;
        out.value = state.type;
        const auto* known = ui ? chat_state_store_find_channel(ui->chat_state, state.id) : nullptr;
        if (known) {
            if (!(state.fields & gateway_client_state_field_guild_id)) out.guild_id = known->guild_id;
            if (!(state.fields & gateway_client_state_field_type)) out.value = known->type;
            if (!out.name.bytes) out.name = chat_state_store_channel_name(ui->chat_state, *known);
        }
    }
    push(out, ui);
}

void member_changed(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui) noexcept {
    using K = GatewayClientDispatchKind;
    PluginEvent out{};
    if (event.dispatch_kind == K::guild_member_update) {
        // Your own member object (roles, timeout); others arrive as profile updates.
        out.kind = MC_EVENT_MEMBER_UPDATE;
        out.guild_id = event.state.guild_id;
        out.user_id = event.state.id;
    } else if (event.dispatch_kind == K::guild_member_remove) {
        Utf8View name{};
        if (!gateway_client_event_guild_membership(event, payload, &out.guild_id, &out.user_id, &name)) return;
        out.kind = MC_EVENT_MEMBER_REMOVE;
        out.name = name;
    } else {
        u32 cursor = 0, index = 0;
        GatewayClientMemberListMutation mutation{};
        ConstSlice<u8> roles{};
        Utf8View nickname{}, display{}, avatar{}, username{};
        if (event.member_list.record_count != 1 ||
            !gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation, &roles, &nickname,
                                                     &display, &avatar, &username))
            return;
        out.kind = event.member_list.member_added ? MC_EVENT_MEMBER_ADD : MC_EVENT_MEMBER_UPDATE;
        out.guild_id = event.member_list.guild_id;
        out.user_id = mutation.user_id;
        out.name = nickname.bytes ? nickname : display.bytes ? display : username;
    }
    if (!out.guild_id || !out.user_id) return;
    if (!out.name.bytes && ui) out.name = display_name(*ui, out.guild_id, out.user_id);
    push(out, ui);
}

void relationship_changed(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui) noexcept {
    u32 cursor = 0;
    RelationshipMutation mutation{};
    if (event.state.record_count != 1 || !gateway_client_relationship_record(payload, &cursor, &mutation) ||
        cursor != payload.count || !mutation.record.user_id)
        return;
    PluginEvent out{};
    out.user_id = mutation.record.user_id;
    if (event.dispatch_kind == GatewayClientDispatchKind::relationship_remove) {
        out.kind = MC_EVENT_RELATIONSHIP_REMOVE;
        // What ended: the relationship the store still holds.
        out.value = relationship_value(out.user_id);
    } else {
        // An update that keeps the type is not a change.
        const u32 before = relationship_value(out.user_id);
        switch (mutation.record.type) {
        case RelationshipType::friend_: out.value = MC_RELATIONSHIP_FRIEND; break;
        case RelationshipType::blocked: out.value = MC_RELATIONSHIP_BLOCKED; break;
        case RelationshipType::incoming: out.value = MC_RELATIONSHIP_INCOMING; break;
        case RelationshipType::outgoing: out.value = MC_RELATIONSHIP_OUTGOING; break;
        default: return;
        }
        if (before == out.value) return;
        out.kind = MC_EVENT_RELATIONSHIP_ADD;
    }
    if (ui && mutation.user.id) {
        // The name arrives with the record when the store has none yet.
        const auto* known = user_store_find(ui->user_store, out.user_id);
        const auto& user = mutation.user;
        out.name = known                   ? user_store_name(ui->user_store, *known)
                   : user.display_name_bytes ? Utf8View{user.display_name, user.display_name_bytes}
                                             : Utf8View{user.username, user.username_bytes};
    }
    push(out, ui);
}

// ---- Kept messages on screen -------------------------------------------------------------------

// Puts tombstones of the open channel back into the timeline once its history is resident.
bool restore_tombstones() noexcept {
    if (!g_app.account_ui || !keep_stats().messages) return false;
    const u64 channel_id = g_app.account_ui->active_channel_id;
    auto& timeline = g_app.message_view.timeline;
    if (!channel_id || !timeline.store || !timeline.list) return false;
    KeepMessage kept[kKeepPerChannelMax];
    const u32 count = keep_tombstones(channel_id, kept, kKeepPerChannelMax);
    bool changed = false;
    for (u32 i = 0; i < count && i < kKeepPerChannelMax; ++i) {
        if (message_store_find(*timeline.store, kept[i].message_id).ok()) continue;
        MessageInput input{};
        input.id = kept[i].message_id;
        input.channel_id = channel_id;
        input.author_id = kept[i].author_id;
        input.timestamp_ms = kept[i].timestamp_ms;
        input.edited_timestamp_ms = kept[i].edited_ms;
        input.content = kept[i].content;
        input.delivery = MessageDeliveryState::retained;
        input.type = 0;
        changed = message_timeline_insert_retained(&timeline, input).mutation.inserted || changed;
    }
    return changed;
}

// The tombstone rows of the timeline, removed (the keep store is gone or the account changed).
void drop_tombstone_rows() noexcept {
    auto& timeline = g_app.message_view.timeline;
    if (!timeline.store || !timeline.list) return;
    for (u32 i = timeline.store->count; i; --i) {
        const auto* record = message_store_resolve(*timeline.store, message_store_ordered_handle(*timeline.store, i - 1));
        if (record && record->delivery == MessageDeliveryState::retained) message_timeline_remove_local(&timeline, record->id);
    }
}

// ---- Actions ------------------------------------------------------------------------------------

const MessageRecord* cached_message(u64 channel_id, u64 message_id) noexcept {
    const auto* record = application_message_record(message_id);
    return record && record->channel_id == channel_id ? record : nullptr;
}

mc_status sink_send(u64, const PluginSend& send) noexcept {
    if (!account_ready()) return MC_E_BUSY;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_find_channel(ui.chat_state, send.channel_id)) return MC_E_NOT_FOUND;
    if (!application_thread_actions_channel_sendable(send.channel_id)) return MC_E_DENIED;
    if (send.reply_to && !cached_message(send.channel_id, send.reply_to)) return MC_E_NOT_FOUND;
    if (send.reply_to && !application_channel_readable(g_app, send.channel_id)) return MC_E_DENIED;
    char storage[MC_DRAFT_TEXT_MAX + 16];
    MessageComposer composer{};
    if (!message_composer_initialize(&composer, {storage, sizeof(storage)}) ||
        !message_composer_restore(&composer, send.channel_id, send.text, send.text.bytes, send.text.bytes))
        return MC_E_INVALID;
    const auto queued = message_composer_controller_queue_text(
        &ui.message_composer_controller, &composer, ui.chat_state.self_user_id, clock_unix_milliseconds(),
        clock_monotonic_milliseconds(), send.reply_to, (send.flags & MC_DRAFT_REPLY_PING) != 0,
        (send.flags & MC_DRAFT_SILENT) != 0, (send.flags & MC_DRAFT_SUPPRESS_EMBEDS) != 0);
    if (!queued.ok())
        return queued.error == MessageComposerControllerError::invalid_input ? MC_E_INVALID : MC_E_BUSY;
    pump_message_history();
    return MC_OK;
}

mc_status sink_edit(u64, u64 channel, u64 message, Utf8View text) noexcept {
    if (!account_ready()) return MC_E_BUSY;
    const auto* record = cached_message(channel, message);
    // Edits go through the open channel's message view.
    if (!record || channel != g_app.account_ui->active_channel_id) return MC_E_NOT_FOUND;
    if (record->author_id != g_app.account_ui->chat_state.self_user_id ||
        !application_thread_actions_channel_sendable(channel))
        return MC_E_DENIED;
    if (g_app.message_edit.message_id || g_app.message_edit.editor_active) return MC_E_BUSY;
    return queue_message_edit_content(message, text) ? MC_OK : MC_E_DENIED;
}

mc_status reaction_result(ApplicationMessageReactionEnqueueResult result) noexcept {
    switch (result) {
    case ApplicationMessageReactionEnqueueResult::accepted:
    case ApplicationMessageReactionEnqueueResult::coalesced: return MC_OK;
    case ApplicationMessageReactionEnqueueResult::permission: return MC_E_DENIED;
    case ApplicationMessageReactionEnqueueResult::stale: return MC_E_NOT_FOUND;
    case ApplicationMessageReactionEnqueueResult::invalid: return MC_E_INVALID;
    default: return MC_E_BUSY;
    }
}

// "name:id" is a custom emoji; anything else is Unicode.
void split_emoji(Utf8View emoji, Utf8View* name, u64* id) noexcept {
    u32 colon = emoji.bytes;
    for (u32 i = 0; i < emoji.bytes; ++i)
        if (emoji.data[i] == ':') {
            colon = i;
            break;
        }
    *id = 0;
    *name = emoji;
    if (colon == emoji.bytes) return;
    *name = {emoji.data, colon};
    for (u32 i = colon + 1; i < emoji.bytes; ++i) {
        if (emoji.data[i] < '0' || emoji.data[i] > '9') {
            *id = 0;
            return;
        }
        *id = *id * 10 + static_cast<u64>(emoji.data[i] - '0');
    }
}

mc_status sink_react(u64, u64 channel, u64 message, Utf8View emoji, bool remove) noexcept {
    if (!account_ready()) return MC_E_BUSY;
    if (channel != g_app.account_ui->active_channel_id || !cached_message(channel, message)) return MC_E_NOT_FOUND;
    Utf8View name{};
    u64 id = 0;
    split_emoji(emoji, &name, &id);
    auto& ui = *g_app.account_ui;
    if (remove) {
        // Taking a reaction back toggles the one you already have.
        const auto* record = application_message_record(message);
        const auto* rich = record ? message_rich_store_find(ui.message_rich_store, message) : nullptr;
        if (!rich || rich->message_revision != record->revision) return MC_E_NOT_FOUND;
        for (u32 i = 0; i < rich->reaction_count; ++i) {
            const auto& item = rich->reactions[i];
            if ((item.flags & 1u) && item.emoji_id == id &&
                (id || utf8_equal(message_rich_store_string(ui.message_rich_store, item.name), name)))
                return reaction_result(application_queue_message_reaction(message, i));
        }
        return MC_E_NOT_FOUND;
    }
    const auto* custom = id ? chat_state_store_find_emoji(ui.chat_state, id) : nullptr;
    return reaction_result(
        application_queue_message_reaction_emoji(message, id, name, custom && (custom->flags & custom_emoji_animated)));
}

mc_status sink_delete(u64, u64 channel, u64 message) noexcept {
    if (!account_ready()) return MC_E_BUSY;
    const auto* record = cached_message(channel, message);
    if (!record || channel != g_app.account_ui->active_channel_id) return MC_E_NOT_FOUND;
    if (record->author_id != g_app.account_ui->chat_state.self_user_id) return MC_E_DENIED;
    return application_message_delete_request(message, true) ? MC_OK : MC_E_BUSY;
}

bool begin_scoped_read(ReadScopeKind kind, u64 id) noexcept {
    ApplicationScopedReadTicket ticket{};
    return application_scoped_read_capture(kind, id, &ticket) && application_scoped_read_begin(ticket);
}

mc_status sink_mark_read(u64, u32 scope, u64 id) noexcept {
    if (!account_ready()) return MC_E_BUSY;
    auto& chat = g_app.account_ui->chat_state;
    if (scope == MC_READ_CHANNEL) {
        if (!chat_state_store_find_channel(chat, id)) return MC_E_NOT_FOUND;
        return application_read_state_mark(id, true) ? MC_OK : MC_E_DENIED;
    }
    if (scope == MC_READ_GUILD) {
        if (!chat_state_store_find_guild(chat, id)) return MC_E_NOT_FOUND;
        return begin_scoped_read(ReadScopeKind::guild, id) ? MC_OK : MC_E_BUSY;
    }
    // Every server, one after the other.
    bool any = false;
    for (u32 i = 0; i < chat.guild_count; ++i) any = begin_scoped_read(ReadScopeKind::guild, chat.guilds[chat.guild_order[i]].id) || any;
    return any ? MC_OK : MC_E_BUSY;
}

mc_status sink_activity(u64 owner, const mc_activity* activity) noexcept {
    if (!activity) return application_activity_set_plugin(owner, nullptr) ? MC_OK : MC_E_NOT_FOUND;
    if (!account_ready()) return MC_E_BUSY;
    if (activity->type == MC_ACTIVITY_CUSTOM) {
        const u64 now = clock_unix_milliseconds();
        SelfCustomStatus status{};
        status.text_bytes = static_cast<u16>(utf8_copy_truncated(status.text, {activity->state.data, activity->state.bytes}));
        status.emoji_name_bytes =
            static_cast<u8>(utf8_copy_truncated(status.emoji_name, {activity->emoji.data, activity->emoji.bytes}));
        status.present = status.text_bytes || status.emoji_name_bytes;
        if (status.present) {
            status.created_at_ms = now;
            status.expires_at_ms = activity->end_ms;
        }
        return application_custom_status_request(&status) ? MC_OK : MC_E_BUSY;
    }
    // A streaming activity needs a stream; it is not offered.
    if (activity->type == MC_ACTIVITY_STREAMING) return MC_E_UNSUPPORTED;
    const ApplicationPluginActivity value{activity->application_id,
                                          activity->start_ms,
                                          activity->end_ms,
                                          static_cast<u8>(activity->type),
                                          {activity->name.data, activity->name.bytes},
                                          {activity->details.data, activity->details.bytes},
                                          {activity->state.data, activity->state.bytes}};
    return application_activity_set_plugin(owner, &value) ? MC_OK : MC_E_BUSY;
}

mc_status sink_local(u64, u64 channel, Utf8View text, u64* id) noexcept {
    return application_plugins_local_notice(channel, text, id) ? MC_OK : MC_E_NOT_FOUND;
}

mc_status sink_local_remove(u64, u64 channel, u64 message) noexcept {
    const auto* record = cached_message(channel, message);
    // Only local messages go: a plugin cannot remove what the server sent.
    if (!record || record->type != kMessageTypeLocalNotice || !g_app.message_scene.timeline) return MC_E_NOT_FOUND;
    message_timeline_remove_local(g_app.message_scene.timeline, message);
    application_request_frame();
    return MC_OK;
}

// ---- Composer ---------------------------------------------------------------------------------

bool composer_ready() noexcept {
    return account_ready() && g_app.message_composer.channel_id && !g_app.login.visible;
}

mc_status sink_composer_text(void* output, u32 capacity, u32* bytes) noexcept {
    if (!composer_ready()) return MC_E_BUSY;
    const Utf8View text = message_composer_text(g_app.message_composer);
    *bytes = text.bytes;
    if (text.bytes > capacity) return MC_E_TRUNCATED;
    copy_bytes(output, text.data, text.bytes);
    return MC_OK;
}

mc_status sink_composer_insert(Utf8View text) noexcept {
    if (!composer_ready()) return MC_E_BUSY;
    return composer_apply_result(message_composer_insert_utf8(&g_app.message_composer, text), true) ? MC_OK : MC_E_CAPACITY;
}

mc_status sink_composer_replace(Utf8View text) noexcept {
    if (!composer_ready()) return MC_E_BUSY;
    auto& composer = g_app.message_composer;
    const auto result = message_composer_replace_range(&composer, 0, composer.bytes, text);
    if (result == MessageComposerResult::invalid_input) return MC_E_CAPACITY;
    return composer_apply_result(result, true) ? MC_OK : MC_E_BUSY;
}

mc_status sink_composer_focus() noexcept {
    if (!composer_ready()) return MC_E_BUSY;
    return focus_message_composer_control(MessageComposerControl::none) ? MC_OK : MC_E_BUSY;
}

mc_status sink_composer_reply(u64 channel, u64 message, bool ping) noexcept {
    if (!composer_ready()) return MC_E_BUSY;
    if (channel != g_app.message_composer.channel_id) return MC_E_NOT_FOUND;
    if (!message) return application_reply_draft_cancel() ? MC_OK : MC_E_DENIED;
    if (!cached_message(channel, message)) return MC_E_NOT_FOUND;
    return application_reply_draft_set(message, ping) ? MC_OK : MC_E_DENIED;
}

mc_status sink_composer_edit(u64 channel, u64 message) noexcept {
    if (!composer_ready()) return MC_E_BUSY;
    if (channel != g_app.message_composer.channel_id || !cached_message(channel, message)) return MC_E_NOT_FOUND;
    return begin_message_edit(message) ? MC_OK : MC_E_DENIED;
}

void sink_keep_changed() noexcept {
    application_request_frame();
}

// ---- Reads ------------------------------------------------------------------------------------

mc_status sink_open_channel(mc_open_channel* out) noexcept {
    const AccountUiRuntime* ui = account_ready() ? ui_state() : nullptr;
    if (!ui || !ui->active_channel_id) return MC_E_NOT_FOUND;
    out->channel_id = ui->active_channel_id;
    out->guild_id = guild_of_channel(*ui, out->channel_id);
    if (g_app.message_composer.channel_id == out->channel_id) {
        const MessageComposerReplySnapshot reply = application_reply_draft();
        out->reply_to = reply.channel_id == out->channel_id ? reply.message_id : 0;
    }
    out->edit_id = g_app.message_edit.editor_active ? g_app.message_edit.message_id : 0;
    return MC_OK;
}

// The open channel's loaded window, newest first, from the row after `before`.
mc_status sink_messages(u64 channel_id, u64 before, mc_message_ref* out, u32 capacity, u32* count) noexcept {
    const AccountUiRuntime* ui = account_ready() ? ui_state() : nullptr;
    if (!ui || channel_id != ui->active_channel_id) return MC_E_NOT_FOUND;
    const MessageStore& store = g_app.message_view.store;
    bool below = !before;
    for (u32 i = store.count; i-- > 0 && *count < capacity;) {
        const MessageRecord* record = message_store_resolve(store, message_store_ordered_handle(store, i));
        if (!record || !record->id || record->channel_id != channel_id) continue;
        if (!below) {
            below = record->id == before;
            continue;
        }
        out[(*count)++] = {record->id, record->author_id, record->timestamp_ms, to_plugin(*record, {}).flags, record->type};
    }
    return below ? MC_OK : MC_E_NOT_FOUND;
}

const PluginPipelineSink kSink{sink_send,         sink_edit,          sink_react,          sink_delete,
                               sink_mark_read,    sink_activity,      sink_local,          sink_local_remove,
                               sink_composer_text, sink_composer_insert, sink_composer_replace, sink_composer_focus,
                               sink_composer_reply, sink_composer_edit, sink_keep_changed, sink_open_channel,
                               sink_messages};

// ---- Confirming a send ---------------------------------------------------------------------------

TextFormOwnerKey g_confirm;

bool confirm_valid(TextFormOwnerKey key) noexcept {
    const MessageComposer& composer = g_app.message_composer;
    return g_app.account_ui && key.scope == g_app.plugin_account_scope_serial && key.scope == g_confirm.scope &&
           key.collection == g_confirm.collection && key.item == g_confirm.item &&
           composer.channel_id == key.collection && g_app.account_ui->active_channel_id == key.collection &&
           composer.text_revision == key.item && !g_app.message_edit.editor_active;
}

bool confirm_save(TextFormOwnerKey key, Utf8View) noexcept {
    // The draft changed or the channel went away: closing is the answer.
    if (!confirm_valid(key)) return true;
    reset_value(&g_confirm);
    application_message_send_confirmed();
    return true;
}

} // namespace

void application_plugins_pipeline_install() noexcept {
    plugin_pipeline_set_sink(&kSink);
    g_message_retain_query = keep_is_deleted;
    g_app.message_view.timeline.retains = true;
}

void application_plugins_pipeline_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    using K = GatewayClientDispatchKind;
    if (event.kind != GatewayClientEventKind::dispatch) return;
    const bool hooks = plugin_hook_subscribed(PluginHook::message_deleted) || plugin_hook_subscribed(PluginHook::message_edited);
    const u64 interest = plugin_events_interest();
    if (!hooks && !interest) return;
    application_plugins_pipeline_install();
    const AccountUiRuntime* ui = ui_state();
    switch (event.dispatch_kind) {
    case K::message_create:
    case K::message_update: message_changed(event, payload, ui); break;
    case K::message_delete: {
        GatewayMessageDeleteEvent message{};
        if (gateway_client_event_as_message_delete(event, payload, &message))
            deleted_message(message.channel_id, message.id, false, ui);
        break;
    }
    case K::message_bulk_delete: {
        u64 ids[kGatewayBulkDeleteMaximumIds];
        GatewayMessageBulkDeleteEvent message{};
        if (!gateway_client_event_as_message_bulk_delete(event, payload, {ids, kGatewayBulkDeleteMaximumIds}, &message))
            break;
        for (u32 i = 0; i < message.id_count; ++i) deleted_message(message.channel_id, ids[i], true, ui);
        break;
    }
    default: break;
    }
    if (!interest) return;
    switch (event.dispatch_kind) {
    case K::typing_start: {
        GatewayTypingStartEvent typing{};
        if (!gateway_client_event_as_typing_start(event, payload, &typing)) break;
        PluginEvent out{};
        out.kind = MC_EVENT_TYPING;
        out.channel_id = typing.channel_id;
        out.user_id = typing.user_id;
        out.guild_id = typing.guild_id;
        push(out, ui);
        break;
    }
    case K::message_reaction_add:
    case K::message_reaction_remove: {
        GatewayMessageReactionEvent reaction{};
        if (!gateway_client_event_as_message_reaction(event, payload, &reaction)) break;
        PluginEvent out{};
        out.kind = event.dispatch_kind == K::message_reaction_add ? MC_EVENT_REACTION_ADD : MC_EVENT_REACTION_REMOVE;
        out.channel_id = reaction.channel_id;
        out.message_id = reaction.message_id;
        out.user_id = reaction.user_id;
        out.value = reaction.emoji_id;
        out.text = reaction.name;
        if (reaction.name.bytes) out.flags |= MC_EVENT_TEXT;
        push(out, ui);
        break;
    }
    case K::presence_update: {
        const auto& presence = event.presence;
        if (!presence.user_id || presence.status > GatewayClientMemberListStatus::offline) break;
        PluginEvent out{};
        out.kind = MC_EVENT_PRESENCE;
        out.guild_id = presence.guild_id;
        out.user_id = presence.user_id;
        out.value = presence_value(presence.status);
        push(out, ui);
        break;
    }
    case K::voice_state_snapshot: voice_changed(event, payload, ui); break;
    case K::channel_create:
    case K::channel_update:
    case K::channel_delete:
    case K::guild_create:
    case K::guild_update:
    case K::guild_delete: state_changed(event, payload, ui); break;
    case K::guild_member_update:
    case K::guild_member_profile_update:
    case K::guild_member_remove: member_changed(event, payload, ui); break;
    case K::relationship_add:
    case K::relationship_update:
    case K::relationship_remove: relationship_changed(event, payload, ui); break;
    default: break;
    }
}

namespace {
void poll_pipeline() noexcept {
    // With no plugin keeping or listening this is a handful of loads.
    const bool keepers = plugin_hook_subscribed(PluginHook::message_deleted) ||
                         plugin_hook_subscribed(PluginHook::message_edited);
    if (!keepers && !plugin_hook_subscribed(PluginHook::events) && !plugin_hook_subscribed(PluginHook::timer) &&
        !keep_stats().messages)
        return;
    application_plugins_pipeline_install();
    // Kept messages belong to the account that saw them, and to the plugins that kept them: the tombstones
    // go when the last one is disabled.
    static u64 profile;
    static bool had_keepers;
    if (profile != g_app.account.status.profile_id || (had_keepers && !keepers)) {
        if (profile) application_plugins_pipeline_account_invalidated();
        profile = g_app.account.status.profile_id;
    }
    had_keepers = keepers;
    const bool pending = plugin_events_deliver();
    const bool due = plugin_timers_poll(clock_monotonic_milliseconds()) != 0;
    // Tombstones return once the window they belong to is resident again: whenever the timeline was
    // rebuilt, gained a page or a message, or the open channel or the kept set changed.
    struct Seen {
        u64 channel;
        u32 generation, pages, created, deleted, kept;
    };
    static Seen seen;
    const auto& timeline = g_app.message_view.timeline;
    const Seen now{g_app.account_ui ? g_app.account_ui->active_channel_id : 0, timeline.reset_generation,
                   timeline.history_pages, timeline.store ? timeline.store->created : 0u,
                   timeline.store ? timeline.store->deleted : 0u, keep_stats().messages};
    if (now.channel != seen.channel || now.generation != seen.generation || now.pages != seen.pages ||
        now.created != seen.created || now.deleted != seen.deleted || now.kept != seen.kept) {
        seen = now;
        if (restore_tombstones()) application_request_frame();
    }
    if (pending || due) application_request_frame();
}
} // namespace

// Once per loop pass: what plugins changed since the UI last built from it, the pipeline's queues and timers,
// and the composer notice that has run out.
void application_plugins_poll() noexcept {
    application_plugins_composer_notice_poll(clock_monotonic_milliseconds());
    application_plugin_surfaces_poll();
    poll_pipeline();
}

u32 application_plugins_wait_timeout(u64 now_ms) noexcept {
    u32 wait = application_plugins_composer_notice_wait(now_ms);
    if (plugin_events_pending()) return 0;
    const u64 due = plugin_timers_next_due();
    if (due) wait = minimum(wait, due <= now_ms ? 0u : static_cast<u32>(minimum<u64>(due - now_ms, 0x7FFFFFFFu)));
    return wait;
}

bool application_plugins_message_click(LPARAM lparam, bool middle, u32 count) noexcept {
    if (!plugin_hook_subscribed(PluginHook::message_click) || !g_app.account_ui || !g_app.message_scene.active ||
        g_app.login.visible)
        return false;
    const float x = client_dip_x(lparam), y = client_dip_y(lparam);
    if (!ui_rect_contains(g_app.message_scene.viewport, x, y)) return false;
    const MessageSceneHit hit = message_scene_hit_test(g_app.message_scene, x, y);
    Utf8View content{};
    const MessageRecord* record = hit.message_id ? resident(0, hit.message_id, &content) : nullptr;
    if (!record) return false;
    const u32 modifiers = (application_shift_down() ? MC_MOD_SHIFT : 0u) | (application_control_down() ? MC_MOD_CTRL : 0u) |
                          (application_alt_down() ? MC_MOD_ALT : 0u);
    return plugin_dispatch_click(to_plugin(*record, content),
                                 {middle ? static_cast<u32>(MC_CLICK_MIDDLE) : static_cast<u32>(MC_CLICK_LEFT), count, modifiers});
}

bool application_plugins_typing_suppressed(u64 channel_id) noexcept {
    if (!plugin_hook_subscribed(PluginHook::pre_typing)) return false;
    // Every key press asks; plugins answer once per channel for a couple of seconds.
    static u64 channel, until;
    static bool suppressed;
    const u64 now = clock_monotonic_milliseconds();
    if (channel != channel_id || now >= until) {
        channel = channel_id;
        until = now + 2000;
        suppressed = plugin_dispatch_typing(channel_id);
    }
    return suppressed;
}

bool application_plugins_send_confirm(Utf8View question) noexcept {
    const MessageComposer& composer = g_app.message_composer;
    g_confirm = {g_app.plugin_account_scope_serial, composer.channel_id, composer.text_revision};
    ApplicationTextFormRequest request{question, {}, {}, utf8_literal("Send"), {}, g_confirm, confirm_valid, confirm_save, 0, true};
    request.accept_focused = true;
    if (application_text_form_open(request)) return true;
    reset_value(&g_confirm);
    return false;
}

namespace {
bool g_suppress_embeds;
} // namespace

void application_plugins_suppress_embeds_next(bool suppress) noexcept {
    g_suppress_embeds = suppress;
}

bool application_plugins_take_suppress_embeds() noexcept {
    const bool suppress = g_suppress_embeds;
    g_suppress_embeds = false;
    return suppress;
}

void application_plugins_pipeline_account_invalidated() noexcept {
    if (!keep_stats().messages) return;
    drop_tombstone_rows();
    keep_clear();
}

void application_plugins_own_removal(PluginOwnRemoval kind, u64 id) noexcept {
    const u32 event = static_cast<u32>(kind);
    if (!id || !(plugin_events_interest() & MC_EVENT_BIT(event))) return;
    // The oldest (or an expired) entry makes room.
    OwnRemoval* slot = &g_own_removals[0];
    for (OwnRemoval& removal : g_own_removals)
        if (removal.until < slot->until) slot = &removal;
    *slot = {id, clock_monotonic_milliseconds() + 60'000, event};
}
} // namespace maicord::app_detail

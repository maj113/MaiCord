#include "application_state.hpp"
#include "maicord/core/hash.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
u64 sync_signature;
} // namespace

bool thread_starter_project(const MessageStore& messages, const ChatStateStore& state,
                            MessageRichStore* rich_store) noexcept {
    if (!rich_store || !rich_store->records)
        return false;
    bool changed = false;
    for (u32 order = 0; order < messages.count; ++order) {
        const auto* record = message_store_resolve(messages, message_store_ordered_handle(messages, order));
        if (!record || record->delivery != MessageDeliveryState::authoritative)
            continue;
        const auto* rich = message_rich_store_find(*rich_store, record->id);
        if (rich && (rich->message_revision != record->revision || rich->thread.id))
            continue;
        const auto* thread = chat_state_store_find_channel(state, record->id);
        if (!thread || !thread->occupied || !channel_is_thread(thread->type) ||
            thread->parent_id != record->channel_id)
            continue;
        GatewayMessageEvent event{};
        event.id = record->id;
        event.channel_id = record->channel_id;
        event.fields = gateway_message_field_thread;
        event.thread.id = thread->id;
        event.thread.last_message_id = thread->last_message_id;
        event.thread.name = chat_state_store_channel_name(state, *thread);
        event.thread.archived = thread->thread_archived_known && thread->thread_archived;
        event.thread.locked = thread->thread_locked_known && thread->thread_locked;
        event.thread.present = true;
        changed |= message_rich_store_apply(rich_store, record->id, record->revision, event).ok();
    }
    return changed;
}

bool application_thread_starters_sync() noexcept {
    if (!g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    const auto& messages = g_app.message_view.store;
    u64 signature = hash64_mix(kHash64Seed, ui.active_channel_id);
    signature = hash64_mix(signature, messages.created);
    signature = hash64_mix(signature, messages.updated);
    signature = hash64_mix(signature, messages.deleted);
    signature = hash64_mix(signature, ui.chat_state.channel_mutations);
    if (signature == sync_signature)
        return false;
    sync_signature = signature;
    return thread_starter_project(messages, ui.chat_state, &ui.message_rich_store);
}
} // namespace maicord::app_detail

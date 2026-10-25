#include "maicord/app/message_view_runtime.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

struct MessageViewStorage {
    MessageRecord records[kMessageViewItemCapacity];
    u32 order[kMessageViewItemCapacity];
    u32 item_units[kMessageViewItemCapacity];
    u64 fenwick[kMessageViewItemCapacity];
};

MessageViewRuntimeResult runtime_error(MessageViewRuntimeError error) noexcept {
    return MessageViewRuntimeResult{error};
}

bool prepare_owned_storage(MessageViewRuntime* runtime) noexcept {
    if (runtime->storage.base) {
        if (!runtime->owns_storage) {
            return false;
        }
        Arena storage = runtime->storage;
        Arena text_storage = runtime->text_storage;
        arena_reset(&storage, ArenaRewindMode::erase);
        reset_value<MessageViewRuntime>(runtime);
        runtime->storage = storage;
        runtime->text_storage = text_storage;
    } else {
        reset_value<MessageViewRuntime>(runtime);
        if (!arena_initialize(&runtime->storage,
                              sizeof(MessageViewStorage), 0, 4 * 1024) ||
            !arena_initialize(&runtime->text_storage, kMessageViewTextCapacity, 0, kMessageStoreTextCommitStep)) {
            arena_release(&runtime->storage);
            return false;
        }
    }
    runtime->owns_storage = true;
    auto* storage = arena_allocate<MessageViewStorage>(&runtime->storage);
    if (!storage) {
        message_view_runtime_release(runtime);
        return false;
    }
    runtime->records = storage->records;
    runtime->order = storage->order;
    runtime->item_units = storage->item_units;
    runtime->fenwick = storage->fenwick;
    return true;
}

} // namespace

MessageViewRuntimeResult message_view_runtime_reindex(MessageViewRuntime* runtime, u32 default_item_units,
                                                      u64 viewport_units, bool follow_bottom) noexcept {
    if (!runtime || !runtime->initialized || !runtime->store.records ||
        runtime->store.records != runtime->records || runtime->store.order != runtime->order ||
        runtime->store.text != runtime->text_storage.base || default_item_units == 0 || viewport_units == 0) {
        return runtime_error(MessageViewRuntimeError::invalid_input);
    }
    MessageRichStore* const rich_store = runtime->timeline.rich_store;
    if (!ui_variable_list_initialize(&runtime->list,
                                     Slice<u32>{runtime->item_units, kMessageViewItemCapacity},
                                     Slice<u64>{runtime->fenwick, kMessageViewItemCapacity},
                                     runtime->store.count, default_item_units)) {
        return runtime_error(MessageViewRuntimeError::variable_list);
    }
    const u32 reset_generation = runtime->timeline.reset_generation;
    if (!message_timeline_initialize(&runtime->timeline, &runtime->store, &runtime->list,
                                     default_item_units, viewport_units, follow_bottom)) {
        return runtime_error(MessageViewRuntimeError::timeline);
    }
    runtime->timeline.reset_generation = reset_generation;
    if (rich_store && !message_timeline_bind_rich_store(&runtime->timeline, rich_store)) {
        return runtime_error(MessageViewRuntimeError::rich_store);
    }
    return runtime_error(MessageViewRuntimeError::none);
}

MessageViewRuntimeResult message_view_runtime_initialize(MessageViewRuntime* runtime,
                                                         u32 default_item_units, u64 viewport_units,
                                                         bool follow_bottom) noexcept {
    if (!runtime || default_item_units == 0 || viewport_units == 0) {
        return runtime_error(MessageViewRuntimeError::invalid_input);
    }
    if (!prepare_owned_storage(runtime)) {
        return runtime_error(MessageViewRuntimeError::store);
    }
    if (!message_store_initialize_growing(&runtime->store,
                                          Slice<MessageRecord>{runtime->records, kMessageViewItemCapacity},
                                          Slice<u32>{runtime->order, kMessageViewItemCapacity},
                                          &runtime->text_storage)) {
        return runtime_error(MessageViewRuntimeError::store);
    }
    runtime->initialized = true;
    const MessageViewRuntimeResult indexed =
        message_view_runtime_reindex(runtime, default_item_units, viewport_units, follow_bottom);
    if (!indexed.ok()) {
        message_view_runtime_release(runtime);
    }
    return indexed;
}

MessageViewRuntimeResult message_view_runtime_bind_rich_store(MessageViewRuntime* runtime,
                                                              MessageRichStore* rich_store) noexcept {
    if (!runtime || !runtime->initialized || !rich_store) {
        return runtime_error(MessageViewRuntimeError::invalid_input);
    }
    if (!message_timeline_bind_rich_store(&runtime->timeline, rich_store)) {
        return runtime_error(MessageViewRuntimeError::rich_store);
    }
    return runtime_error(MessageViewRuntimeError::none);
}

void message_view_runtime_unbind_rich_store(MessageViewRuntime* runtime) noexcept {
    if (runtime && runtime->initialized) {
        runtime->timeline.rich_store = nullptr;
    }
}

void message_view_runtime_release(MessageViewRuntime* runtime) noexcept {
    if (!runtime) {
        return;
    }
    if (runtime->owns_storage) {
        arena_release(&runtime->storage);
        arena_release(&runtime->text_storage);
    }
    reset_value<MessageViewRuntime>(runtime);
}

} // namespace maicord

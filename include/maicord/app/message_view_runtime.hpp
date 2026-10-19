#pragma once

#include "maicord/app/message_timeline.hpp"
#include "maicord/core/arena.hpp"

namespace maicord {

inline constexpr u32 kMessageViewItemCapacity = 256;
// Address space for every resident message at its longest; the text bank
// commits as content needs it (message_store_initialize_growing).
inline constexpr u32 kMessageViewTextCapacity = kMessageViewItemCapacity * kMaximumMessageContentBytes;

enum class MessageViewRuntimeError : u8 {
    none,
    invalid_input,
    store,
    variable_list,
    timeline,
    rich_store,
};

struct MessageViewRuntimeResult {
    MessageViewRuntimeError error;

    constexpr bool ok() const noexcept {
        return error == MessageViewRuntimeError::none;
    }
};

struct MessageViewRuntime {
    MessageStore store;
    UiVariableList list;
    MessageTimeline timeline;
    Arena storage;
    Arena text_storage;
    MessageRecord* records;
    u32* order;
    u32* item_units;
    u64* fenwick;
    bool initialized;
    bool owns_storage;
};

MessageViewRuntimeResult message_view_runtime_initialize(MessageViewRuntime* runtime,
                                                         u32 default_item_units, u64 viewport_units,
                                                         bool follow_bottom) noexcept;

// Rebuilds only the variable-height index/timeline over the current store.
// This is intended for fixture/bootstrap bulk insertion; ordinary Gateway and
// history mutations must flow through MessageTimeline instead.
MessageViewRuntimeResult message_view_runtime_reindex(MessageViewRuntime* runtime, u32 default_item_units,
                                                      u64 viewport_units, bool follow_bottom) noexcept;

MessageViewRuntimeResult message_view_runtime_bind_rich_store(MessageViewRuntime* runtime,
                                                              MessageRichStore* rich_store) noexcept;
void message_view_runtime_unbind_rich_store(MessageViewRuntime* runtime) noexcept;
void message_view_runtime_release(MessageViewRuntime* runtime) noexcept;

static_assert(sizeof(MessageViewRuntime) < 384);

} // namespace maicord

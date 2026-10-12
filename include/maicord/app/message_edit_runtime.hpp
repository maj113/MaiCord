#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/model/message_composer.hpp"

namespace maicord {

enum class MessageEditRequestPhase : u8 {
    idle,
    queued,
    submitting,
    retry_wait,
};

struct MessageEditRuntime {
    StableStorage content_storage;
    StableStorage saved_draft_storage;
    char* content;
    char* saved_draft;
    u64 channel_id;
    u64 message_id;
    u64 retry_at_ms;
    u64 saved_reply_message_id;
    u32 content_bytes;
    u32 saved_draft_bytes;
    u32 saved_draft_anchor;
    u32 saved_draft_caret;
    MessageEditRequestPhase phase;
    bool editor_active;
    bool saved_draft_focused;
    bool saved_reply_mention_suppressed;
};

bool message_edit_runtime_initialize(MessageEditRuntime* runtime) noexcept;
bool message_edit_runtime_reserve(MessageEditRuntime* runtime,
                                  u32 content_bytes,
                                  u32 saved_draft_bytes) noexcept;
void message_edit_runtime_reset(MessageEditRuntime* runtime) noexcept;
void message_edit_runtime_release(MessageEditRuntime* runtime) noexcept;

static_assert(sizeof(MessageEditRuntime) < 256);

} // namespace maicord

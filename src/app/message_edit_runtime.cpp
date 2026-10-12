#include "maicord/app/message_edit_runtime.hpp"

namespace maicord {
namespace {

void erase(char* text, u32 bytes) noexcept {
    volatile char* output = text;
    for (u32 index = 0; output && index < bytes; ++index) {
        output[index] = 0;
    }
}

} // namespace

bool message_edit_runtime_initialize(MessageEditRuntime* runtime) noexcept {
    if (!runtime || runtime->content_storage.arena.base ||
        runtime->saved_draft_storage.arena.base) {
        return false;
    }
    *runtime = MessageEditRuntime{};
    if (!stable_storage_initialize(
            &runtime->content_storage, kMessageComposerMaximumBytes, 0,
            sizeof(char), alignof(char)) ||
        !stable_storage_initialize(
            &runtime->saved_draft_storage, kMessageComposerMaximumBytes, 0,
            sizeof(char), alignof(char))) {
        message_edit_runtime_release(runtime);
        return false;
    }
    runtime->content = stable_storage_data<char>(runtime->content_storage);
    runtime->saved_draft =
        stable_storage_data<char>(runtime->saved_draft_storage);
    return true;
}

bool message_edit_runtime_reserve(MessageEditRuntime* runtime,
                                  u32 content_bytes,
                                  u32 saved_draft_bytes) noexcept {
    if (!runtime || !runtime->content || !runtime->saved_draft ||
        content_bytes > kMessageComposerMaximumBytes ||
        saved_draft_bytes > kMessageComposerMaximumBytes) {
        return false;
    }
    return (content_bytes == 0 ||
            stable_storage_grow(&runtime->content_storage, content_bytes)) &&
           (saved_draft_bytes == 0 ||
            stable_storage_grow(&runtime->saved_draft_storage,
                                saved_draft_bytes));
}

void message_edit_runtime_reset(MessageEditRuntime* runtime) noexcept {
    if (!runtime || !runtime->content || !runtime->saved_draft) {
        return;
    }
    erase(runtime->content, runtime->content_bytes);
    erase(runtime->saved_draft, runtime->saved_draft_bytes);
    const StableStorage content_storage = runtime->content_storage;
    const StableStorage saved_draft_storage = runtime->saved_draft_storage;
    char* const content = runtime->content;
    char* const saved_draft = runtime->saved_draft;
    *runtime = MessageEditRuntime{};
    runtime->content_storage = content_storage;
    runtime->saved_draft_storage = saved_draft_storage;
    runtime->content = content;
    runtime->saved_draft = saved_draft;
}

void message_edit_runtime_release(MessageEditRuntime* runtime) noexcept {
    if (!runtime) {
        return;
    }
    if (runtime->content && runtime->saved_draft) {
        message_edit_runtime_reset(runtime);
    }
    stable_storage_release(&runtime->saved_draft_storage);
    stable_storage_release(&runtime->content_storage);
    *runtime = MessageEditRuntime{};
}

} // namespace maicord

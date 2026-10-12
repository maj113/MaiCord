#pragma once

#include "maicord/app/account_profile.hpp"
#include "maicord/model/message_draft_store.hpp"

namespace maicord {

struct MessageDraftSession {
    u64 profile_id;
    u64 persist_at_ms;
    u32 persisted_revision;
    u32 persist_failures;
    u32 persist_saves;
    u32 persisted_bytes;
    bool cleanup_required;
    // A throttled save is committed by a background writer; the session
    // applies its result on a later persist/wait call. Both fields use
    // existing padding.
    bool persist_in_flight;
    wchar_t path[kAccountProfileMaximumPathCharacters];
    u32 inflight_revision;
};
static_assert(sizeof(MessageDraftSession) == 2088);

bool message_draft_session_configure(MessageDraftSession& session, MessageDraftStore& drafts,
                                     const wchar_t* path, u64 profile_id, u64 now_ms) noexcept;
void message_draft_session_observe(MessageDraftSession& session, const MessageDraftStore& drafts,
                                   u64 now_ms) noexcept;
// Due non-forced saves are queued to the background writer (latest state at
// the deadline wins; edits made while a save is in flight are written by the
// next one). A forced save first waits for any in-flight save, then commits
// synchronously with write-through.
void message_draft_session_persist(MessageDraftSession& session, const MessageDraftStore& drafts,
                                   u64 now_ms, bool force) noexcept;
// Blocks until no save is in flight and applies its result. Returns false on timeout.
bool message_draft_session_wait(MessageDraftSession& session, const MessageDraftStore& drafts, u64 now_ms,
                                u32 timeout_ms) noexcept;

} // namespace maicord

#include "maicord/app/message_draft_session.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/app/message_draft_persistence.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord {
namespace {

inline constexpr u32 kPersistThrottleMs = 500;
inline constexpr u32 kPersistRetryMaximumMs = 30'000;
// While a background save is in flight the scheduler re-polls at this cadence
// instead of spinning on an elapsed deadline.
inline constexpr u32 kPersistPollMs = 25;
inline constexpr u32 kPersistDrainMs = 5'000;

u32 retry_delay(u32 failures) noexcept {
    const u32 shift = minimum(failures == 0 ? 0u : failures - 1u, 5u);
    return minimum(kPersistThrottleMs << shift, kPersistRetryMaximumMs);
}

// One process-wide single-slot writer: the UI thread submits only when no
// save is in flight, so the slot never queues stale snapshots.
struct DraftWriter {
    SrwLock lock;
    HANDLE work_event;
    HANDLE done_event;
    HANDLE thread;
    MessageDraftImage image;
    MessageDraftPersistenceResult result;
    bool job_ready;
    bool job_done;
    wchar_t path[kAccountProfileMaximumPathCharacters];
};

DraftWriter* g_writer = nullptr;
bool g_writer_failed = false;

DWORD WINAPI draft_writer_main(void* context) noexcept {
    auto* writer = static_cast<DraftWriter*>(context);
    for (;;) {
        if (WaitForSingleObject(writer->work_event, INFINITE) != WAIT_OBJECT_0) return 1;
        AcquireSRWLockExclusive(writer->lock.native());
        const bool ready = writer->job_ready;
        MessageDraftImage image = writer->image;
        reset_value<MessageDraftImage>(&writer->image);
        ReleaseSRWLockExclusive(writer->lock.native());
        if (!ready) continue;
        // The UI thread does not touch path while a job is outstanding.
        const MessageDraftPersistenceResult result = message_drafts_commit(writer->path, &image, false);
        AcquireSRWLockExclusive(writer->lock.native());
        writer->result = result;
        writer->job_ready = false;
        writer->job_done = true;
        SetEvent(writer->done_event);
        ReleaseSRWLockExclusive(writer->lock.native());
    }
}

DraftWriter* draft_writer() noexcept {
    if (g_writer || g_writer_failed) return g_writer;
    auto* writer = static_cast<DraftWriter*>(HeapAlloc(process_heap(), HEAP_ZERO_MEMORY, sizeof(DraftWriter)));
    if (writer) {
        writer->work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        writer->done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (writer->work_event && writer->done_event)
            writer->thread = CreateThread(nullptr, 64u * 1024u, draft_writer_main, writer,
                                          STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!writer->thread) {
            if (writer->done_event) CloseHandle(writer->done_event);
            if (writer->work_event) CloseHandle(writer->work_event);
            HeapFree(process_heap(), 0, writer);
            writer = nullptr;
        }
    }
    g_writer_failed = writer == nullptr;
    g_writer = writer;
    return writer;
}

void apply_result(MessageDraftSession& session, const MessageDraftStore& drafts, u32 revision,
                  const MessageDraftPersistenceResult& saved, u64 now_ms) noexcept {
    if (saved.ok()) {
        session.persisted_revision = revision;
        session.persist_at_ms = 0;
        session.persist_failures = 0;
        session.cleanup_required = false;
        ++session.persist_saves;
        message_draft_session_observe(session, drafts, now_ms);
        return;
    }
    ++session.persist_failures;
    session.persist_at_ms = now_ms + retry_delay(session.persist_failures);
    log_u64(LogLevel::warning, "draft", "save_error", static_cast<u64>(saved.error));
    log_u64(LogLevel::warning, "draft", "save_native_error", saved.native_error);
    log_u64(LogLevel::warning, "draft", "save_retry_at_ms", session.persist_at_ms);
}

// Applies a finished background save. With a timeout, waits for it first.
bool collect(MessageDraftSession& session, const MessageDraftStore& drafts, u64 now_ms,
             u32 timeout_ms) noexcept {
    if (!session.persist_in_flight) return true;
    DraftWriter* writer = g_writer;
    if (timeout_ms && WaitForSingleObject(writer->done_event, timeout_ms) != WAIT_OBJECT_0) return false;
    AcquireSRWLockExclusive(writer->lock.native());
    const bool done = writer->job_done;
    const MessageDraftPersistenceResult saved = writer->result;
    if (done) {
        writer->job_done = false;
        ResetEvent(writer->done_event);
    }
    ReleaseSRWLockExclusive(writer->lock.native());
    if (!done) return false;
    session.persist_in_flight = false;
    apply_result(session, drafts, session.inflight_revision, saved, now_ms);
    return true;
}

} // namespace

void message_draft_session_observe(MessageDraftSession& session, const MessageDraftStore& drafts,
                                   u64 now_ms) noexcept {
    if (session.profile_id == 0) {
        return;
    }
    const bool dirty = session.cleanup_required || drafts.revision != session.persisted_revision;
    if (!dirty) {
        session.persist_at_ms = 0;
    } else if (session.persist_at_ms == 0) {
        // The official client's composer bounds draft saves with a 500 ms throttle.
        // Keep the first deadline fixed so continuous typing cannot defer it.
        session.persist_at_ms = now_ms + kPersistThrottleMs;
    }
}

void message_draft_session_persist(MessageDraftSession& session, const MessageDraftStore& drafts,
                                   u64 now_ms, bool force) noexcept {
    if (session.profile_id == 0) {
        return;
    }
    if (!collect(session, drafts, now_ms, force ? kPersistDrainMs : 0)) {
        // Still in flight; a forced save cannot overtake an older commit.
        session.persist_at_ms = now_ms + kPersistPollMs;
        return;
    }
    message_draft_session_observe(session, drafts, now_ms);
    const bool dirty = session.cleanup_required || drafts.revision != session.persisted_revision;
    if (!dirty || (!force && session.persist_at_ms > now_ms)) {
        return;
    }
    DraftWriter* writer = force ? nullptr : draft_writer();
    if (writer) {
        MessageDraftImage image{};
        const MessageDraftPersistenceResult encoded =
            message_drafts_encode(session.profile_id, drafts, &image);
        if (!encoded.ok()) {
            apply_result(session, drafts, drafts.revision, encoded, now_ms);
            return;
        }
        u32 characters = 0;
        while (session.path[characters] != 0) ++characters;
        AcquireSRWLockExclusive(writer->lock.native());
        if (writer->job_ready) {
            // Only reachable when a discarded session's commit is still running.
            ReleaseSRWLockExclusive(writer->lock.native());
            message_drafts_image_release(&image);
            session.persist_at_ms = now_ms + kPersistPollMs;
            return;
        }
        for (u32 index = 0; index <= characters; ++index) writer->path[index] = session.path[index];
        writer->image = image;
        writer->job_ready = true;
        // A completion nobody collected (its session was discarded) must not
        // be mistaken for this job's.
        writer->job_done = false;
        ResetEvent(writer->done_event);
        SetEvent(writer->work_event);
        ReleaseSRWLockExclusive(writer->lock.native());
        session.persist_in_flight = true;
        session.inflight_revision = drafts.revision;
        session.persist_at_ms = now_ms + kPersistPollMs;
        return;
    }
    apply_result(session, drafts, drafts.revision,
                 message_drafts_save_file(session.path, session.profile_id, drafts), now_ms);
}

bool message_draft_session_wait(MessageDraftSession& session, const MessageDraftStore& drafts, u64 now_ms,
                                u32 timeout_ms) noexcept {
    return collect(session, drafts, now_ms, maximum(timeout_ms, 1u));
}

bool message_draft_session_configure(MessageDraftSession& session, MessageDraftStore& drafts,
                                     const wchar_t* path, u64 profile_id, u64 now_ms) noexcept {
    if (!path || profile_id == 0) {
        return false;
    }
    u32 characters = 0;
    while (characters < kAccountProfileMaximumPathCharacters && path[characters] != 0) {
        ++characters;
    }
    if (characters == 0 || characters >= kAccountProfileMaximumPathCharacters) {
        return false;
    }
    // A pending commit belongs to the current scope; finish it before any
    // reload or scope change can reinterpret its revision.
    if (!collect(session, drafts, now_ms, kPersistDrainMs)) {
        return false;
    }
    bool same_scope = session.profile_id == profile_id;
    for (u32 index = 0; same_scope && index <= characters; ++index)
        same_scope = session.path[index] == path[index];
    if (!same_scope) {
        // A missing or invalid store for another profile must not retain the
        // previous profile's text or reply targets.
        if (!message_draft_store_reset(&drafts)) return false;
        reset_value(&session);
    }
    for (u32 index = 0; index <= characters; ++index) {
        session.path[index] = path[index];
    }
    session.profile_id = profile_id;
    const MessageDraftPersistenceResult loaded =
        message_drafts_load_file(session.path, profile_id, &drafts);
    if (loaded.ok()) {
        session.persisted_revision = drafts.revision;
        session.cleanup_required = loaded.discarded != 0;
        log_u64(LogLevel::info, "draft", "loaded_records", loaded.records);
        log_u64(LogLevel::info, "draft", "discarded_blank_records", loaded.discarded);
        log_u64(LogLevel::info, "draft", "loaded_bytes", loaded.bytes);
        message_draft_session_observe(session, drafts, now_ms);
        return true;
    }
    if ((loaded.error == MessageDraftPersistenceError::open &&
         win32_error_is_missing_file(loaded.native_error)) ||
        loaded.error == MessageDraftPersistenceError::version) {
        log_write(LogLevel::info, "draft", "store_absent");
        message_draft_session_observe(session, drafts, now_ms);
        return true;
    }
    log_u64(LogLevel::warning, "draft", "load_error", static_cast<u64>(loaded.error));
    log_u64(LogLevel::warning, "draft", "load_native_error", loaded.native_error);
    message_draft_session_observe(session, drafts, now_ms);
    return true;
}

} // namespace maicord

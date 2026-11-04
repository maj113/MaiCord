#include "maicord/app/settings/client_preferences_writer.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/win32_process.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/os/thread.hpp"

namespace maicord {

struct ClientPreferencesWriter {
    SrwLock lock;
    Waitable work_event;
    Waitable completion_event;
    Thread* thread;
    ClientPreferences pending;
    ClientPreferencesResult completed_result;
    u64 requested_revision;
    u64 completed_revision;
    bool pending_ready;
    Atomic32 completion_ready;
    bool stopping;
    bool explicit_path;
    wchar_t path[kAccountProfileMaximumPathCharacters];
};

static_assert(sizeof(ClientPreferencesWriter) <= 2304);

namespace {
bool reject(u32 error) noexcept {
    SetLastError(error);
    return false;
}

void release_writer(ClientPreferencesWriter* writer) noexcept {
    if (writer->thread) thread_close(writer->thread);
    if (writer->completion_event) waitable_close(writer->completion_event);
    if (writer->work_event) waitable_close(writer->work_event);
    heap_free(writer);
}

u32 write_preferences(void* context) noexcept {
    auto* writer = static_cast<ClientPreferencesWriter*>(context);
    for (;;) {
        ClientPreferences preferences{};
        u64 revision = 0;
        AcquireSRWLockExclusive(writer->lock.native());
        if (writer->pending_ready) {
            preferences = writer->pending;
            revision = writer->requested_revision;
            writer->pending_ready = false;
        } else if (writer->stopping) {
            ReleaseSRWLockExclusive(writer->lock.native());
            return 0;
        }
        ReleaseSRWLockExclusive(writer->lock.native());
        if (revision == 0) {
            if (wait_one(writer->work_event, INFINITE) != WAIT_OBJECT_0) return 1;
            continue;
        }

        // Only the worker calls persistence, including directory creation,
        // write-through, flush and atomic commit. Stop cannot cancel a write.
        const ClientPreferencesResult result = writer->explicit_path
            ? client_preferences_save_file(writer->path, preferences)
            : client_preferences_save(preferences);
        AcquireSRWLockExclusive(writer->lock.native());
        writer->completed_revision = revision;
        writer->completed_result = result;
        write_release(&writer->completion_ready, 1);
        event_set(writer->completion_event);
        ReleaseSRWLockExclusive(writer->lock.native());
    }
}
} // namespace

bool client_preferences_writer_create(ClientPreferencesWriter** output,
                                      const wchar_t* path) noexcept {
    if (!output || *output) return reject(ERROR_INVALID_PARAMETER);
    const u32 length = wide_length(path, kAccountProfileMaximumPathCharacters);
    if (path && (length == 0 || length + 4 >= kAccountProfileMaximumPathCharacters))
        return reject(ERROR_INVALID_PARAMETER);
    auto* writer = static_cast<ClientPreferencesWriter*>(
        heap_allocate(sizeof(ClientPreferencesWriter), true));
    if (!writer) return reject(ERROR_NOT_ENOUGH_MEMORY);
    writer->explicit_path = path != nullptr;
    for (u32 i = 0; i < length; ++i) writer->path[i] = path[i];
    writer->work_event = event_create(false, false);
    if (writer->work_event)
        writer->completion_event = event_create(true, false);
    if (!writer->work_event || !writer->completion_event) {
        const u32 error = thread_last_error();
        release_writer(writer);
        return reject(error);
    }
    writer->thread = thread_start(write_preferences, writer, 64u * 1024u, nullptr);
    if (!writer->thread) {
        const u32 error = thread_last_error();
        release_writer(writer);
        return reject(error);
    }
    *output = writer;
    return true;
}

bool client_preferences_writer_submit(ClientPreferencesWriter* writer,
                                      const ClientPreferences& preferences,
                                      u64* revision) noexcept {
    if (!writer || !revision || !client_preferences_valid(preferences))
        return reject(ERROR_INVALID_PARAMETER);
    AcquireSRWLockExclusive(writer->lock.native());
    if (writer->stopping) {
        ReleaseSRWLockExclusive(writer->lock.native());
        return reject(ERROR_OPERATION_ABORTED);
    }
    writer->pending = preferences;
    writer->pending_ready = true;
    *revision = ++writer->requested_revision;
    ReleaseSRWLockExclusive(writer->lock.native());
    // The worker rechecks pending_ready under the lock before it waits, and the event stays
    // signaled until it does.
    event_set(writer->work_event);
    return true;
}

HANDLE client_preferences_writer_completion_event(const ClientPreferencesWriter* writer) noexcept {
    return writer ? writer->completion_event : nullptr;
}

bool client_preferences_writer_poll(ClientPreferencesWriter* writer,
                                    u64* completed_revision,
                                    ClientPreferencesResult* result,
                                    bool* ready) noexcept {
    if (!writer || !completed_revision || !result || !ready)
        return reject(ERROR_INVALID_PARAMETER);
    // The main loop polls on every wake; only a published completion needs the lock. The worker
    // sets the completion event after publishing, so a poll that misses it sees the event later.
    *ready = read_acquire(&writer->completion_ready) != 0;
    if (!*ready)
        return true;
    AcquireSRWLockExclusive(writer->lock.native());
    *completed_revision = writer->completed_revision;
    *result = writer->completed_result;
    write_release(&writer->completion_ready, 0);
    event_reset(writer->completion_event);
    ReleaseSRWLockExclusive(writer->lock.native());
    return true;
}

bool client_preferences_writer_stop(ClientPreferencesWriter** output, u32 timeout_ms) noexcept {
    if (!output) return reject(ERROR_INVALID_PARAMETER);
    auto* writer = *output;
    if (!writer) return true;
    AcquireSRWLockExclusive(writer->lock.native());
    writer->stopping = true;
    event_set(writer->work_event);
    ReleaseSRWLockExclusive(writer->lock.native());
    if (!thread_wait(writer->thread, timeout_ms))
        return reject(WAIT_TIMEOUT);
    release_writer(writer);
    *output = nullptr;
    return true;
}

} // namespace maicord

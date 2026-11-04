// The overlapped file reads and writes of maicord/platform/net/network.hpp on Linux. A regular file cannot be
// polled, so a small process-wide pool of threads serves the one operation each NetworkFile has pending with
// pread/pwrite and posts its completion to the loop, which is how Windows delivers an overlapped file operation.
//
// The pool works on the descriptor it was handed (`job_handle`). Closing a file whose operation is pending leaves
// that descriptor open and closes it when the completion is polled: closing under a running call would let the
// number be reused by an unrelated open.
#include "internal.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"

namespace maicord {
namespace {

using namespace net_detail;

constexpr u32 kPoolThreads = 2;
constexpr u32 kPoolStackBytes = 128 * 1024;

struct Pool {
    SRWLOCK lock;
    NetworkOperation* head;
    NetworkOperation* tail;
    bool started;
    Waitable wake;
};

Pool g_pool;

struct Locked {
    Locked() noexcept {
        AcquireSRWLockExclusive(&g_pool.lock);
    }
    ~Locked() noexcept {
        ReleaseSRWLockExclusive(&g_pool.lock);
    }
};

NetworkOperation* dequeue() noexcept {
    Locked locked;
    NetworkOperation* operation = g_pool.head;
    if (operation) {
        g_pool.head = operation->next;
        if (!g_pool.head) {
            g_pool.tail = nullptr;
        }
        operation->next = nullptr;
    }
    return operation;
}

void serve(NetworkFile* file) noexcept {
    NetworkOperation& operation = file->operation;
    NetworkLoop* loop = file->loop;
    u32 done = 0;
    bool ok = false;
    i32 error = ERROR_OPERATION_ABORTED;
    if (!__atomic_load_n(&file->cancelled, __ATOMIC_ACQUIRE)) {
        ok = operation.kind == NetworkOperationKind::file_read
                 ? file_read_at(file->job_handle, file->offset, operation.buffer.buf, operation.buffer.len, &done)
                 : file_write_at(file->job_handle, file->offset, operation.buffer.buf, operation.buffer.len, &done);
        error = ok ? 0 : static_cast<i32>(thread_last_error());
        if (__atomic_load_n(&file->cancelled, __ATOMIC_ACQUIRE)) {
            ok = false;
            error = ERROR_OPERATION_ABORTED;
        }
    }
    // The file belongs to the loop's thread again once its completion is posted.
    post(loop, &operation, ok, done, error);
}

u32 pool_main(void*) noexcept {
    for (;;) {
        wait_one(g_pool.wake, 0xFFFFFFFFu);
        while (NetworkOperation* operation = dequeue()) {
            serve(static_cast<NetworkFile*>(operation->owner));
        }
    }
}

// Starts the pool's threads on first use; false when they cannot be made.
bool start_pool() noexcept {
    Locked locked;
    if (g_pool.started) {
        return true;
    }
    g_pool.wake = event_create(false, false);
    if (!g_pool.wake) {
        return false;
    }
    for (u32 index = 0; index < kPoolThreads; ++index) {
        Thread* thread = thread_start(pool_main, nullptr, kPoolStackBytes, nullptr);
        if (!thread) {
            if (index == 0) {
                waitable_close(g_pool.wake);
                g_pool.wake = nullptr;
                return false;
            }
            break;
        }
        thread_close(thread);
    }
    g_pool.started = true;
    return true;
}

NetworkResult fail(i32 error, NetworkError kind) noexcept {
    return failure(kind, error);
}

// Queues the operation for the pool. False when no thread can serve it.
bool submit(NetworkFile* file) noexcept {
    if (!start_pool()) {
        return false;
    }
    {
        Locked locked;
        file->operation.next = nullptr;
        if (g_pool.tail) {
            g_pool.tail->next = &file->operation;
        } else {
            g_pool.head = &file->operation;
        }
        g_pool.tail = &file->operation;
    }
    event_set(g_pool.wake);
    return true;
}

bool start_operation(NetworkFile* file, NetworkOperationKind kind, u64 offset, Slice<u8> buffer) noexcept {
    reset_value(&file->operation);
    file->operation.owner = file;
    file->operation.kind = kind;
    file->operation.generation = file->generation;
    file->operation.buffer = {buffer.count, reinterpret_cast<char*>(buffer.data)};
    file->offset = offset;
    file->job_handle = file->handle;
    __atomic_store_n(&file->cancelled, 0u, __ATOMIC_RELEASE);
    file->pending = true;
    if (!submit(file)) {
        file->pending = false;
        return false;
    }
    return true;
}

} // namespace

namespace net_detail {

void translate_file(NetworkOperation* operation, NetworkPollResult* result) noexcept {
    auto* file = static_cast<NetworkFile*>(operation->owner);
    NetworkCompletion& completion = result->completion;
    completion.transferred = operation->transferred;
    completion.native_error = operation->native_error;
    completion.kind = operation->kind;
    completion.succeeded = operation->succeeded;
    completion.file = file;
    completion.stale = !file || operation->generation != file->generation;
    if (!completion.stale) {
        file->pending = false;
        if (file->close_when_done) {
            file_close(file->job_handle);
            file->close_when_done = false;
        }
    }
}

} // namespace net_detail

NetworkResult network_file_open(NetworkLoop* loop, NetworkFile* file, const wchar_t* path) noexcept {
    if (!loop || !loop->ready || !file || file->handle || file->pending || !path || !path[0]) {
        return fail(ERROR_INVALID_PARAMETER, NetworkError::invalid_argument);
    }
    const FileHandle handle =
        file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, kFileSequential | kFileOverlapped});
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(static_cast<i32>(thread_last_error()), NetworkError::file_open);
    }
    FileSnapshot information{};
    u32 native_error = 0;
    if (!file_snapshot_read(handle, &information, &native_error)) {
        file_close(handle);
        return fail(static_cast<i32>(native_error), NetworkError::file_open);
    }
    file->handle = handle;
    file->bytes = information.bytes;
    file->identity = information.identity;
    file->loop = loop;
    file->close_when_done = false;
    if (++file->generation == 0) {
        ++file->generation;
    }
    return success();
}

NetworkResult network_file_read(NetworkFile* file, u64 offset, Slice<u8> buffer) noexcept {
    if (!file || !file->handle || file->pending || !buffer.data || !buffer.count || offset >= file->bytes ||
        buffer.count > file->bytes - offset) {
        return fail(ERROR_INVALID_PARAMETER, NetworkError::invalid_argument);
    }
    if (!start_operation(file, NetworkOperationKind::file_read, offset, buffer)) {
        return fail(ERROR_NOT_ENOUGH_MEMORY, NetworkError::file_read);
    }
    return success();
}

void network_file_close(NetworkFile* file) noexcept {
    if (!file || !file->handle) {
        return;
    }
    if (file->pending) {
        __atomic_store_n(&file->cancelled, 1u, __ATOMIC_RELEASE);
        file->close_when_done = true;
    } else {
        file_close(file->handle);
    }
    file->handle = nullptr;
}

NetworkResult network_file_create(NetworkLoop* loop, NetworkFile* file, const wchar_t* path) noexcept {
    if (!loop || !loop->ready || !file || file->handle || file->pending || !path || !path[0]) {
        return fail(ERROR_INVALID_PARAMETER, NetworkError::invalid_argument);
    }
    const FileHandle handle = file_open(path, {FileAccess::write, FileCreate::create_new, 0,
                                               kFileSequential | kFileOverlapped});
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(static_cast<i32>(thread_last_error()), NetworkError::file_open);
    }
    file->handle = handle;
    file->bytes = 0;
    reset_value(&file->identity);
    file->loop = loop;
    file->close_when_done = false;
    if (++file->generation == 0) {
        ++file->generation;
    }
    return success();
}

NetworkResult network_file_write(NetworkFile* file, u64 offset, ConstSlice<u8> buffer) noexcept {
    if (!file || !file->handle || file->pending || !buffer.data || !buffer.count || offset > 0x7fffffffffffffffull ||
        buffer.count > 0x7fffffffffffffffull - offset) {
        return fail(ERROR_INVALID_PARAMETER, NetworkError::invalid_argument);
    }
    if (!start_operation(file, NetworkOperationKind::file_write, offset,
                         Slice<u8>{const_cast<u8*>(buffer.data), buffer.count})) {
        return fail(ERROR_NOT_ENOUGH_MEMORY, NetworkError::file_write);
    }
    return success();
}

} // namespace maicord

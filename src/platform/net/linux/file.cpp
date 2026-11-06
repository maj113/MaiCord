// The overlapped file reads and writes of maicord/platform/net/network.hpp on Linux: read and write requests of the
// loop's ring, which the kernel serves inline when the page cache allows and from its own workers when it does not,
// so no thread of ours is involved. A request that moves only part of its buffer asks for the rest (see finish in
// network.cpp), so an operation completes with everything or with the error.
//
// The request works on the descriptor it was handed (`job_handle`). Closing a file whose operation is pending cancels
// the request and leaves the descriptor open; it is closed when the completion is polled, since closing a descriptor
// whose entry has not been submitted yet would let the number be reused by an unrelated open.
#include "internal.hpp"
#include "maicord/os/file.hpp"

namespace maicord {
namespace {

using namespace net_detail;

NetworkResult fail(i32 error, NetworkError kind) noexcept {
    return failure(kind, error);
}

void start_operation(NetworkFile* file, NetworkOperationKind kind, u64 offset, Slice<u8> buffer) noexcept {
    reset_value(&file->operation);
    file->operation.owner = file;
    file->operation.kind = kind;
    file->operation.generation = file->generation;
    file->operation.buffer = {buffer.count, reinterpret_cast<char*>(buffer.data)};
    file->offset = offset;
    file->job_handle = file->handle;
    file->pending = true;
    start(file->loop, &file->operation);
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
    start_operation(file, NetworkOperationKind::file_read, offset, buffer);
    return success();
}

void network_file_close(NetworkFile* file) noexcept {
    if (!file || !file->handle) {
        return;
    }
    if (file->pending) {
        file->close_when_done = true;
        cancel_operation(file->loop, &file->operation);
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
    start_operation(file, NetworkOperationKind::file_write, offset, Slice<u8>{const_cast<u8*>(buffer.data), buffer.count});
    return success();
}

} // namespace maicord

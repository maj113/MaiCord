#include "maicord/core/win32_process.hpp"
#include "network_native.hpp"

namespace maicord {
NetworkResult network_file_open(NetworkLoop* loop, NetworkFile* file, const wchar_t* path) noexcept {
    if (!loop || !loop->completion_port || !file || file->handle || file->pending || !path || !path[0])
        return {ERROR_INVALID_PARAMETER, NetworkError::invalid_argument};
    const HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {static_cast<i32>(thread_last_error()), NetworkError::file_open};
    FileSnapshot information{};
    u32 native_error = 0;
    if (!file_snapshot_read(handle, &information, &native_error)) {
        CloseHandle(handle);
        return {static_cast<i32>(native_error), NetworkError::file_open};
    }
    if (!CreateIoCompletionPort(handle, loop->completion_port, reinterpret_cast<ULONG_PTR>(file), 0)) {
        const i32 error = static_cast<i32>(thread_last_error());
        CloseHandle(handle);
        return {error, NetworkError::file_open};
    }
    file->handle = handle;
    file->bytes = information.bytes;
    file->identity = information.identity;
    if (++file->generation == 0) ++file->generation;
    return {};
}
NetworkResult network_file_read(NetworkFile* file, u64 offset, Slice<u8> buffer) noexcept {
    if (!file || !file->handle || file->pending || !buffer.data || !buffer.count ||
        offset >= file->bytes || buffer.count > file->bytes - offset)
        return {ERROR_INVALID_PARAMETER, NetworkError::invalid_argument};
    file->operation = {};
    file->operation.owner = file;
    file->operation.kind = NetworkOperationKind::file_read;
    file->operation.generation = file->generation;
    file->operation.overlapped.Offset = static_cast<DWORD>(offset);
    file->operation.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    file->pending = true;
    if (!ReadFile(file->handle, buffer.data, buffer.count, nullptr, native(&file->operation.overlapped))) {
        const i32 error = static_cast<i32>(thread_last_error());
        if (error != ERROR_IO_PENDING) {
            file->pending = false;
            return {error, NetworkError::file_read};
        }
    }
    // Synchronous completion also posts an IOCP packet; consume it before
    // touching the buffer or reusing this OVERLAPPED.
    return {};
}
void network_file_close(NetworkFile* file) noexcept {
    if (!file || !file->handle) return;
    if (file->pending) CancelIoEx(file->handle, native(&file->operation.overlapped));
    CloseHandle(file->handle);
    file->handle = nullptr;
}
NetworkResult network_file_create(NetworkLoop* loop, NetworkFile* file, const wchar_t* path) noexcept {
    if (!loop || !loop->completion_port || !file || file->handle || file->pending || !path || !path[0])
        return {ERROR_INVALID_PARAMETER, NetworkError::invalid_argument};
    const HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN | FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {static_cast<i32>(thread_last_error()), NetworkError::file_open};
    file->handle = handle; file->bytes = 0; file->identity = {};
    if (++file->generation == 0) ++file->generation;
    if (!CreateIoCompletionPort(handle, loop->completion_port, reinterpret_cast<ULONG_PTR>(file), 0)) {
        return {static_cast<i32>(thread_last_error()), NetworkError::file_open};
    }
    return {};
}
NetworkResult network_file_write(NetworkFile* file, u64 offset, ConstSlice<u8> buffer) noexcept {
    if (!file || !file->handle || file->pending || !buffer.data || !buffer.count ||
        offset > 0x7fffffffffffffffull || buffer.count > 0x7fffffffffffffffull - offset)
        return {ERROR_INVALID_PARAMETER, NetworkError::invalid_argument};
    file->operation = {}; file->operation.owner = file;
    file->operation.kind = NetworkOperationKind::file_write; file->operation.generation = file->generation;
    file->operation.overlapped.Offset = static_cast<DWORD>(offset);
    file->operation.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    file->pending = true;
    if (!WriteFile(file->handle, buffer.data, buffer.count, nullptr, native(&file->operation.overlapped))) {
        const auto error = static_cast<i32>(thread_last_error());
        if (error != ERROR_IO_PENDING) { file->pending = false; return {error, NetworkError::file_write}; }
    }
    return {};
}
}


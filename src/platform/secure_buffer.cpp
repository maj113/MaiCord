#include "maicord/core/win32_process.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {

SecureBufferResult success(u32 bytes = 0) noexcept {
    return SecureBufferResult{bytes, 0, SecureBufferError::none};
}

SecureBufferResult failure(SecureBufferError error, u32 native_error = 0, u32 bytes = 0) noexcept {
    return SecureBufferResult{bytes, native_error, error};
}

// VirtualLock's process-wide quota is the minimum working set less an
// undocumented overhead. Only recover that specific failure; ordinary buffers
// keep the original one-call path and never change working-set policy.
constexpr SIZE_T kLockRecoveryMinimumLimit = 1024 * 1024;
constexpr u32 kLockRecoveryAttempts = 16;
// A DPAPI blob is the plaintext plus a few hundred bytes; this leaves headroom.
constexpr u32 kProtectedOverheadGuess = 1024;

struct LockBudgetGuard {
    HANDLE mutex;
    ~LockBudgetGuard() noexcept {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
};

u32 acquire_lock_budget(HANDLE* acquired) noexcept {
    *acquired = nullptr;
    // A PID-qualified named mutex serializes recovery without static state or a
    // persistent heap allocation. It is recursive, so unprotect can hold it
    // while lock_pages takes it again. Every handle is noninheritable and closed.
    wchar_t name[64] = L"Local\\MaiCord.SecureLockBudget.";
    u32 used = 0;
    while (name[used]) ++used;
    const u32 id = process_id();
    for (u32 digit = 0; digit < 8; ++digit) {
        const u32 value = (id >> ((7 - digit) * 4)) & 15;
        name[used++] = static_cast<wchar_t>(value < 10 ? L'0' + value : L'a' + value - 10);
    }
    name[used] = 0;
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
    if (!mutex) return thread_last_error();
    const DWORD waited = WaitForSingleObject(mutex, 1000);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        const u32 error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : thread_last_error();
        CloseHandle(mutex);
        return error;
    }
    *acquired = mutex;
    return 0;
}

u32 lock_pages(void* address, u32 bytes) noexcept {
    if (VirtualLock(address, bytes)) return 0;
    u32 error = thread_last_error();
    if (error != ERROR_WORKING_SET_QUOTA) return error;
    HANDLE mutex = nullptr;
    const u32 acquired = acquire_lock_budget(&mutex);
    if (acquired) return acquired;
    LockBudgetGuard guard{mutex};

    // Another image/thread may already have recovered the quota. Abandoned
    // ownership is also safe: no local accounting state needs reconstruction.
    if (VirtualLock(address, bytes)) return 0;
    error = thread_last_error();
    if (error != ERROR_WORKING_SET_QUOTA) return error;

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const SIZE_T page = system.dwPageSize;
    if (!page) return ERROR_INVALID_DATA;
    const SIZE_T offset = reinterpret_cast<SIZE_T>(address) % page;
    const SIZE_T span = ((offset + bytes + page - 1) / page) * page;
    SIZE_T growth = span;
    for (u32 attempt = 0; attempt < kLockRecoveryAttempts; ++attempt) {
        SIZE_T minimum = 0, maximum = 0;
        DWORD flags = 0;
        if (!GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minimum, &maximum, &flags))
            return thread_last_error();
        // Preserve the host's maximum and hard/soft enforcement flags. Never
        // lower a value on failure or release: other threads can have locked
        // pages against the raised quota, including during the initial fast path.
        const SIZE_T ceiling = maximum < kLockRecoveryMinimumLimit ? maximum : kLockRecoveryMinimumLimit;
        if (minimum >= ceiling) return error;
        const SIZE_T available = ceiling - minimum;
        const SIZE_T next = minimum + (growth < available ? growth : available);
        if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(), next, maximum, flags))
            return thread_last_error();
        if (VirtualLock(address, bytes)) return 0;
        error = thread_last_error();
        if (error != ERROR_WORKING_SET_QUOTA) return error;
        // Retries cover the overhead and quota that a concurrent fast-path
        // lock took after this raise: one page, then doubling to the cap.
        growth = attempt ? growth * 2 : page;
    }
    return error;
}

} // namespace

SecureBufferResult secure_buffer_initialize(SecureBuffer* buffer, u32 capacity) noexcept {
    if (!buffer || capacity == 0) {
        return failure(SecureBufferError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    reset_value<SecureBuffer>(buffer);
    auto* allocation =
        static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation) {
        return failure(SecureBufferError::allocation_failed, thread_last_error());
    }
    const u32 native_error = lock_pages(allocation, capacity);
    if (native_error != 0) {
        SecureZeroMemory(allocation, capacity);
        VirtualFree(allocation, 0, MEM_RELEASE);
        return failure(SecureBufferError::lock_failed, native_error);
    }
    buffer->data = allocation;
    buffer->capacity = capacity;
    buffer->locked = true;
    return success();
}

SecureBufferResult secure_buffer_assign(SecureBuffer* buffer, ConstSlice<u8> value) noexcept {
    if (!buffer || !buffer->data || !buffer->locked || (!value.data && value.count != 0)) {
        return failure(SecureBufferError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    if (value.count > buffer->capacity) {
        return failure(SecureBufferError::output_too_small, 0, value.count);
    }
    SecureZeroMemory(buffer->data, buffer->capacity);
    copy_bytes(buffer->data, value.data, value.count);
    buffer->bytes = value.count;
    return success(value.count);
}

bool secure_buffer_transfer(SecureBuffer* destination, SecureBuffer* source) noexcept {
    if (!destination || !source || destination == source || destination->data || destination->locked ||
        !source->data || !source->locked || source->capacity == 0 || source->bytes == 0) {
        return false;
    }
    *destination = *source;
    reset_value<SecureBuffer>(source);
    return true;
}

void secure_buffer_clear(SecureBuffer* buffer) noexcept {
    if (buffer && buffer->data) {
        SecureZeroMemory(buffer->data, buffer->capacity);
        buffer->bytes = 0;
    }
}

void secure_buffer_release(SecureBuffer* buffer) noexcept {
    if (!buffer) {
        return;
    }
    if (buffer->data) {
        SecureZeroMemory(buffer->data, buffer->capacity);
        if (buffer->locked) {
            VirtualUnlock(buffer->data, buffer->capacity);
        }
        VirtualFree(buffer->data, 0, MEM_RELEASE);
    }
    reset_value<SecureBuffer>(buffer);
}

ConstSlice<u8> secure_buffer_view(const SecureBuffer& buffer) noexcept {
    return ConstSlice<u8>{buffer.data, buffer.bytes};
}

SecureBufferResult secure_buffer_protect_user_bound(const SecureBuffer& plaintext, ConstSlice<u8> entropy,
                                                    Slice<u8> protected_output) noexcept {
    if (!plaintext.data || !plaintext.locked || plaintext.bytes == 0 ||
        (!protected_output.data && protected_output.count != 0) || (!entropy.data && entropy.count != 0)) {
        return failure(SecureBufferError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    DATA_BLOB input{plaintext.bytes, plaintext.data};
    DATA_BLOB entropy_blob{entropy.count, const_cast<u8*>(entropy.data)};
    DATA_BLOB* entropy_pointer = entropy.count == 0 ? nullptr : &entropy_blob;
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"MaiCord credential", entropy_pointer, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return failure(SecureBufferError::protect_failed, thread_last_error());
    }
    const u32 required = output.cbData;
    SecureBufferResult result = success(required);
    if (required > protected_output.count) {
        result = failure(SecureBufferError::output_too_small, 0, required);
    } else {
        copy_bytes(protected_output.data, output.pbData, required);
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

SecureBufferResult secure_buffer_protect_user_bound_block(const SecureBuffer& plaintext, ConstSlice<u8> entropy,
                                                          u32 header_bytes, u32 maximum_bytes, u8** block,
                                                          u32* block_bytes) noexcept {
    if (!block || !block_bytes || header_bytes == 0 || header_bytes >= maximum_bytes) {
        return failure(SecureBufferError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    *block = nullptr;
    *block_bytes = 0;
    const u32 maximum_protected = maximum_bytes - header_bytes;
    // DPAPI adds a few hundred bytes (description, GUIDs, salt, MAC, padding) to the plaintext:
    // guess that, and retry once with the reported size when the guess was short.
    u32 capacity = plaintext.bytes + kProtectedOverheadGuess;
    if (capacity > maximum_protected) {
        capacity = maximum_protected;
    }
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        const u32 total = header_bytes + capacity;
        auto* bytes = static_cast<u8*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!bytes) {
            return failure(SecureBufferError::allocation_failed, thread_last_error());
        }
        const SecureBufferResult result =
            secure_buffer_protect_user_bound(plaintext, entropy, Slice<u8>{bytes + header_bytes, capacity});
        if (result.ok() && result.bytes != 0) {
            *block = bytes;
            *block_bytes = total;
            return result;
        }
        SecureZeroMemory(bytes, total);
        VirtualFree(bytes, 0, MEM_RELEASE);
        if (result.ok()) {
            return failure(SecureBufferError::protect_failed);
        }
        if (result.error != SecureBufferError::output_too_small || result.bytes <= capacity ||
            result.bytes > maximum_protected) {
            return result;
        }
        capacity = result.bytes;
    }
    return failure(SecureBufferError::protect_failed);
}

SecureBufferResult secure_buffer_unprotect_user_bound(ConstSlice<u8> protected_input,
                                                      ConstSlice<u8> entropy,
                                                      SecureBuffer* plaintext) noexcept {
    if (!protected_input.data || protected_input.count == 0 || !plaintext || !plaintext->data ||
        !plaintext->locked || (!entropy.data && entropy.count != 0)) {
        return failure(SecureBufferError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    DATA_BLOB input{protected_input.count, const_cast<u8*>(protected_input.data)};
    DATA_BLOB entropy_blob{entropy.count, const_cast<u8*>(entropy.data)};
    DATA_BLOB* entropy_pointer = entropy.count == 0 ? nullptr : &entropy_blob;
    DATA_BLOB output{};
    // DPAPI returns LocalAlloc storage, so concurrent plaintext outputs can
    // share a page. VirtualLock is not reference-counted: one temporary's
    // unlock could otherwise unlock another live temporary. Serialize BEFORE
    // decryption until after wipe/unlock/free.
    HANDLE mutex = nullptr;
    const u32 acquired = acquire_lock_budget(&mutex);
    if (acquired) return failure(SecureBufferError::lock_failed, acquired);
    LockBudgetGuard guard{mutex};
    if (!CryptUnprotectData(&input, nullptr, entropy_pointer, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output)) {
        return failure(SecureBufferError::unprotect_failed, thread_last_error());
    }
    SecureBufferResult result{};
    const u32 native_error = output.cbData ? lock_pages(output.pbData, output.cbData) : ERROR_INVALID_DATA;
    const bool locked = native_error == 0;
    if (!locked) {
        result = failure(SecureBufferError::lock_failed, native_error);
    } else {
        result = secure_buffer_assign(plaintext, ConstSlice<u8>{output.pbData, output.cbData});
    }
    SecureZeroMemory(output.pbData, output.cbData);
    if (locked) {
        VirtualUnlock(output.pbData, output.cbData);
    }
    LocalFree(output.pbData);
    return result;
}

} // namespace maicord

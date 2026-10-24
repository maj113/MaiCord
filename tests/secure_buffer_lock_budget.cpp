#include "maicord/core/win32_process.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"
#include "maicord/sys/process_info.hpp"
#include "test_output.hpp"

// QA-only peer: the production implementation is separately linked into this
// image, just as it is in MaiCord. No shipping test seam is introduced.
#if defined(MAICORD_LOCK_BUDGET_PEER_BUILD)
#include "../src/platform/secure_buffer.cpp"
extern "C" __declspec(dllexport) maicord::SecureBufferResult WINAPI
maicord_lock_budget_peer_initialize(maicord::SecureBuffer* buffer, maicord::u32 bytes) noexcept {
    return maicord::secure_buffer_initialize(buffer, bytes);
}
extern "C" __declspec(dllexport) void WINAPI
maicord_lock_budget_peer_release(maicord::SecureBuffer* buffer) noexcept {
    maicord::secure_buffer_release(buffer);
}
#else
namespace maicord::lock_budget_test {
enum class Fault : u32 { real, nonquota, mutex_create, mutex_wait, mutex_timeout,
    query, setter, later_query, later_setter, retry_nonquota, decrypt, ceiling, maximum,
    second_chance, grow, extra_pages, persistent_quota, abandoned };
Fault fault = Fault::real;
u32 lock_calls = 0, query_calls = 0, set_calls = 0, mutex_calls = 0;
u32 close_calls = 0, release_calls = 0, failures = 0;
u32 ownership_depth = 0, maximum_depth = 0, decrypt_calls = 0, unlock_calls = 0;
SIZE_T fake_minimum = 204800, fake_maximum = 1413120;
DWORD fake_flags = QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_ENABLE;
bool policy_preserved = true, wipe_observed = true, inspect_free = false;
bool dpapi_active = false, lifecycle_preserved = true;
void* inspected_allocation = nullptr;
u32 inspected_bytes = 0;

BOOL WINAPI lock(LPVOID address, SIZE_T bytes) noexcept {
    if (fault == Fault::real) return VirtualLock(address, bytes);
    ++lock_calls;
    if (lock_calls == 1 && inspect_free && !inspected_allocation) {
        inspected_allocation = address;
        for (u32 i = 0; i < inspected_bytes; ++i) static_cast<u8*>(address)[i] = 0xa5;
    }
    if ((fault == Fault::second_chance && lock_calls >= 2) ||
        ((fault == Fault::grow || fault == Fault::abandoned) && lock_calls >= 3) ||
        (fault == Fault::extra_pages && lock_calls >= 6)) return TRUE;
    SetLastError((fault == Fault::nonquota || (fault == Fault::retry_nonquota && lock_calls >= 3))
        ? ERROR_NOACCESS : ERROR_WORKING_SET_QUOTA);
    return FALSE;
}
HANDLE WINAPI mutex_create(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name) noexcept {
    if (fault == Fault::real) return CreateMutexW(attributes, owner, name);
    ++mutex_calls;
    policy_preserved = policy_preserved && !attributes && !owner && name && name[0];
    if (fault == Fault::mutex_create) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    return reinterpret_cast<HANDLE>(1);
}
DWORD WINAPI mutex_wait(HANDLE mutex, DWORD timeout) noexcept {
    if (fault == Fault::real) return WaitForSingleObject(mutex, timeout);
    policy_preserved = policy_preserved && mutex == reinterpret_cast<HANDLE>(1) && timeout == 1000;
    if (fault == Fault::mutex_timeout) return WAIT_TIMEOUT;
    if (fault == Fault::mutex_wait) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    ++ownership_depth;
    if (ownership_depth > maximum_depth) maximum_depth = ownership_depth;
    return fault == Fault::abandoned ? WAIT_ABANDONED : WAIT_OBJECT_0;
}
BOOL WINAPI mutex_release(HANDLE mutex) noexcept {
    if (fault == Fault::real) return ReleaseMutex(mutex);
    ++release_calls;
    lifecycle_preserved = lifecycle_preserved && ownership_depth != 0;
    if (ownership_depth) --ownership_depth;
    SetLastError(ERROR_GEN_FAILURE); return TRUE;
}
BOOL WINAPI handle_close(HANDLE handle) noexcept {
    if (fault == Fault::real) return CloseHandle(handle);
    ++close_calls; SetLastError(ERROR_GEN_FAILURE); return TRUE;
}
BOOL WINAPI query(HANDLE process, PSIZE_T minimum, PSIZE_T maximum, PDWORD flags) noexcept {
    if (fault == Fault::real) return GetProcessWorkingSetSizeEx(process, minimum, maximum, flags);
    ++query_calls;
    if (fault == Fault::query || (fault == Fault::later_query && query_calls == 2)) {
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    *minimum = fake_minimum; *maximum = fake_maximum; *flags = fake_flags; return TRUE;
}
BOOL WINAPI set(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD flags) noexcept {
    if (fault == Fault::real) return SetProcessWorkingSetSizeEx(process, minimum, maximum, flags);
    ++set_calls;
    policy_preserved = policy_preserved && minimum > fake_minimum && minimum <= 1024 * 1024 &&
        minimum <= maximum && maximum == fake_maximum && flags == fake_flags;
    if (fault == Fault::setter || (fault == Fault::later_setter && set_calls == 2)) {
        SetLastError(ERROR_PRIVILEGE_NOT_HELD); return FALSE;
    }
    fake_minimum = minimum; return TRUE;
}
BOOL WINAPI free(LPVOID allocation, SIZE_T bytes, DWORD type) noexcept {
    if (inspect_free && allocation == inspected_allocation) {
        for (u32 i = 0; i < inspected_bytes; ++i)
            wipe_observed = wipe_observed && static_cast<u8*>(allocation)[i] == 0;
        inspected_allocation = nullptr;
    }
    const BOOL result = VirtualFree(allocation, bytes, type);
    if (fault != Fault::real) SetLastError(ERROR_GEN_FAILURE);
    return result;
}
HLOCAL WINAPI local_free(HLOCAL allocation) noexcept {
    if (fault != Fault::real && dpapi_active)
        lifecycle_preserved = lifecycle_preserved && ownership_depth == 1;
    if (inspect_free && allocation == inspected_allocation) {
        for (u32 i = 0; i < inspected_bytes; ++i)
            wipe_observed = wipe_observed && static_cast<u8*>(allocation)[i] == 0;
        inspected_allocation = nullptr;
    }
    const HLOCAL result = LocalFree(allocation);
    dpapi_active = false;
    if (fault != Fault::real) SetLastError(ERROR_GEN_FAILURE);
    return result;
}
BOOL WINAPI decrypt(DATA_BLOB* input, LPWSTR* description, DATA_BLOB* entropy, PVOID reserved,
    CRYPTPROTECT_PROMPTSTRUCT* prompt, DWORD flags, DATA_BLOB* output) noexcept {
    if (fault != Fault::real) {
        ++decrypt_calls;
        lifecycle_preserved = lifecycle_preserved && ownership_depth == 1;
        if (fault == Fault::decrypt) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    }
    const BOOL result = CryptUnprotectData(input, description, entropy, reserved, prompt, flags, output);
    if (fault != Fault::real && result) {
        dpapi_active = true;
        if (inspect_free) { inspected_allocation = output->pbData; inspected_bytes = output->cbData; }
    }
    return result;
}
BOOL WINAPI unlock(LPVOID allocation, SIZE_T bytes) noexcept {
    if (fault != Fault::real && dpapi_active) {
        ++unlock_calls;
        lifecycle_preserved = lifecycle_preserved && ownership_depth == 1;
    }
    return VirtualUnlock(allocation, bytes);
}
} // namespace maicord::lock_budget_test

// All Windows declarations were read above. These test-local call wrappers
// exercise the exact source, not a second model of the recovery algorithm.
#define VirtualLock maicord::lock_budget_test::lock
#define CreateMutexW maicord::lock_budget_test::mutex_create
#define WaitForSingleObject maicord::lock_budget_test::mutex_wait
#define ReleaseMutex maicord::lock_budget_test::mutex_release
#define CloseHandle maicord::lock_budget_test::handle_close
#define GetProcessWorkingSetSizeEx maicord::lock_budget_test::query
#define SetProcessWorkingSetSizeEx maicord::lock_budget_test::set
#define VirtualFree maicord::lock_budget_test::free
#define LocalFree maicord::lock_budget_test::local_free
#define CryptUnprotectData maicord::lock_budget_test::decrypt
#define VirtualUnlock maicord::lock_budget_test::unlock
#include "../src/platform/secure_buffer.cpp"
#undef VirtualLock
#undef CreateMutexW
#undef WaitForSingleObject
#undef ReleaseMutex
#undef CloseHandle
#undef GetProcessWorkingSetSizeEx
#undef SetProcessWorkingSetSizeEx
#undef VirtualFree
#undef LocalFree
#undef CryptUnprotectData
#undef VirtualUnlock

namespace maicord::lock_budget_test {
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
void measurement(const char* label, SIZE_T value) noexcept {
    test_write_metric(label, value);
}
bool all_pages_locked(const SecureBuffer& buffer) noexcept {
    if (!buffer.data || !buffer.locked) return false;
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    for (u32 offset = 0; offset < buffer.capacity; offset += system.dwPageSize) {
        MEMORY_BASIC_INFORMATION region{};
        PSAPI_WORKING_SET_EX_INFORMATION page{}; page.VirtualAddress = buffer.data + offset;
        if (VirtualQuery(page.VirtualAddress, &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || region.Type != MEM_PRIVATE || region.Protect != PAGE_READWRITE ||
            !QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page)) ||
            !page.VirtualAttributes.Valid || !page.VirtualAttributes.Locked) return false;
    }
    return true;
}
bool released(void* address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    return address && VirtualQuery(address, &region, sizeof(region)) == sizeof(region) && region.State == MEM_FREE;
}
void real_peak() noexcept {
    SIZE_T initial_minimum = 0, initial_maximum = 0; DWORD initial_flags = 0;
    check(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &initial_minimum, &initial_maximum, &initial_flags),
        "lock budget: initial policy query");
    SecureBuffer ordinary{};
    check(secure_buffer_initialize(&ordinary, 64 * 1024).ok() && all_pages_locked(ordinary),
        "lock budget: ordinary 64KiB arena pages are actually locked");
    SIZE_T ordinary_minimum = 0, ordinary_maximum = 0; DWORD ordinary_flags = 0;
    check(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &ordinary_minimum, &ordinary_maximum, &ordinary_flags) &&
        ordinary_minimum == initial_minimum && ordinary_maximum == initial_maximum && ordinary_flags == initial_flags,
        "lock budget: ordinary initial allocation leaves working-set policy unchanged");
    secure_buffer_release(&ordinary);

    // Reset fixture callers + 104KiB owner + actual REST secure arena:
    // request72 + encrypted32 + record20 + headers32, plus 64KiB UI transient.
    constexpr u32 sizes[] = {72, 32, 12, 4, 4, 20, 4, 104, 156, 64};
    SecureBuffer buffers[10]{}; SIZE_T total = 0;
    for (u32 i = 0; i < 10; ++i) {
        const auto result = secure_buffer_initialize(&buffers[i], sizes[i] * 1024);
        check(result.ok() && all_pages_locked(buffers[i]), "lock budget: every combined reset/REST/UI page is locked");
        if (!result.ok()) measurement("lock_budget_native_error=", result.native_error);
        total += sizes[i] * 1024;
    }
    measurement("lock_budget_combined_requested_bytes=", total);
    SIZE_T peak_minimum = 0, peak_maximum = 0; DWORD peak_flags = 0;
    check(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &peak_minimum, &peak_maximum, &peak_flags) &&
        peak_minimum >= initial_minimum && (peak_minimum <= 1024 * 1024 || peak_minimum == initial_minimum) &&
        peak_maximum == initial_maximum && peak_flags == initial_flags,
        "lock budget: recovery stays within cap and preserves maximum/enforcement flags");
    measurement("lock_budget_peak_minimum=", peak_minimum);
    for (auto& buffer : buffers) {
        void* address = buffer.data; secure_buffer_release(&buffer);
        check(!buffer.data && !buffer.locked && buffer.capacity == 0 && buffer.bytes == 0 && released(address),
            "lock budget: all peak virtual allocations release");
    }
    SIZE_T end_minimum = 0, end_maximum = 0; DWORD end_flags = 0;
    check(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &end_minimum, &end_maximum, &end_flags) &&
        end_minimum == peak_minimum && end_maximum == peak_maximum && end_flags == peak_flags,
        "lock budget: release retains quota high-water policy without retaining allocation");
}

using PeerInitialize = SecureBufferResult (WINAPI*)(SecureBuffer*, u32) noexcept;
using PeerRelease = void (WINAPI*)(SecureBuffer*) noexcept;
struct ThreadContext {
    HANDLE start, ready, finish;
    PeerInitialize initialize;
    PeerRelease release;
    SecureBuffer buffer;
    u32 capacity, error;
    bool ok, freed;
};
DWORD WINAPI thread(void* parameter) noexcept {
    auto& context = *static_cast<ThreadContext*>(parameter);
    if (WaitForSingleObject(context.start, 10000) != WAIT_OBJECT_0) {
        SetEvent(context.ready); return 1;
    }
    const auto result = context.initialize(&context.buffer, context.capacity);
    context.error = result.native_error;
    context.ok = result.ok() && all_pages_locked(context.buffer);
    SetEvent(context.ready);
    WaitForSingleObject(context.finish, INFINITE);
    void* address = context.buffer.data;
    context.release(&context.buffer);
    context.freed = released(address) && !context.buffer.data && !context.buffer.locked;
    return context.ok && context.freed ? 0 : 1;
}
void real_concurrent_images() noexcept {
    HMODULE peer = LoadLibraryW(L"MaiCordSecureBufferLockBudgetPeer.dll");
    check(peer != nullptr, "lock budget: independently linked peer DLL loads");
    if (!peer) return;
    const auto peer_initialize = reinterpret_cast<PeerInitialize>(GetProcAddress(peer, "maicord_lock_budget_peer_initialize"));
    const auto peer_release = reinterpret_cast<PeerRelease>(GetProcAddress(peer, "maicord_lock_budget_peer_release"));
    check(peer_initialize && peer_release, "lock budget: peer has 24-byte SecureBuffer ABI exports");
    if (!peer_initialize || !peer_release) { FreeLibrary(peer); return; }
    SecureBuffer baseline{};
    check(secure_buffer_initialize(&baseline, 128 * 1024).ok(), "lock budget: concurrent baseline allocation");
    HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finish = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ThreadContext contexts[4]{}; HANDLE threads[4]{};
    bool setup = start && finish;
    for (u32 i = 0; i < 4; ++i) {
        contexts[i].start = start; contexts[i].finish = finish;
        contexts[i].ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        contexts[i].initialize = i == 3 ? peer_initialize :
            +[](SecureBuffer* value, u32 bytes) noexcept -> SecureBufferResult { return secure_buffer_initialize(value, bytes); };
        contexts[i].release = i == 3 ? peer_release :
            +[](SecureBuffer* value) noexcept { secure_buffer_release(value); };
        contexts[i].capacity = (i == 3 ? 256 : 128) * 1024;
        threads[i] = setup && contexts[i].ready ? CreateThread(nullptr, 0, thread, &contexts[i], 0, nullptr) : nullptr;
        setup = setup && contexts[i].ready && threads[i];
    }
    check(setup, "lock budget: concurrent EXE/DLL fixture creates synchronization resources");
    if (start) SetEvent(start);
    for (u32 i = 0; i < 4; ++i) if (threads[i]) {
        check(WaitForSingleObject(contexts[i].ready, 10000) == WAIT_OBJECT_0 && contexts[i].ok &&
            all_pages_locked(contexts[i].buffer), "lock budget: concurrent EXE/DLL buffers remain locked simultaneously");
        if (!contexts[i].ok) measurement("lock_budget_concurrent_native_error=", contexts[i].error);
    }
    check(all_pages_locked(baseline), "lock budget: concurrent recovery retains earlier caller lock");
    measurement("lock_budget_concurrent_requested_bytes=", 768 * 1024);
    if (finish) SetEvent(finish);
    for (u32 i = 0; i < 4; ++i) {
        if (threads[i]) {
            const DWORD joined = WaitForSingleObject(threads[i], 10000);
            check(joined == WAIT_OBJECT_0 && contexts[i].freed, "lock budget: joined EXE/DLL threads release allocations");
            // Never unload an image whose worker has failed to join.
            if (joined != WAIT_OBJECT_0) ExitProcess(1);
            CloseHandle(threads[i]);
        }
        if (contexts[i].ready) CloseHandle(contexts[i].ready);
    }
    if (start) CloseHandle(start); if (finish) CloseHandle(finish);
    secure_buffer_release(&baseline); FreeLibrary(peer);
}

void reset_fault(Fault value, u32 bytes = 4096) noexcept {
    fault = value; lock_calls = query_calls = set_calls = mutex_calls = close_calls = release_calls = 0;
    ownership_depth = maximum_depth = decrypt_calls = unlock_calls = 0;
    fake_minimum = value == Fault::ceiling ? 1024 * 1024 : 204800;
    fake_maximum = value == Fault::maximum ? fake_minimum : 1413120;
    fake_flags = QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_ENABLE;
    policy_preserved = wipe_observed = true; inspect_free = true;
    dpapi_active = false; lifecycle_preserved = true;
    inspected_allocation = nullptr; inspected_bytes = bytes;
}
void failed_allocation(Fault value, u32 expected, u32 closes, u32 releases) noexcept {
    reset_fault(value);
    SecureBuffer buffer{};
    const auto result = secure_buffer_initialize(&buffer, inspected_bytes);
    check(result.error == SecureBufferError::lock_failed && result.native_error == expected &&
        !buffer.data && !buffer.capacity && !buffer.bytes && !buffer.locked,
        "lock budget: controlled failure preserves native error and empty owner");
    check(wipe_observed && !inspected_allocation, "lock budget: controlled failure erases every requested byte before free");
    check(close_calls == closes && release_calls == releases && policy_preserved && lifecycle_preserved && !ownership_depth,
        "lock budget: controlled failure closes exactly the acquired handle/ownership");
    if (value == Fault::nonquota) check(lock_calls == 1 && !mutex_calls && !query_calls && !set_calls,
        "lock budget: nonquota failure never enters policy recovery");
    if (value == Fault::ceiling || value == Fault::maximum) check(!set_calls,
        "lock budget: absolute cap and existing maximum forbid policy growth");
    if (value == Fault::persistent_quota) check(set_calls == 9 && lock_calls == 11 && fake_minimum == 1024 * 1024,
        "lock budget: doubling recovery reaches the cap in 9 sets/11 locks");
}
void controlled_failures() noexcept {
    failed_allocation(Fault::nonquota, ERROR_NOACCESS, 0, 0);
    failed_allocation(Fault::mutex_create, ERROR_ACCESS_DENIED, 0, 0);
    failed_allocation(Fault::mutex_wait, ERROR_INVALID_HANDLE, 1, 0);
    failed_allocation(Fault::mutex_timeout, ERROR_TIMEOUT, 1, 0);
    failed_allocation(Fault::query, ERROR_ACCESS_DENIED, 1, 1);
    failed_allocation(Fault::setter, ERROR_PRIVILEGE_NOT_HELD, 1, 1);
    failed_allocation(Fault::later_query, ERROR_ACCESS_DENIED, 1, 1);
    failed_allocation(Fault::later_setter, ERROR_PRIVILEGE_NOT_HELD, 1, 1);
    failed_allocation(Fault::retry_nonquota, ERROR_NOACCESS, 1, 1);
    failed_allocation(Fault::ceiling, ERROR_WORKING_SET_QUOTA, 1, 1);
    failed_allocation(Fault::maximum, ERROR_WORKING_SET_QUOTA, 1, 1);
    failed_allocation(Fault::persistent_quota, ERROR_WORKING_SET_QUOTA, 1, 1);
    const Fault successes[] = {Fault::second_chance, Fault::grow, Fault::extra_pages, Fault::abandoned};
    for (Fault value : successes) {
        reset_fault(value, 4097); SecureBuffer source{}, destination{};
        check(secure_buffer_initialize(&source, inspected_bytes).ok(), "lock budget: controlled recheck/growth succeeds");
        const u8 synthetic[] = {1, 2, 3};
        check(secure_buffer_assign(&source, {synthetic, 3}).ok() && secure_buffer_transfer(&destination, &source) &&
            !source.data && destination.bytes == 3 && destination.capacity == inspected_bytes,
            "lock budget: quota recovery preserves assign and ownership transfer");
        const u32 before_release = set_calls;
        secure_buffer_release(&destination);
        check(set_calls == before_release && policy_preserved && close_calls == 1 && release_calls == 1 && wipe_observed &&
            lifecycle_preserved && !ownership_depth,
            "lock budget: release wipes transferred allocation without lowering process quota");
        const u32 expected_sets = value == Fault::second_chance ? 0 : value == Fault::extra_pages ? 4 : 1;
        check(set_calls == expected_sets && fake_minimum == 204800 +
            (value == Fault::second_chance ? 0 : 8192 + (value == Fault::extra_pages ? 7 * 4096 : 0)),
            "lock budget: page-rounded initial span plus 1/2/4-page retries");
    }
    fault = Fault::real; inspect_free = false;
}
void dpapi_failure() noexcept {
    SecureBuffer input{}, output{}; u8 protected_bytes[8192]{};
    const u8 synthetic[] = {'f', 'i', 'x', 't', 'u', 'r', 'e'};
    const bool initialized = secure_buffer_initialize(&input, 4096).ok() && secure_buffer_initialize(&output, 4096).ok();
    check(initialized && secure_buffer_assign(&input, {synthetic, sizeof(synthetic)}).ok(),
        "lock budget: DPAPI synthetic fixture initializes");
    if (initialized) {
        const auto encrypted = secure_buffer_protect_user_bound(input, {}, {protected_bytes, sizeof(protected_bytes)});
        check(encrypted.ok(), "lock budget: DPAPI fixture protects synthetic bytes");
        if (encrypted.ok()) {
            const u8 sentinel[] = {0x7e, 0x53, 0x19};
            secure_buffer_assign(&output, {sentinel, sizeof(sentinel)});
            reset_fault(Fault::setter, sizeof(synthetic));
            const auto rejected = secure_buffer_unprotect_user_bound({protected_bytes, encrypted.bytes}, {}, &output);
            check(rejected.error == SecureBufferError::lock_failed && rejected.native_error == ERROR_PRIVILEGE_NOT_HELD &&
                output.bytes == sizeof(sentinel) && bytes_equal(output.data, sentinel, sizeof(sentinel)) &&
                set_calls == 1 && close_calls == 2 && release_calls == 2 && decrypt_calls == 1 && maximum_depth == 2 &&
                wipe_observed && !inspected_allocation && lifecycle_preserved && !ownership_depth,
                "lock budget: DPAPI recursive recovery failure retains destination, native error, ownership and full wipe");
            reset_fault(Fault::grow, sizeof(synthetic));
            const auto recovered = secure_buffer_unprotect_user_bound({protected_bytes, encrypted.bytes}, {}, &output);
            check(recovered.ok() && output.bytes == sizeof(synthetic) && bytes_equal(output.data, synthetic, sizeof(synthetic)) &&
                set_calls == 1 && close_calls == 2 && release_calls == 2 && maximum_depth == 2 && unlock_calls == 1 &&
                wipe_observed && !inspected_allocation && lifecycle_preserved && !ownership_depth,
                "lock budget: DPAPI recursive quota success holds outer ownership through copy/wipe/unlock/free");
            const Fault before_decrypt[] = {Fault::mutex_create, Fault::mutex_wait, Fault::mutex_timeout, Fault::decrypt};
            for (Fault value : before_decrypt) {
                reset_fault(value); secure_buffer_assign(&output, {sentinel, sizeof(sentinel)});
                const auto failed = secure_buffer_unprotect_user_bound({protected_bytes, encrypted.bytes}, {}, &output);
                const u32 expected_error = value == Fault::mutex_create ? ERROR_ACCESS_DENIED :
                    value == Fault::mutex_wait ? ERROR_INVALID_HANDLE : value == Fault::mutex_timeout ? ERROR_TIMEOUT : ERROR_INVALID_DATA;
                check(failed.error == (value == Fault::decrypt ? SecureBufferError::unprotect_failed : SecureBufferError::lock_failed) &&
                    failed.native_error == expected_error && output.bytes == sizeof(sentinel) &&
                    bytes_equal(output.data, sentinel, sizeof(sentinel)) &&
                    !lock_calls && !set_calls && lifecycle_preserved && !ownership_depth &&
                    close_calls == (value == Fault::mutex_create ? 0u : 1u) &&
                    release_calls == (value == Fault::decrypt ? 1u : 0u) &&
                    decrypt_calls == (value == Fault::decrypt ? 1u : 0u),
                    "lock budget: DPAPI acquisition/decryption failure makes no plaintext copy and releases ownership");
            }
            fault = Fault::real; inspect_free = false;
            check(secure_buffer_unprotect_user_bound({protected_bytes, encrypted.bytes}, {}, &output).ok() &&
                output.bytes == sizeof(synthetic) && bytes_equal(output.data, synthetic, sizeof(synthetic)) && all_pages_locked(output),
                "lock budget: DPAPI normal decrypt remains locked and copies synthetic plaintext");
        }
    }
    fault = Fault::real; inspect_free = false;
    secure_buffer_release(&input); secure_buffer_release(&output);
    SecureZeroMemory(protected_bytes, sizeof(protected_bytes));
}
void run() noexcept {
    static_assert(sizeof(SecureBuffer) == 24);
    real_peak(); real_concurrent_images(); controlled_failures(); dpapi_failure();
    measurement("lock_budget_failures=", failures);
}
} // namespace maicord::lock_budget_test
extern "C" __declspec(noreturn) void WINAPI maicord_secure_buffer_lock_budget_entry() noexcept {
    maicord::lock_budget_test::run();
    ExitProcess(maicord::lock_budget_test::failures ? 1 : 0);
}
#endif

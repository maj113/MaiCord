#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_session_store.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline constexpr u8 kMagic[8] = {'M', 'A', 'I', 'C', 'U', 'S', 'E', 'S'};
inline constexpr u32 kHeaderBytes = 40;
inline constexpr u32 kEntropyBytes = 32;
inline constexpr u32 kLegacyPayloadHeaderBytes = 32;
inline constexpr u32 kPayloadHeaderBytes = 40;
inline constexpr u32 kFlagResume = 1u << 0;
inline constexpr u32 kKnownFlags = kFlagResume;
inline constexpr u32 kPayloadCapacity =
    kPayloadHeaderBytes + kGatewaySessionIdCapacity - 1 + kGatewayResumeUrlCapacity - 1;

AccountSessionStoreResult success(u32 bytes = 0) noexcept {
    return AccountSessionStoreResult{bytes, 0, AccountSessionStoreError::none};
}

AccountSessionStoreResult failure(AccountSessionStoreError error, u32 native_error = 0,
                                  u32 bytes = 0) noexcept {
    return AccountSessionStoreResult{bytes, native_error, error};
}

bool snapshot_valid(const GatewayUserSessionSnapshot& snapshot) noexcept {
    return snapshot.profile_id != 0 && ((snapshot.has_resume && gateway_resume_validate(snapshot.resume)) ||
                                        (!snapshot.has_resume && !snapshot.resume.valid));
}

void make_entropy(u64 profile_id, u32 flags, u32 version, u8* output) noexcept {
    copy_bytes(output, kMagic, sizeof(kMagic));
    byte_store_le32(output + 8, version);
    byte_store_le32(output + 12, flags);
    byte_store_le64(output + 16, profile_id);
    byte_store_le64(output + 24, 0);
}

bool encode(const GatewayUserSessionSnapshot& snapshot, SecureBuffer* payload) noexcept {
    if (!snapshot_valid(snapshot) || !secure_buffer_initialize(payload, kPayloadCapacity).ok()) {
        return false;
    }
    const u32 flags = snapshot.has_resume ? kFlagResume : 0;
    const u32 session_bytes = snapshot.has_resume ? snapshot.resume.session_id_bytes : 0;
    const u32 url_bytes = snapshot.has_resume ? snapshot.resume.resume_url_bytes : 0;
    const u32 bytes = kPayloadHeaderBytes + session_bytes + url_bytes;
    byte_store_le32(payload->data, kUserAccountSessionStoreVersion);
    byte_store_le32(payload->data + 4, flags);
    byte_store_le64(payload->data + 8, snapshot.profile_id);
    byte_store_le64(payload->data + 16, snapshot.has_resume ? snapshot.resume.sequence : 0);
    byte_store_le32(payload->data + 24, session_bytes);
    byte_store_le32(payload->data + 28, url_bytes);
    byte_store_le64(payload->data + 32, snapshot.self_user_id);
    copy_bytes(payload->data + kPayloadHeaderBytes, snapshot.resume.session_id, session_bytes);
    copy_bytes(payload->data + kPayloadHeaderBytes + session_bytes, snapshot.resume.resume_url, url_bytes);
    payload->bytes = bytes;
    return true;
}

bool decode(const SecureBuffer& payload, u64 expected_profile_id, u32 expected_flags, u32 version,
            GatewayUserSessionSnapshot* snapshot) noexcept {
    const u32 header_bytes = version == 1 ? kLegacyPayloadHeaderBytes : kPayloadHeaderBytes;
    if (!snapshot || payload.bytes < header_bytes ||
        byte_load_le32(payload.data) != version ||
        byte_load_le32(payload.data + 4) != expected_flags ||
        byte_load_le64(payload.data + 8) != expected_profile_id) {
        return false;
    }
    const u32 session_bytes = byte_load_le32(payload.data + 24);
    const u32 url_bytes = byte_load_le32(payload.data + 28);
    if (session_bytes >= kGatewaySessionIdCapacity || url_bytes >= kGatewayResumeUrlCapacity ||
        static_cast<u64>(header_bytes) + session_bytes + url_bytes != payload.bytes) {
        return false;
    }
    GatewayUserSessionSnapshot decoded{};
    decoded.profile_id = expected_profile_id;
    decoded.self_user_id = version == 1 ? 0 : byte_load_le64(payload.data + 32);
    if ((expected_flags & kFlagResume) != 0) {
        if (session_bytes == 0 || url_bytes == 0) {
            return false;
        }
        decoded.resume.valid = true;
        decoded.resume.sequence = byte_load_le64(payload.data + 16);
        decoded.resume.session_id_bytes = static_cast<u16>(session_bytes);
        decoded.resume.resume_url_bytes = static_cast<u16>(url_bytes);
        copy_bytes(decoded.resume.session_id, payload.data + header_bytes, session_bytes);
        copy_bytes(decoded.resume.resume_url, payload.data + header_bytes + session_bytes,
                   url_bytes);
        decoded.has_resume = true;
        if (!gateway_resume_validate(decoded.resume)) {
            return false;
        }
    } else if (session_bytes != 0 || url_bytes != 0 || byte_load_le64(payload.data + 16) != 0) {
        return false;
    }
    *snapshot = decoded;
    return true;
}

void write_header(u8* output, u32 total_bytes, u32 protected_bytes, u32 flags, u64 profile_id) noexcept {
    copy_bytes(output, kMagic, sizeof(kMagic));
    byte_store_le32(output + 8, kUserAccountSessionStoreVersion);
    byte_store_le32(output + 12, kHeaderBytes);
    byte_store_le32(output + 16, total_bytes);
    byte_store_le32(output + 20, protected_bytes);
    byte_store_le64(output + 24, profile_id);
    byte_store_le32(output + 32, flags);
    byte_store_le32(output + 36, 0);
}

bool parse_header(ConstSlice<u8> file, u64 expected_profile_id, u32* flags, u32* protected_bytes) noexcept {
    if (!flags || !protected_bytes || file.count < kHeaderBytes) {
        return false;
    }
    for (u32 index = 0; index < sizeof(kMagic); ++index) {
        if (file[index] != kMagic[index]) {
            return false;
        }
    }
    *protected_bytes = byte_load_le32(file.data + 20);
    *flags = byte_load_le32(file.data + 32);
    return byte_load_le32(file.data + 12) == kHeaderBytes && byte_load_le32(file.data + 16) == file.count &&
           *protected_bytes != 0 && *protected_bytes == file.count - kHeaderBytes &&
           byte_load_le64(file.data + 24) == expected_profile_id && (*flags & ~kKnownFlags) == 0 &&
           byte_load_le32(file.data + 36) == 0;
}

} // namespace

AccountSessionStoreResult account_user_session_store_save(
    const wchar_t* path, const GatewayUserSessionSnapshot& snapshot) noexcept {
    const u32 characters =
        wide_length(path, kAccountProfileMaximumPathCharacters);
    if (characters == 0 || characters + 4 >= kAccountProfileMaximumPathCharacters ||
        !snapshot_valid(snapshot)) {
        return failure(AccountSessionStoreError::invalid_input);
    }
    SecureBuffer payload{};
    if (!encode(snapshot, &payload)) {
        secure_buffer_release(&payload);
        return failure(AccountSessionStoreError::memory);
    }
    const u32 flags = snapshot.has_resume ? kFlagResume : 0;
    u8 entropy[kEntropyBytes]{};
    make_entropy(snapshot.profile_id, flags, kUserAccountSessionStoreVersion, entropy);
    const SecureBufferResult required =
        secure_buffer_protect_user_bound(payload, ConstSlice<u8>{entropy, sizeof(entropy)}, {});
    if (required.error != SecureBufferError::output_too_small || required.bytes == 0 ||
        required.bytes > kAccountSessionStoreMaximumFileBytes - kHeaderBytes) {
        secure_clear_bytes(entropy, sizeof(entropy));
        secure_buffer_release(&payload);
        return failure(AccountSessionStoreError::protect, required.native_error, required.bytes);
    }
    const u32 allocation_bytes = kHeaderBytes + required.bytes;
    auto* file_bytes =
        static_cast<u8*>(VirtualAlloc(nullptr, allocation_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!file_bytes) {
        secure_clear_bytes(entropy, sizeof(entropy));
        secure_buffer_release(&payload);
        return failure(AccountSessionStoreError::memory, thread_last_error());
    }
    const SecureBufferResult protected_result =
        secure_buffer_protect_user_bound(payload, ConstSlice<u8>{entropy, sizeof(entropy)},
                                         Slice<u8>{file_bytes + kHeaderBytes, required.bytes});
    secure_clear_bytes(entropy, sizeof(entropy));
    secure_buffer_release(&payload);
    if (!protected_result.ok() || protected_result.bytes == 0 || protected_result.bytes > required.bytes) {
        const AccountSessionStoreResult result = failure(
            AccountSessionStoreError::protect, protected_result.native_error, protected_result.bytes);
        file_persistence_secure_release(file_bytes, allocation_bytes);
        return result;
    }
    const u32 file_size = kHeaderBytes + protected_result.bytes;
    write_header(file_bytes, file_size, protected_result.bytes, flags, snapshot.profile_id);

    const FilePersistenceWriteResult stored = file_persistence_write_atomic(
        path, characters, ConstSlice<u8>{file_bytes, file_size});
    file_persistence_secure_release(file_bytes, allocation_bytes);
    if (!stored.ok()) {
        const AccountSessionStoreError error =
            stored.error == FilePersistenceWriteError::open ? AccountSessionStoreError::open
            : stored.error == FilePersistenceWriteError::write ? AccountSessionStoreError::write
            : stored.error == FilePersistenceWriteError::flush ? AccountSessionStoreError::flush
                                                               : AccountSessionStoreError::commit;
        return failure(error, stored.native_error);
    }
    return success(file_size);
}

AccountSessionStoreResult account_user_session_store_load(const wchar_t* path, u64 expected_profile_id,
                                                          GatewayUserSessionSnapshot* snapshot) noexcept {
    const u32 characters =
        wide_length(path, kAccountProfileMaximumPathCharacters);
    if (characters == 0 || characters >= kAccountProfileMaximumPathCharacters || expected_profile_id == 0 ||
        !snapshot) {
        return failure(AccountSessionStoreError::invalid_input);
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return failure(AccountSessionStoreError::open, thread_last_error());
    }
    Win32LargeInteger size{};
    if (!GetFileSizeEx(file, size.native()) || size.QuadPart < kHeaderBytes ||
        size.QuadPart > kAccountSessionStoreMaximumFileBytes) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(AccountSessionStoreError::size, native_error);
    }
    const u32 file_size = static_cast<u32>(size.QuadPart);
    auto* file_bytes =
        static_cast<u8*>(VirtualAlloc(nullptr, file_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!file_bytes) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(AccountSessionStoreError::memory, native_error);
    }
    u32 native_error = 0;
    const bool read =
        file_persistence_read_complete(file, Slice<u8>{file_bytes, file_size}, &native_error);
    CloseHandle(file);
    if (!read) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountSessionStoreError::read, native_error);
    }
    const u32 version = byte_load_le32(file_bytes + 8);
    if (version != 1 && version != kUserAccountSessionStoreVersion) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountSessionStoreError::version);
    }
    u32 flags = 0;
    u32 protected_bytes = 0;
    if (!parse_header(ConstSlice<u8>{file_bytes, file_size}, expected_profile_id, &flags,
                      &protected_bytes)) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountSessionStoreError::format);
    }
    SecureBuffer payload{};
    const SecureBufferResult initialized = secure_buffer_initialize(&payload, kPayloadCapacity);
    if (!initialized.ok()) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountSessionStoreError::memory, initialized.native_error);
    }
    u8 entropy[kEntropyBytes]{};
    make_entropy(expected_profile_id, flags, version, entropy);
    const SecureBufferResult unprotected =
        secure_buffer_unprotect_user_bound(ConstSlice<u8>{file_bytes + kHeaderBytes, protected_bytes},
                                           ConstSlice<u8>{entropy, sizeof(entropy)}, &payload);
    secure_clear_bytes(entropy, sizeof(entropy));
    file_persistence_secure_release(file_bytes, file_size);
    if (!unprotected.ok()) {
        const u32 error = unprotected.native_error;
        secure_buffer_release(&payload);
        return failure(AccountSessionStoreError::unprotect, error);
    }
    GatewayUserSessionSnapshot decoded{};
    const bool valid = decode(payload, expected_profile_id, flags, version, &decoded);
    secure_buffer_release(&payload);
    if (!valid) {
        return failure(AccountSessionStoreError::format);
    }
    *snapshot = decoded;
    return success(file_size);
}

} // namespace maicord

#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_session_store.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/app/client_session_metadata.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline constexpr u8 kMagic[8] = {'M', 'A', 'I', 'C', 'U', 'S', 'E', 'S'};
inline constexpr u32 kHeaderBytes = 40;
inline constexpr u32 kEntropyBytes = 32;
inline constexpr u32 kLegacyPayloadHeaderBytes = 32;
inline constexpr u32 kPayloadHeaderBytes = 40;
inline constexpr u32 kFlagResume = 1u << 0;
inline constexpr u32 kFlagHeartbeat = 1u << 1;
inline constexpr u32 kFlagCookies = 1u << 2;
inline constexpr u32 kKnownFlags = kFlagResume | kFlagHeartbeat | kFlagCookies;
// Created and last-used times, then the UUID, after the resume strings.
inline constexpr u32 kHeartbeatBytes = 16 + kGatewayUserLaunchIdBytes;
// Last, the persistent cookies: their length, then their record.
inline constexpr u32 kPayloadCapacity = kPayloadHeaderBytes + kGatewaySessionIdCapacity - 1 +
                                        kGatewayResumeUrlCapacity - 1 + kHeartbeatBytes + 4 +
                                        kCookieJarPersistentMaximumBytes;

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

u32 store_flags(const GatewayUserSessionSnapshot& snapshot, const ClientHeartbeatSession* heartbeat,
                bool cookies) noexcept {
    return (snapshot.has_resume ? kFlagResume : 0) | (heartbeat ? kFlagHeartbeat : 0) |
           (cookies ? kFlagCookies : 0);
}

bool encode(const GatewayUserSessionSnapshot& snapshot, const ClientHeartbeatSession* heartbeat,
            CookieJar* cookies, SecureBuffer* payload, u32* flags_output) noexcept {
    if (!snapshot_valid(snapshot) || !secure_buffer_initialize(payload, kPayloadCapacity).ok()) {
        return false;
    }
    const u32 session_bytes = snapshot.has_resume ? snapshot.resume.session_id_bytes : 0;
    const u32 url_bytes = snapshot.has_resume ? snapshot.resume.resume_url_bytes : 0;
    u32 bytes = kPayloadHeaderBytes + session_bytes + url_bytes + (heartbeat ? kHeartbeatBytes : 0);
    const u32 cookie_bytes =
        cookie_jar_has_persistent(cookies, nullptr)
            ? cookie_jar_encode_persistent(cookies, clock_unix_milliseconds(),
                                           {payload->data + bytes + 4, kCookieJarPersistentMaximumBytes})
            : 0;
    const u32 flags = store_flags(snapshot, heartbeat, cookie_bytes != 0);
    if (cookie_bytes) {
        byte_store_le32(payload->data + bytes, cookie_bytes);
        bytes += 4 + cookie_bytes;
    }
    *flags_output = flags;
    byte_store_le32(payload->data, kUserAccountSessionStoreVersion);
    byte_store_le32(payload->data + 4, flags);
    byte_store_le64(payload->data + 8, snapshot.profile_id);
    byte_store_le64(payload->data + 16, snapshot.has_resume ? snapshot.resume.sequence : 0);
    byte_store_le32(payload->data + 24, session_bytes);
    byte_store_le32(payload->data + 28, url_bytes);
    byte_store_le64(payload->data + 32, snapshot.self_user_id);
    copy_bytes(payload->data + kPayloadHeaderBytes, snapshot.resume.session_id, session_bytes);
    copy_bytes(payload->data + kPayloadHeaderBytes + session_bytes, snapshot.resume.resume_url, url_bytes);
    if (heartbeat) {
        u8* record = payload->data + kPayloadHeaderBytes + session_bytes + url_bytes;
        byte_store_le64(record, heartbeat->created_ms);
        byte_store_le64(record + 8, heartbeat->last_used_ms);
        copy_bytes(record + 16, heartbeat->uuid, kGatewayUserLaunchIdBytes);
    }
    payload->bytes = bytes;
    return true;
}

bool decode(const SecureBuffer& payload, u64 expected_profile_id, u32 expected_flags, u32 version,
            GatewayUserSessionSnapshot* snapshot, ClientHeartbeatSession* heartbeat,
            CookieJar* cookies) noexcept {
    const u32 header_bytes = version == 1 ? kLegacyPayloadHeaderBytes : kPayloadHeaderBytes;
    const u32 heartbeat_bytes = (expected_flags & kFlagHeartbeat) ? kHeartbeatBytes : 0;
    const bool has_cookies = (expected_flags & kFlagCookies) != 0;
    if (!snapshot || payload.bytes < header_bytes || byte_load_le32(payload.data) != version ||
        byte_load_le32(payload.data + 4) != expected_flags ||
        byte_load_le64(payload.data + 8) != expected_profile_id || (heartbeat_bytes && version < 3) ||
        (has_cookies && version < 4)) {
        return false;
    }
    const u32 session_bytes = byte_load_le32(payload.data + 24);
    const u32 url_bytes = byte_load_le32(payload.data + 28);
    const u64 cookie_offset = static_cast<u64>(header_bytes) + session_bytes + url_bytes + heartbeat_bytes;
    const u32 cookie_bytes =
        has_cookies && cookie_offset + 4 <= payload.bytes ? byte_load_le32(payload.data + cookie_offset) : 0;
    if (session_bytes >= kGatewaySessionIdCapacity || url_bytes >= kGatewayResumeUrlCapacity ||
        (has_cookies && (!cookie_bytes || cookie_bytes > kCookieJarPersistentMaximumBytes)) ||
        cookie_offset + (has_cookies ? 4ull + cookie_bytes : 0) != payload.bytes) {
        return false;
    }
    ClientHeartbeatSession restored{};
    if (heartbeat_bytes) {
        const u8* record = payload.data + header_bytes + session_bytes + url_bytes;
        restored.created_ms = byte_load_le64(record);
        restored.last_used_ms = byte_load_le64(record + 8);
        copy_bytes(restored.uuid, record + 16, kGatewayUserLaunchIdBytes);
        restored.valid = true;
        if (!client_heartbeat_session_valid(restored))
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
    if (has_cookies && cookies &&
        !cookie_jar_decode_persistent(cookies, {payload.data + cookie_offset + 4, cookie_bytes},
                                      clock_unix_milliseconds()))
        return false;
    *snapshot = decoded;
    *heartbeat = restored;
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
    const wchar_t* path, const GatewayUserSessionSnapshot& snapshot,
    const ClientHeartbeatSession* heartbeat, CookieJar* cookies) noexcept {
    const u32 characters =
        wide_length(path, kAccountProfileMaximumPathCharacters);
    if (characters == 0 || characters + 4 >= kAccountProfileMaximumPathCharacters ||
        !snapshot_valid(snapshot) || (heartbeat && !client_heartbeat_session_valid(*heartbeat))) {
        return failure(AccountSessionStoreError::invalid_input);
    }
    SecureBuffer payload{};
    u32 flags = 0;
    if (!encode(snapshot, heartbeat, cookies, &payload, &flags)) {
        secure_buffer_release(&payload);
        return failure(AccountSessionStoreError::memory);
    }
    u8 entropy[kEntropyBytes]{};
    make_entropy(snapshot.profile_id, flags, kUserAccountSessionStoreVersion, entropy);
    u8* file_bytes = nullptr;
    u32 allocation_bytes = 0;
    const SecureBufferResult protected_result = secure_buffer_protect_user_bound_block(
        payload, ConstSlice<u8>{entropy, sizeof(entropy)}, kHeaderBytes, kAccountSessionStoreMaximumFileBytes,
        &file_bytes, &allocation_bytes);
    secure_clear_bytes(entropy, sizeof(entropy));
    secure_buffer_release(&payload);
    if (!protected_result.ok()) {
        return protected_result.error == SecureBufferError::allocation_failed
                   ? failure(AccountSessionStoreError::memory, protected_result.native_error)
                   : failure(AccountSessionStoreError::protect, protected_result.native_error,
                             protected_result.bytes);
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
                                                          GatewayUserSessionSnapshot* snapshot,
                                                          ClientHeartbeatSession* heartbeat,
                                                          CookieJar* cookies) noexcept {
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
    if (version == 0 || version > kUserAccountSessionStoreVersion) {
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
    ClientHeartbeatSession restored{};
    const bool valid = decode(payload, expected_profile_id, flags, version, &decoded, &restored, cookies);
    secure_buffer_release(&payload);
    if (!valid) {
        return failure(AccountSessionStoreError::format);
    }
    *snapshot = decoded;
    if (heartbeat)
        *heartbeat = restored;
    return success(file_size);
}

namespace {

template <typename T> u64 hash_value(u64 hash, const T& value) noexcept {
    return hash64_bytes(hash, &value, static_cast<u32>(sizeof(value)));
}

// Everything a save writes except the Resume sequence.
u64 save_fingerprint(const GatewayUserSessionSnapshot& snapshot, const ClientHeartbeatSession* heartbeat,
                     u32 cookie_revision) noexcept {
    u64 hash = hash_value(kHash64Seed, cookie_revision);
    if (heartbeat) {
        hash = hash_value(hash, heartbeat->created_ms);
        hash = hash_value(hash, heartbeat->last_used_ms);
        hash = hash64_bytes(hash, heartbeat->uuid, kGatewayUserLaunchIdBytes);
    }
    hash = hash_value(hash, snapshot.profile_id);
    hash = hash_value(hash, snapshot.self_user_id);
    hash = hash_value(hash, snapshot.has_resume);
    if (snapshot.has_resume) {
        hash = hash_value(hash, snapshot.resume.session_id_bytes);
        hash = hash64_bytes(hash, snapshot.resume.session_id, snapshot.resume.session_id_bytes);
        hash = hash_value(hash, snapshot.resume.resume_url_bytes);
        hash = hash64_bytes(hash, snapshot.resume.resume_url, snapshot.resume.resume_url_bytes);
    }
    return hash;
}

u64 save_sequence(const GatewayUserSessionSnapshot& snapshot) noexcept {
    return snapshot.has_resume ? snapshot.resume.sequence : 0;
}

} // namespace

bool account_session_save_state_current(const AccountSessionSaveState& state,
                                        const GatewayUserSessionSnapshot& snapshot,
                                        const ClientHeartbeatSession* heartbeat, u32 cookie_revision,
                                        bool include_sequence) noexcept {
    return state.known && state.fingerprint == save_fingerprint(snapshot, heartbeat, cookie_revision) &&
           (!include_sequence || state.sequence == save_sequence(snapshot));
}

void account_session_save_state_record(AccountSessionSaveState* state,
                                       const GatewayUserSessionSnapshot& snapshot,
                                       const ClientHeartbeatSession* heartbeat, u32 cookie_revision) noexcept {
    state->fingerprint = save_fingerprint(snapshot, heartbeat, cookie_revision);
    state->sequence = save_sequence(snapshot);
    state->known = true;
}

} // namespace maicord

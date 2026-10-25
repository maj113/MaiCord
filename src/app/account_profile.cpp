#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_profile.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline constexpr u8 kProfileMagic[8] = {'M', 'A', 'I', 'C', 'U', 'S', 'E', 'R'};
inline constexpr u32 kProfileHeaderBytes = 48;
inline constexpr u32 kProfileEntropyBytes = 32;

AccountProfileResult success(u32 bytes = 0) noexcept {
    return AccountProfileResult{bytes, 0, AccountProfileError::none};
}

AccountProfileResult failure(AccountProfileError error, u32 native_error = 0, u32 bytes = 0) noexcept {
    return AccountProfileResult{bytes, native_error, error};
}

bool valid_metadata(const AccountProfileMetadata& metadata) noexcept {
    return metadata.profile_id != 0;
}

bool valid_credential(const SecureBuffer& credential) noexcept {
    if (!credential.data || !credential.locked || credential.bytes == 0 ||
        credential.bytes > kAccountProfileMaximumCredentialBytes) {
        return false;
    }
    for (u32 index = 0; index < credential.bytes; ++index) {
        if (credential.data[index] < 0x21 || credential.data[index] > 0x7E) {
            return false;
        }
    }
    return true;
}

void make_entropy(const AccountProfileMetadata& metadata, u8* entropy) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        entropy[index] = kProfileMagic[index];
    }
    byte_store_le32(entropy + 8, kAccountProfileVersion);
    byte_store_le32(entropy + 12, 0);
    byte_store_le32(entropy + 16, 0);
    byte_store_le32(entropy + 20, 0);
    byte_store_le64(entropy + 24, metadata.profile_id);
}

void write_header(u8* output, u32 total_bytes, u32 protected_bytes,
                  const AccountProfileMetadata& metadata) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        output[index] = kProfileMagic[index];
    }
    byte_store_le32(output + 8, kAccountProfileVersion);
    byte_store_le32(output + 12, kProfileHeaderBytes);
    byte_store_le32(output + 16, total_bytes);
    byte_store_le32(output + 20, protected_bytes);
    byte_store_le32(output + 24, 0);
    byte_store_le32(output + 28, 0);
    byte_store_le64(output + 32, metadata.profile_id);
    byte_store_le64(output + 40, 0);
}

bool parse_header(ConstSlice<u8> file, AccountProfileMetadata* metadata, u32* protected_bytes) noexcept {
    if (!metadata || !protected_bytes || file.count < kProfileHeaderBytes) {
        return false;
    }
    for (u32 index = 0; index < 8; ++index) {
        if (file[index] != kProfileMagic[index]) {
            return false;
        }
    }
    if (byte_load_le32(file.data + 12) != kProfileHeaderBytes ||
        byte_load_le32(file.data + 16) != file.count || byte_load_le32(file.data + 24) != 0 ||
        byte_load_le32(file.data + 28) != 0 || byte_load_le64(file.data + 40) != 0) {
        return false;
    }
    const u32 payload_bytes = byte_load_le32(file.data + 20);
    if (payload_bytes == 0 || payload_bytes != file.count - kProfileHeaderBytes) {
        return false;
    }
    AccountProfileMetadata parsed{};
    parsed.profile_id = byte_load_le64(file.data + 32);
    if (!valid_metadata(parsed)) {
        return false;
    }
    *metadata = parsed;
    *protected_bytes = payload_bytes;
    return true;
}

} // namespace

AccountProfileResult account_profile_save(const wchar_t* path, const AccountProfileMetadata& metadata,
                                          const SecureBuffer& credential) noexcept {
    const u32 characters =
        wide_length(path, kAccountProfileMaximumPathCharacters);
    if (characters == 0 || characters + 4 >= kAccountProfileMaximumPathCharacters ||
        !valid_metadata(metadata) || !valid_credential(credential)) {
        return failure(AccountProfileError::invalid_input, ERROR_INVALID_PARAMETER);
    }

    u8 entropy[kProfileEntropyBytes]{};
    make_entropy(metadata, entropy);
    u8* file_bytes = nullptr;
    u32 allocation_bytes = 0;
    const SecureBufferResult protected_result = secure_buffer_protect_user_bound_block(
        credential, ConstSlice<u8>{entropy, kProfileEntropyBytes}, kProfileHeaderBytes,
        kAccountProfileMaximumFileBytes, &file_bytes, &allocation_bytes);
    secure_clear_bytes(entropy, sizeof(entropy));
    if (!protected_result.ok()) {
        return protected_result.error == SecureBufferError::allocation_failed
                   ? failure(AccountProfileError::memory, protected_result.native_error)
                   : failure(AccountProfileError::protect, protected_result.native_error,
                             protected_result.bytes);
    }
    const u32 file_size = kProfileHeaderBytes + protected_result.bytes;
    write_header(file_bytes, file_size, protected_result.bytes, metadata);

    const FilePersistenceWriteResult stored = file_persistence_write_atomic(
        path, characters, ConstSlice<u8>{file_bytes, file_size});
    file_persistence_secure_release(file_bytes, allocation_bytes);
    if (!stored.ok()) {
        const AccountProfileError error =
            stored.error == FilePersistenceWriteError::open ? AccountProfileError::open
            : stored.error == FilePersistenceWriteError::write ? AccountProfileError::write
            : stored.error == FilePersistenceWriteError::flush ? AccountProfileError::flush
                                                               : AccountProfileError::commit;
        return failure(error, stored.native_error);
    }
    return success(file_size);
}

AccountProfileResult account_profile_load(const wchar_t* path, AccountProfile* profile) noexcept {
    const u32 characters =
        wide_length(path, kAccountProfileMaximumPathCharacters);
    if (characters == 0 || characters >= kAccountProfileMaximumPathCharacters || !profile ||
        profile->credential.data) {
        return failure(AccountProfileError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return failure(AccountProfileError::open, thread_last_error());
    }
    Win32LargeInteger size{};
    if (!GetFileSizeEx(file, size.native()) || size.QuadPart < kProfileHeaderBytes ||
        size.QuadPart > kAccountProfileMaximumFileBytes) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(AccountProfileError::size, native_error);
    }
    const u32 file_size = static_cast<u32>(size.QuadPart);
    auto* file_bytes =
        static_cast<u8*>(VirtualAlloc(nullptr, file_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!file_bytes) {
        const u32 native_error = thread_last_error();
        CloseHandle(file);
        return failure(AccountProfileError::memory, native_error);
    }
    u32 native_error = 0;
    const bool read =
        file_persistence_read_complete(file, Slice<u8>{file_bytes, file_size}, &native_error);
    CloseHandle(file);
    if (!read) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountProfileError::read, native_error);
    }
    if (byte_load_le32(file_bytes + 8) != kAccountProfileVersion) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountProfileError::version);
    }
    AccountProfileMetadata metadata{};
    u32 protected_bytes = 0;
    if (!parse_header(ConstSlice<u8>{file_bytes, file_size}, &metadata, &protected_bytes)) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountProfileError::format);
    }
    SecureBuffer credential{};
    const SecureBufferResult initialized =
        secure_buffer_initialize(&credential, kAccountProfileMaximumCredentialBytes);
    if (!initialized.ok()) {
        file_persistence_secure_release(file_bytes, file_size);
        return failure(AccountProfileError::memory, initialized.native_error);
    }
    u8 entropy[kProfileEntropyBytes]{};
    make_entropy(metadata, entropy);
    const SecureBufferResult unprotected = secure_buffer_unprotect_user_bound(
        ConstSlice<u8>{file_bytes + kProfileHeaderBytes, protected_bytes},
        ConstSlice<u8>{entropy, kProfileEntropyBytes}, &credential);
    secure_clear_bytes(entropy, sizeof(entropy));
    file_persistence_secure_release(file_bytes, file_size);
    if (!unprotected.ok() || !valid_credential(credential)) {
        const u32 error = unprotected.native_error;
        secure_buffer_release(&credential);
        return failure(AccountProfileError::unprotect, error);
    }
    AccountProfile loaded{};
    loaded.credential = credential;
    loaded.metadata = metadata;
    *profile = loaded;
    return success(file_size);
}

void account_profile_release(AccountProfile* profile) noexcept {
    if (!profile) {
        return;
    }
    secure_buffer_release(&profile->credential);
    reset_value<AccountProfile>(profile);
}

} // namespace maicord

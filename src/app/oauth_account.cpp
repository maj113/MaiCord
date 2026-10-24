#include "maicord/app/oauth_account.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
namespace {
const wchar_t* filename(OAuthProvider provider) noexcept {
    return provider == OAuthProvider::spotify ? L"spotify-account.bin" : nullptr;
}
struct Locked {
    SecureBuffer value{};
    ~Locked() noexcept { secure_buffer_release(&value); }
};
struct Record {
    u8 encrypted[4096], entropy[kOAuthClientBytes + 32];
    ~Record() noexcept { secure_clear_bytes(this, sizeof(*this)); }
};
ConstSlice<u8> bind(Record& record, Utf8View client, OAuthProvider provider) noexcept {
    if (!oauth_client_valid(provider, client)) return {};
    const auto purpose = utf8_literal("MaiCord.Spotify.refresh.v1:");
    copy_bytes(record.entropy, purpose.data, purpose.bytes);
    copy_bytes(record.entropy + purpose.bytes, client.data, client.bytes);
    return {record.entropy, purpose.bytes + client.bytes};
}
bool empty(const SecureBuffer* b) noexcept {
    return b && !b->data && !b->capacity && !b->bytes && !b->locked;
}
}
bool oauth_account_save(Utf8View client, Utf8View refresh, OAuthProvider provider) noexcept {
    if (!oauth_client_valid(provider, client) || !oauth_token_valid(refresh)) return false;
    Locked plaintext;
    if (!secure_buffer_initialize(&plaintext.value, kOAuthTokenBytes).ok() ||
        !secure_buffer_assign(&plaintext.value, {reinterpret_cast<const u8*>(refresh.data), refresh.bytes}).ok()) return false;
    Record record;
    const auto encrypted = secure_buffer_protect_user_bound(plaintext.value, bind(record, client, provider), {record.encrypted, sizeof(record.encrypted)});
    wchar_t path[1024];
    const auto location = data_path(path, 1024, filename(provider), true);
    return encrypted.ok() && location.ok() &&
        file_persistence_write_atomic(path, location.length, {record.encrypted, encrypted.bytes}).ok();
}
bool oauth_account_load(Utf8View client, SecureBuffer* output, OAuthProvider provider) noexcept {
    if (!empty(output) || !oauth_client_valid(provider, client)) return false;
    wchar_t path[1024];
    if (!data_path(path, 1024, filename(provider)).ok()) return false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Record record; Win32LargeInteger size{}; u32 native{};
    const bool read = GetFileSizeEx(file, size.native()) && size.QuadPart > 0 &&
        size.QuadPart <= static_cast<i64>(sizeof(record.encrypted)) &&
        file_persistence_read_complete(file, {record.encrypted, static_cast<u32>(size.QuadPart)}, &native);
    CloseHandle(file);
    Locked plaintext;
    return read && secure_buffer_initialize(&plaintext.value, kOAuthTokenBytes).ok() &&
        secure_buffer_unprotect_user_bound({record.encrypted, static_cast<u32>(size.QuadPart)}, bind(record, client, provider), &plaintext.value).ok() &&
        oauth_token_valid({reinterpret_cast<char*>(plaintext.value.data), plaintext.value.bytes}) &&
        secure_buffer_transfer(output, &plaintext.value);
}
bool oauth_account_forget(OAuthProvider provider) noexcept {
    if (!oauth_scope(provider).bytes) return false;
    wchar_t path[1024];
    if (!data_path(path, 1024, filename(provider)).ok()) return false;
    if (DeleteFileW(path)) return true;
    const auto error = thread_last_error();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
OAuthAuthorizationResult oauth_account_refresh(OAuthAuthorization* owner, SecureBuffer* output,
    OAuthAuthorizationTransport transport, Utf8View client, OAuthProvider provider) noexcept {
    if (!owner || !empty(output) || !oauth_client_valid(provider, client)) return {OAuthError::invalid_input};
    if (InterlockedCompareExchange(&owner->native_state, 0, 0) == 3) return {OAuthError::cancelled};
    Locked grant;
    if (!oauth_account_load(client, &grant.value, provider)) return {OAuthError::invalid_grant};
    return oauth_authorization_refresh(owner, client,
        {reinterpret_cast<char*>(grant.value.data), grant.value.bytes}, output, transport,
        provider);
}
u32 oauth_account_client(OAuthProvider provider, Slice<char> output) noexcept {
    if (!output.data) return 0;
    if (provider != OAuthProvider::spotify || output.count < 32) return 0;
    wchar_t path[1024];
    if (!data_path(path, 1024, L"spotify-client.txt").ok()) return 0;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    char client[32]; Win32LargeInteger size{}; u32 native{};
    const bool read = GetFileSizeEx(file, size.native()) && size.QuadPart == sizeof(client) &&
        file_persistence_read_complete(file, {reinterpret_cast<u8*>(client), sizeof(client)}, &native);
    CloseHandle(file);
    if (!read || !oauth_client_valid(provider, {client, sizeof(client)})) return 0;
    copy_bytes(output.data, client, sizeof(client));
    return sizeof(client);
}
} // namespace maicord

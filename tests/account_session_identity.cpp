#include "maicord/app/account_session_store.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
// Independent v1 wire fixture, encrypted for the test process's Windows user.
// It intentionally has no authenticated-user field and uses a different profile key.
bool write_legacy(const wchar_t* path, u64 profile_id) noexcept {
    constexpr auto session = utf8_literal("synthetic-legacy-session");
    constexpr auto url = utf8_literal("wss://127.0.0.1:1");
    const u8 magic[8] = {'M', 'A', 'I', 'C', 'U', 'S', 'E', 'S'};
    SecureBuffer payload{};
    if (!secure_buffer_initialize(&payload, 32 + session.bytes + url.bytes).ok())
        return false;
    byte_store_le32(payload.data, 1);
    byte_store_le32(payload.data + 4, 1);
    byte_store_le64(payload.data + 8, profile_id);
    byte_store_le64(payload.data + 16, 42);
    byte_store_le32(payload.data + 24, session.bytes);
    byte_store_le32(payload.data + 28, url.bytes);
    copy_bytes(payload.data + 32, session.data, session.bytes);
    copy_bytes(payload.data + 32 + session.bytes, url.data, url.bytes);
    payload.bytes = payload.capacity;
    u8 entropy[32]{};
    copy_bytes(entropy, magic, 8);
    byte_store_le32(entropy + 8, 1);
    byte_store_le32(entropy + 12, 1);
    byte_store_le64(entropy + 16, profile_id);
    u8 file[4096]{};
    const auto encrypted =
        secure_buffer_protect_user_bound(payload, {entropy, 32}, {file + 40, sizeof(file) - 40});
    secure_buffer_release(&payload);
    SecureZeroMemory(entropy, sizeof(entropy));
    if (!encrypted.ok())
        return false;
    copy_bytes(file, magic, 8);
    byte_store_le32(file + 8, 1);
    byte_store_le32(file + 12, 40);
    byte_store_le32(file + 16, 40 + encrypted.bytes);
    byte_store_le32(file + 20, encrypted.bytes);
    byte_store_le64(file + 24, profile_id);
    byte_store_le32(file + 32, 1);
    HANDLE handle =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = handle != INVALID_HANDLE_VALUE &&
                    WriteFile(handle, file, 40 + encrypted.bytes, &written, nullptr) &&
                    written == 40 + encrypted.bytes;
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    SecureZeroMemory(file, sizeof(file));
    return ok;
}
bool rewrite_version(const wchar_t* path, u32 version) noexcept {
    HANDLE handle =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER offset{};
    offset.QuadPart = 8;
    u8 bytes[4]{};
    byte_store_le32(bytes, version);
    DWORD written = 0;
    const bool ok = SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) &&
                    WriteFile(handle, bytes, sizeof(bytes), &written, nullptr) && written == sizeof(bytes);
    CloseHandle(handle);
    return ok;
}
} // namespace

void test_account_session_identity(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    const DWORD directory_bytes = GetTempPathW(MAX_PATH, directory);
    const bool temporary =
        directory_bytes && directory_bytes < MAX_PATH && GetTempFileNameW(directory, L"mci", 0, path);
    check(temporary, "identity fixture creates an isolated temporary file");
    if (!temporary)
        return;
    constexpr u64 profile_id = 0x123456789abcdef0ull;
    GatewayUserSessionSnapshot legacy{};
    check(write_legacy(path, profile_id) &&
              account_user_session_store_load(path, profile_id, &legacy).ok() && legacy.has_resume &&
              legacy.profile_id == profile_id && legacy.resume.sequence == 42 && !legacy.self_user_id,
          "v1 session retains resume state without mistaking its profile key for a user ID");
    if (!legacy.has_resume) {
        DeleteFileW(path);
        return;
    }
    GatewayUserSessionSnapshot unknown{};
    check(account_user_session_store_save(path, legacy).ok() &&
              account_user_session_store_load(path, profile_id, &unknown).ok() &&
              unknown.has_resume && !unknown.self_user_id && unknown.resume.sequence == 42,
          "v1 upgrade can preserve an unknown identity without inventing one");
    GatewayUserSessionSnapshot current = legacy;
    current.self_user_id = ~u64{0};
    check(account_user_session_store_save(path, current).ok(),
          "legacy session can be upgraded with an authenticated identity");
    GatewayUserSessionSnapshot restored{};
    check(account_user_session_store_load(path, profile_id, &restored).ok() &&
              restored.self_user_id == current.self_user_id && restored.profile_id == profile_id &&
              restored.resume.sequence == 42 && restored.has_resume,
          "v2 protected session round-trips a full-width user ID separately from the profile key");
    GatewayUserSessionSnapshot unchanged = restored;
    check(account_user_session_store_load(path, profile_id + 1, &unchanged).error ==
                  AccountSessionStoreError::format &&
              unchanged.self_user_id == restored.self_user_id &&
              unchanged.profile_id == restored.profile_id,
          "identity-bearing session retains cross-profile rejection and output atomicity");
    check(rewrite_version(path, 1) &&
              account_user_session_store_load(path, profile_id, &unchanged).error ==
                  AccountSessionStoreError::unprotect &&
              unchanged.self_user_id == restored.self_user_id,
          "changing a v2 header to v1 cannot bypass the version-bound DPAPI protection");
    check(rewrite_version(path, 99) &&
              account_user_session_store_load(path, profile_id, &unchanged).error ==
                  AccountSessionStoreError::version &&
              unchanged.self_user_id == restored.self_user_id,
          "future session formats fail without publishing an identity");
    check(account_user_session_store_save(path, current).ok(), "restore v2 fixture after tamper checks");

    // Exercise the actual account worker startup/snapshot/teardown path. Both
    // initial and resume endpoints are loopback; no service request is made.
    const GatewayUserClientConfiguration configuration{
        GatewayUserProperties{utf8_literal("Windows"), utf8_literal("Discord Client"),
                              utf8_literal("stable"), utf8_literal("1.0.9256"), utf8_literal("10.0.19045"),
                              utf8_literal("x64"), utf8_literal("x64"), utf8_literal("en-US"),
                              utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
                              utf8_literal("Agent/1.0 (Windows; native)"), utf8_literal("42.9.0"),
                              utf8_literal("19045"), 603738},
        0, GatewayUserStatus::online, false, false};
    GatewayEndpoint endpoint{};
    const bool endpoint_ready =
        gateway_endpoint_parse_with_policy(utf8_literal("wss://127.0.0.1:1"),
                                           GatewayEndpointPolicy::user_v9_json_zlib_stream,
                                           &endpoint) == GatewayEndpointError::none;
    for (u32 i = 0; i < 2; ++i) {
        const auto& saved = i == 0 ? restored : legacy;
        AccountProfile profile{};
        profile.metadata.profile_id = profile_id;
        const u8 token[] = {'t', 'e', 's', 't'};
        const bool credential = secure_buffer_initialize(&profile.credential, 16).ok() &&
                                secure_buffer_assign(&profile.credential, {token, sizeof(token)}).ok();
        GatewayClient client{};
        if (credential && endpoint_ready)
            gateway_client_start_user(&client, &profile, &configuration, &endpoint, &saved);
        const bool started = client.runtime && client.worker.thread && !profile.credential.data;
        GatewayUserSessionSnapshot captured{};
        check(started && gateway_client_snapshot_user_session(&client, &captured) && captured.has_resume &&
                  captured.self_user_id == saved.self_user_id && captured.profile_id == profile_id &&
                  captured.resume.sequence == saved.resume.sequence,
              "worker restores the session and matching identity before dispatch or READY");
        if (client.runtime || client.worker.thread)
            gateway_client_stop(&client, 5000);
        check(!client.runtime && !client.worker.thread, "restored worker teardown drains its operations");
        account_profile_release(&profile);
    }
    GatewayUserSessionSnapshot empty{};
    empty.profile_id = profile_id;
    check(account_user_session_store_save(path, empty).ok() &&
              account_user_session_store_load(path, profile_id, &restored).ok() && !restored.has_resume &&
              !restored.self_user_id,
          "empty protected sessions do not retain an earlier authenticated identity");
    SecureZeroMemory(&legacy, sizeof(legacy));
    SecureZeroMemory(&current, sizeof(current));
    SecureZeroMemory(&restored, sizeof(restored));
    SecureZeroMemory(&unchanged, sizeof(unchanged));
    SecureZeroMemory(&unknown, sizeof(unknown));
    check(DeleteFileW(path) != FALSE, "identity fixture cleanup removes only its temporary file");
}
} // namespace maicord

#include "maicord/app/account/login/oauth_account.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../../support/test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
constexpr auto client = utf8_literal("0123456789abcdef0123456789abcdef");
constexpr auto first = utf8_literal("first-refresh/+="), second = utf8_literal("second-refresh");
bool matches(const SecureBuffer& value, Utf8View text) noexcept {
    return value.locked && utf8_equal({reinterpret_cast<const char*>(value.data), value.bytes}, text);
}
void persistence() noexcept {
    SecureBuffer output{};
    check(oauth_account_forget() && !oauth_account_load(client, &output) && !output.data,
          "absent grant is signed out without allocating plaintext");
    check(oauth_account_save(client, first) && oauth_account_load(client, &output) && matches(output, first),
          "saved grant survives a fresh locked read");
    auto* previous = output.data;
    check(!oauth_account_load(client, &output) && output.data == previous && matches(output, first),
          "load cannot replace live credentials");
    secure_buffer_release(&output);
    check(!oauth_account_load(utf8_literal("fedcba9876543210fedcba9876543210"), &output) && !output.data,
          "ciphertext is bound to the Spotify client registration");
    check(!oauth_account_save(client, utf8_literal("bad\nvalue")) &&
          oauth_account_load(client, &output) && matches(output, first),
          "invalid replacement preserves the previous grant");
    secure_buffer_release(&output);
    wchar_t path[1024]; const auto location = data_path(path, 1024, L"spotify-account.bin");
    const HANDLE held = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(location.ok() && held != INVALID_HANDLE_VALUE && !oauth_account_save(client, second) &&
          !oauth_account_forget(), "failed atomic commit and failed sign-out report failure");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    check(oauth_account_load(client, &output) && matches(output, first),
          "failed write preserves the original grant");
    secure_buffer_release(&output);
    check(oauth_account_save(client, second) && oauth_account_load(client, &output) && matches(output, second),
          "successful replacement rotates the persisted grant");
    secure_buffer_release(&output);
    const u8 junk[]{1, 2, 3};
    check(file_persistence_write_atomic(path, location.length, {junk, sizeof(junk)}).ok() &&
          !oauth_account_load(client, &output) && !output.data,
          "corrupt storage cannot publish partial credentials");
    check(oauth_account_forget() && oauth_account_forget() && !oauth_account_load(client, &output),
          "sign-out removes persisted authorization and is idempotent");
}
}
}
extern "C" void maicord_oauth_account_entry() noexcept {
    using namespace maicord;
    wchar_t original[1024], isolated[1024];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", original, 1024);
    u32 length = n;
    if (!n || n >= 1024) ExitProcess(1);
    copy_bytes(isolated, original, (n + 1) * sizeof(wchar_t));
    if (!wide_append(isolated, 1024, &length, L"\\spotify-account-tests") || !directory_ensure(isolated) ||
        !SetEnvironmentVariableW(L"LOCALAPPDATA", isolated)) ExitProcess(1);
    persistence();
    SetEnvironmentVariableW(L"LOCALAPPDATA", original);
    ExitProcess(failures ? 1 : 0);
}

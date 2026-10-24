#include "maicord/app/oauth_account.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "test_output.hpp"

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
struct Fake { u32 calls; u16 status; bool cancel; OAuthAuthorization* owner; };
bool contains(ConstSlice<u8> data, Utf8View part) noexcept {
    for (u32 i = 0; i + part.bytes <= data.count; ++i)
        if (bytes_equal(data.data + i, part.data, part.bytes)) return true;
    return false;
}
UpdateTransportError exchange(void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                             UpdateHttpResponse* response) noexcept {
    auto& fake = *static_cast<Fake*>(context); ++fake.calls;
    check(contains(options.body, utf8_literal("grant_type=refresh_token&")) &&
          contains(options.body, utf8_literal("refresh_token=first-refresh%2F%2B%3D")) &&
          !contains(options.body, utf8_literal("client_secret")),
          "demand-driven refresh reads the saved grant without a client secret");
    const auto json = fake.status == 200 ? utf8_literal(R"({"access_token":"access","refresh_token":"rotated","expires_in":3600,"token_type":"Bearer"})")
                                        : utf8_literal(R"({"error":"invalid_grant"})");
    *response = {}; response->status = fake.status;
    if (fake.cancel) oauth_authorization_cancel(fake.owner);
    return request.sink(request.sink_context, {reinterpret_cast<const u8*>(json.data), json.bytes})
        ? UpdateTransportError::none : UpdateTransportError::sink;
}
void refresh() noexcept {
    auto* owner = static_cast<OAuthAuthorization*>(VirtualAlloc(nullptr, sizeof(OAuthAuthorization), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner) { check(false, "refresh test owner"); return; }
    Fake fake{0, 200, false, owner}; const OAuthAuthorizationTransport transport{&fake, exchange};
    SecureBuffer tokens{}, saved{};
    check(oauth_account_refresh(owner, &tokens, transport, client).error == OAuthError::invalid_grant && !tokens.data && !fake.calls,
          "no saved grant means no refresh network request");
    check(oauth_account_save(client, first), "save refresh fixture");
    auto result = oauth_account_refresh(owner, &tokens, transport, client);
    check(result.ok() && fake.calls == 1 && tokens.data && tokens.locked, "saved account refresh yields locked tokens on demand");
    check(oauth_account_load(client, &saved) && matches(saved, first),
          "worker does not persist rotation before caller verifies current sign-in generation");
    secure_buffer_release(&saved); secure_buffer_release(&tokens);
    fake.status = 400;
    result = oauth_account_refresh(owner, &tokens, transport, client);
    check(result.error == OAuthError::invalid_grant && !tokens.data && fake.calls == 2 &&
          oauth_account_load(client, &saved) && matches(saved, first),
          "rejected refresh does not overwrite or delete a possibly newer saved sign-in");
    secure_buffer_release(&saved);
    fake.status = 200; fake.cancel = true;
    check(oauth_account_refresh(owner, &tokens, transport, client).error == OAuthError::cancelled && !tokens.data && fake.calls == 3,
          "cancellation discards refreshed credentials");
    check(oauth_account_refresh(owner, &tokens, transport, client).error == OAuthError::cancelled && !tokens.data && fake.calls == 3,
          "cancelled owner never reissues refresh");
    oauth_authorization_release(owner); VirtualFree(owner, 0, MEM_RELEASE);
    oauth_account_forget();
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
    persistence(); refresh();
    SetEnvironmentVariableW(L"LOCALAPPDATA", original);
    ExitProcess(failures ? 1 : 0);
}

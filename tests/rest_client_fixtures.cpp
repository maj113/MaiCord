// Offline fixtures over the shipping REST owner. Each one reaches the owner's
// private storage and transport callbacks, so they share this translation unit
// and the owner implementation compiles once. Every CTest case names one fixture
// as its argument and runs it in its own process.
#include "reset_rest_storage.cpp"
#include "user_settings_rest_storage.cpp"
#include "account_logout_transport.cpp"
#include "account_bulk_read_transport.cpp"
#include "account_action_captcha_transport.cpp"

namespace maicord {
namespace {
struct RestClientFixture {
    const wchar_t* name;
    u32 (*run)() noexcept;
};
constexpr RestClientFixture rest_client_fixtures[]{
    {L"reset-storage", reset_rest_storage_fixture},
    {L"user-settings-storage", user_settings_storage::user_settings_rest_storage_fixture},
    {L"logout-transport", logout_transport::run},
    {L"bulk-read-transport", bulk_read_transport::run},
    {L"action-captcha-transport", action_captcha_transport::run},
};
// The fixture named by the only argument after the program name, or null.
const RestClientFixture* selected_fixture() noexcept {
    const wchar_t* p = GetCommandLineW();
    bool quoted = false;
    while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
        if (*p == L'"')
            quoted = !quoted;
        ++p;
    }
    while (*p == L' ' || *p == L'\t')
        ++p;
    for (const auto& fixture : rest_client_fixtures) {
        u32 i = 0;
        while (fixture.name[i] && p[i] == fixture.name[i])
            ++i;
        if (!fixture.name[i] && (!p[i] || p[i] == L' ' || p[i] == L'\t'))
            return &fixture;
    }
    return nullptr;
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_rest_client_fixtures_entry() noexcept {
    const auto* fixture = maicord::selected_fixture();
    if (!fixture) {
        maicord::test_write_line("rest client fixtures: unknown fixture argument");
        ExitProcess(2);
    }
    ExitProcess(fixture->run() ? 1u : 0u);
}

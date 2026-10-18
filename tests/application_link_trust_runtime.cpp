// The leaving-site warning flow through the real text form controller: trusted
// links launch at once, others show the warning with its trust checkbox, and
// a checked trust is saved per account so the host opens directly next time.
#include "application_state.hpp"
#include "application_text_form_state.hpp"
#include <Windows.h>
#include <shellapi.h>
#include "maicord/core/memory.hpp"

namespace maicord::link_trust_runtime_test {
HINSTANCE WINAPI shell_execute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) noexcept;
}
#define ShellExecuteW ::maicord::link_trust_runtime_test::shell_execute
#include "../src/app/application_link_trust.cpp"
#undef ShellExecuteW

namespace maicord::link_trust_runtime_test {
using namespace app_detail;
using Check = void (*)(bool, const char*) noexcept;
u32 launches;
wchar_t launched[2048];
HINSTANCE WINAPI shell_execute(HWND, LPCWSTR, LPCWSTR file, LPCWSTR, LPCWSTR, INT) noexcept {
    ++launches;
    u32 i = 0;
    for (; file && file[i] && i + 1 < 2048; ++i)
        launched[i] = file[i];
    launched[i] = 0;
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(42));
}
bool launched_is(const wchar_t* url) noexcept {
    u32 i = 0;
    while (url[i] && launched[i] == url[i])
        ++i;
    return !url[i] && !launched[i];
}
bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i = 0; part.bytes <= text.bytes && i <= text.bytes - part.bytes; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes))
            return true;
    return false;
}
bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
bool click(TextFormElement element) noexcept {
    const auto* node = g_app.text_form ? text_form_node(g_app.text_form->scene, element) : nullptr;
    if (!node)
        return false;
    LRESULT result = 0;
    const LPARAM point = MAKELPARAM(static_cast<short>((node->rectangle.left + node->rectangle.right) * .5f),
                                    static_cast<short>((node->rectangle.top + node->rectangle.bottom) * .5f));
    return application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point, &result) &&
           application_text_form_window_message(WM_LBUTTONUP, 0, point, &result);
}
// Removes the per-profile file and the temporary %LOCALAPPDATA% tree.
void remove_tree(const wchar_t* root, u64 profile) noexcept {
    wchar_t path[512]{};
    if (link_trust_path(profile, path, 512))
        DeleteFileW(path);
    wchar_t directory[512]{};
    u32 n = 0;
    for (; root[n]; ++n)
        directory[n] = root[n];
    const wchar_t* parts[] = {L"\\MaiCord\\accounts", L"\\MaiCord", L""};
    for (const auto* part : parts) {
        u32 at = n;
        for (u32 i = 0; part[i]; ++i)
            directory[at++] = part[i];
        directory[at] = 0;
        RemoveDirectoryW(directory);
    }
}

void run(Check check, UiTextMeasureBackend measure) noexcept {
    wchar_t previous[1024]{};
    const DWORD previous_length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 1024);
    wchar_t root[512]{};
    const DWORD temp = GetTempPathW(400, root);
    constexpr wchar_t leaf[] = L"maicord-link-trust-runtime";
    for (u32 i = 0; i < sizeof(leaf) / sizeof(wchar_t); ++i)
        root[temp + i] = leaf[i];
    wchar_t accounts[512]{};
    u32 n = 0;
    for (; root[n]; ++n)
        accounts[n] = root[n];
    constexpr wchar_t tail[] = L"\\MaiCord\\accounts";
    const bool made = CreateDirectoryW(root, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    for (u32 i = 0; i < 8; ++i)
        accounts[n + i] = tail[i];
    CreateDirectoryW(accounts, nullptr);
    for (u32 i = 0; i < sizeof(tail) / sizeof(wchar_t); ++i)
        accounts[n + i] = tail[i];
    CreateDirectoryW(accounts, nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root);

    static UiTextMeasureRecord records[32];
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(made && g_app.account_ui &&
              ui_text_measure_cache_initialize(&g_app.account_ui->text_measure_cache, {records, 32}, measure,
                                               nullptr),
          "link trust: account fixture");
    if (!g_app.account_ui)
        return;
    g_app.renderer_ready = true;
    g_app.login.visible = false;
    g_app.plugin_account_scope_serial = 9;
    g_app.account.status.profile_id = 0x51;
    trust_profile = 0;

    // A bare link (its text is its URL) and the service's own hosts launch at once.
    launches = 0;
    check(application_link_open(utf8_literal("https://example.com/a"), true) && launches == 1 &&
              launched_is(L"https://example.com/a") && !g_app.text_form &&
              application_link_open(utf8_literal("https://cdn.discordapp.com/x.png"), false) && launches == 2 &&
              !g_app.text_form,
          "link trust: bare links and the service's hosts open without a warning");

    // A masked link to an untrusted host asks first, showing the URL without
    // credentials, the host to trust, Visit Site and Go Back.
    check(application_link_open(utf8_literal("https://user:secret@example.com/b"), false) && launches == 2 &&
              g_app.text_form,
          "link trust: a masked link shows the leaving-site warning");
    if (!g_app.text_form)
        return;
    const auto& content = g_app.text_form->scene.content;
    check(same(content.title, utf8_literal("Leaving Discord")) &&
              contains(content.context, utf8_literal("This link is taking you to the following website")) &&
              contains(content.context, utf8_literal("https://example.com/b")) &&
              !contains(content.context, utf8_literal("secret")) &&
              same(content.option, utf8_literal("Trust example.com links from now on")) && !content.option_checked &&
              same(content.accept, utf8_literal("Visit Site")) && same(content.cancel, utf8_literal("Go Back")) &&
              text_form_focused(g_app.text_form->scene) == TextFormElement::accept,
          "link trust: the warning names the URL and the host, with Visit Site and Go Back");
    check(click(TextFormElement::cancel) && !g_app.text_form && launches == 2 &&
              !link_trust_has_host(*application_link_trust_store(), utf8_literal("example.com")),
          "link trust: Go Back opens nothing and trusts nothing");

    // Checking "Trust example.com" (keyboard) then Visit Site launches the
    // exact URL and saves the host for this account.
    check(application_link_open(utf8_literal("https://user:secret@example.com/b"), false) && g_app.text_form &&
              text_form_focus(&g_app.text_form->scene, TextFormElement::option) &&
              application_text_form_input(UiInputKind::key_down, VK_SPACE, false) &&
              application_text_form_input(UiInputKind::key_up, VK_SPACE, false) && g_app.text_form &&
              g_app.text_form->scene.content.option_checked,
          "link trust: the trust checkbox toggles from the keyboard");
    check(click(TextFormElement::accept) && !g_app.text_form && launches == 3 &&
              launched_is(L"https://user:secret@example.com/b") &&
              link_trust_has_host(*application_link_trust_store(), utf8_literal("example.com")),
          "link trust: Visit Site launches the link and trusts its host");
    check(application_link_open(utf8_literal("https://example.com/c"), false) && launches == 4 && !g_app.text_form &&
              application_link_open(utf8_literal("https://sub.example.com/c"), false) && g_app.text_form,
          "link trust: the trusted host opens directly; other hosts still warn");
    click(TextFormElement::cancel);

    // The trust is per account and survives a reload from disk.
    g_app.account.status.profile_id = 0x52;
    check(application_link_open(utf8_literal("https://example.com/d"), false) && g_app.text_form && launches == 4,
          "link trust: another account does not share the trust");
    click(TextFormElement::cancel);
    g_app.account.status.profile_id = 0x51;
    trust_profile = 0;
    check(application_link_open(utf8_literal("https://example.com/e"), false) && !g_app.text_form && launches == 5,
          "link trust: the trust is reloaded from the account's file");

    // Application protocols ask with their own wording and trust the scheme.
    check(application_link_open(utf8_literal("discord://-/channels/1/2"), true) && g_app.text_form &&
              contains(g_app.text_form->scene.content.context,
                       utf8_literal("This link is trying to open an application on your device")) &&
              same(g_app.text_form->scene.content.option, utf8_literal("Trust discord: links from now on")) &&
              same(g_app.text_form->scene.content.accept, utf8_literal("Open Link")),
          "link trust: application links ask to trust their protocol");
    check(click(TextFormElement::option) && g_app.text_form && g_app.text_form->scene.content.option_checked &&
              click(TextFormElement::accept) && !g_app.text_form && launches == 6 &&
              application_link_open(utf8_literal("discord://-/channels/3"), false) && launches == 7 &&
              !g_app.text_form,
          "link trust: a trusted protocol opens directly");
    check(application_link_open(utf8_literal("not a url"), true) && launches == 7 && !g_app.text_form &&
              application_link_channel_trusted(12345),
          "link trust: invalid links are consumed; unknown channels are not DMs");

    application_text_form_close(false, false);
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    g_app.context_focus = nullptr;
    g_app.account.status.profile_id = 0x52;
    remove_tree(root, 0x52);
    g_app.account.status.profile_id = 0;
    remove_tree(root, 0x51);
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous_length ? previous : nullptr);
    trust_profile = 0;
}
} // namespace maicord::link_trust_runtime_test

namespace maicord::app_detail {
const RelationshipStore* application_friends_relationships() noexcept {
    return nullptr;
}
} // namespace maicord::app_detail

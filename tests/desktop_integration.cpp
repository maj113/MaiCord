#include "maicord/platform/desktop_integration.hpp"
#include "maicord/platform/system_tray.hpp"
#include "maicord/platform/taskbar_attention.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/registry.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
template <u32 N>
ConstSlice<wchar_t> wide(const wchar_t (&text)[N]) noexcept {
    return {text, N - 1};
}
template <u32 N>
bool same(const wchar_t* text, u32 units, const wchar_t (&expected)[N]) noexcept {
    return units == N - 1 && bytes_equal(text, expected, units * sizeof(wchar_t));
}
u32 length(const wchar_t* text) noexcept {
    u32 n = 0;
    while (text[n])
        ++n;
    return n;
}

DesktopLaunch launch_storage;
DesktopForwardMessage received;
bool received_valid;
u32 received_count;
LRESULT CALLBACK forward_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_COPYDATA) {
        ++received_count;
        received_valid = desktop_instance_receive(reinterpret_cast<const COPYDATASTRUCT*>(lparam), &received);
        return received_valid ? TRUE : FALSE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void test_launch(Check check) noexcept {
    auto& l = launch_storage;
    check(desktop_launch_parse(wide(L"\"C:\\Users\\a b\\MaiCord.exe\" --startup"), &l) && l.startup && !l.url_present,
          "desktop launch: startup entry flag");
    check(desktop_launch_parse(wide(L"\"C:\\MaiCord\\MaiCord.exe\" --url -- \"discord://-/invite/abc\""), &l) &&
              !l.startup && l.url_present && same(l.url, l.url_units, L"discord://-/invite/abc"),
          "desktop launch: protocol handler shape --url -- \"%1\"");
    check(desktop_launch_parse(wide(L"MaiCord.exe --url discord:///channels/@me/42"), &l) && l.url_present &&
              same(l.url, l.url_units, L"discord:///channels/@me/42"),
          "desktop launch: --url without separator");
    check(desktop_launch_parse(wide(L"MaiCord.exe DISCORD://-/app --unknown --startup"), &l) && l.url_present &&
              l.startup && same(l.url, l.url_units, L"DISCORD://-/app"),
          "desktop launch: bare link and unknown arguments");
    check(desktop_launch_parse(wide(L"MaiCord.exe \"a\\\"b\" --startup"), &l) && l.startup && !l.url_present,
          "desktop launch: escaped quotes follow argv rules");
    check(!desktop_launch_parse(wide(L"MaiCord.exe --url \"discord://-/app"), &l) &&
              !desktop_launch_parse(wide(L"\"MaiCord.exe --startup"), &l),
          "desktop launch: unbalanced quotes are refused");
    check(desktop_launch_parse(wide(L"MaiCord.exe"), &l) && !l.startup && !l.url_present,
          "desktop launch: plain start");
}

void test_links(Check check) noexcept {
    DesktopLinkTarget t{};
    check(desktop_link_parse(wide(L"discord://-/invite/MaiCord-1"), &t) && t.kind == DesktopLinkKind::invite &&
              t.code_bytes == 9 && bytes_equal(t.code, "MaiCord-1", 9),
          "desktop link: invite code");
    check(desktop_link_parse(wide(L"discord:///channels/@me/123/456?x=1"), &t) && t.kind == DesktopLinkKind::message &&
              t.guild_id == 0 && t.channel_id == 123 && t.message_id == 456,
          "desktop link: DM message link ignores the query");
    check(desktop_link_parse(wide(L"discord://discord.com/channels/7/8"), &t) && t.kind == DesktopLinkKind::channel &&
              t.guild_id == 7 && t.channel_id == 8 && !t.message_id,
          "desktop link: guild channel on the service host");
    check(desktop_link_parse(wide(L"discord://"), &t) && t.kind == DesktopLinkKind::open &&
              desktop_link_parse(wide(L"discord://-/app"), &t) && t.kind == DesktopLinkKind::open,
          "desktop link: bare scheme and app open the window");
    check(!desktop_link_parse(wide(L"discord://evil.example/invite/abc"), &t) &&
              !desktop_link_parse(wide(L"https://discord.com/invite/abc"), &t) &&
              !desktop_link_parse(wide(L"discord://-/invite/a%20b"), &t) &&
              !desktop_link_parse(wide(L"discord://-/channels/0/1"), &t) &&
              !desktop_link_parse(wide(L"discord://-/channels/1/99999999999999999999999"), &t) &&
              !desktop_link_parse(wide(L"discord://-/settings/account"), &t) &&
              !desktop_link_parse(wide(L"discord://-/invite/abc/def"), &t),
          "desktop link: foreign hosts, schemes, escapes, zero/overflow ids and unknown routes are refused");
}

void test_tray(Check check) noexcept {
    TrayInput input{};
    check(tray_icon_state(input) == TrayIconState::normal, "tray state: idle is normal");
    input.unread = true;
    check(tray_icon_state(input) == TrayIconState::unread, "tray state: unread");
    input.connected = true;
    check(tray_icon_state(input) == TrayIconState::connected, "tray state: a call wins over unread");
    input.speaking = true;
    check(tray_icon_state(input) == TrayIconState::speaking, "tray state: speaking");
    input.suppressed = true;
    check(tray_icon_state(input) == TrayIconState::muted, "tray state: suppressed counts as muted");
    input.server_deaf = true;
    check(tray_icon_state(input) == TrayIconState::deafened, "tray state: deafened wins over muted");

    TrayModel model{};
    TrayInput idle{};
    auto plan = tray_plan(&model, idle, true);
    check(plan.add && !plan.update && !plan.remove && model.shown && plan.state == TrayIconState::normal,
          "tray plan: first available tick adds the icon");
    plan = tray_plan(&model, idle, true);
    check(!plan.add && !plan.update && !plan.remove, "tray plan: unchanged input is idle");
    TrayInput mentions{};
    mentions.mentions = 3;
    mentions.unread = true;
    plan = tray_plan(&model, mentions, true);
    check(plan.update && plan.state == TrayIconState::unread && plan.badge == 3, "tray plan: mentions update the badge");
    mentions.mentions = 0;
    plan = tray_plan(&model, mentions, true);
    check(plan.update && plan.badge == kTaskbarBadgeDot, "tray plan: unread without mentions is a dot");
    tray_model_reset(&model); // TaskbarCreated
    plan = tray_plan(&model, mentions, true);
    check(plan.add, "tray plan: a shell restart adds the icon again");
    plan = tray_plan(&model, mentions, false);
    check(plan.remove && !model.shown && !tray_plan(&model, mentions, false).remove,
          "tray plan: an unavailable tray removes once");

    TrayMenuItem items[kTrayMenuItems];
    TrayInput voice{};
    voice.self_deaf = true;
    const u32 count = tray_menu_build(voice, items, kTrayMenuItems);
    check(count == 4 && items[0].command == tray_command_open && items[1].command == tray_command_mute &&
              items[1].checked && items[2].command == tray_command_deafen && items[2].checked &&
              items[3].command == tray_command_quit && items[3].separator_before &&
              !tray_menu_build(voice, items, 3),
          "tray menu: Open, Mute, Deafen (checked by self state) and Quit");
    wchar_t tip[128];
    voice.connected = true;
    const u32 tip_units = tray_tooltip(voice, tip, 128);
    const bool deafened_tip = tip_units == length(tip) && same(tip, tip_units, L"MaiCord \x2014 Deafened");
    check(deafened_tip && tray_tooltip(idle, tip, 128) == 7,
          "tray tooltip: names the voice state");

    const TrayIconPalette palette{0xFF5865F2u, 0xFF4450C0u, 0xFF23A55Au, 0xFFF23F43u, 0xFFFFFFFFu};
    static u32 normal[32 * 32], deafened[32 * 32], counted[32 * 32];
    const bool rendered = tray_icon_render(TrayIconState::normal, 0, palette, 32, {normal, 32 * 32}) &&
                          tray_icon_render(TrayIconState::deafened, 0, palette, 32, {deafened, 32 * 32}) &&
                          tray_icon_render(TrayIconState::unread, 5, palette, 32, {counted, 32 * 32});
    const u32 centre = 16 * 32 + 16, corner = 0, overlay = 26 * 32 + 26;
    check(rendered && (normal[corner] >> 24) == 0 && (normal[centre] >> 24) == 255 &&
              (normal[overlay] >> 24) == 255 && normal[overlay] != deafened[overlay] &&
              normal[overlay] != counted[overlay] && normal[4 * 32 + 16] == deafened[4 * 32 + 16] &&
              !tray_icon_render(TrayIconState::normal, 0, palette, 8, {normal, 64}),
          "tray icon: rounded mark with transparent corners and a lower-right state overlay");

    check(desktop_close_action(true, true, false) == DesktopCloseAction::hide &&
              desktop_close_action(true, true, true) == DesktopCloseAction::quit &&
              desktop_close_action(false, true, false) == DesktopCloseAction::quit &&
              desktop_close_action(true, false, false) == DesktopCloseAction::quit,
          "close button: hide to tray only when enabled, shown and not quitting");
    check(desktop_initial_show(false, true, true) == SW_SHOWDEFAULT &&
              desktop_initial_show(true, false, true) == SW_SHOWDEFAULT &&
              desktop_initial_show(true, true, true) == SW_HIDE &&
              desktop_initial_show(true, true, false) == SW_SHOWMINNOACTIVE,
          "start minimized: only startup-entry launches, into the tray when it is on");
}

void test_registry(Check check) noexcept {
    wchar_t base[96];
    const wchar_t prefix[] = L"Software\\MaiCordTests\\desktop-";
    u32 at = length(prefix);
    copy_bytes(base, prefix, at * sizeof(wchar_t));
    u64 tag = (static_cast<u64>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    for (u32 i = 0; i < 16; ++i)
        base[at++] = L"0123456789abcdef"[(tag >> (60 - i * 4)) & 15];
    base[at] = 0;
    wchar_t run[160], approved[160], classes[160];
    const auto join = [&](wchar_t* out, const wchar_t* leaf) noexcept {
        copy_bytes(out, base, at * sizeof(wchar_t));
        const u32 n = length(leaf);
        copy_bytes(out + at, leaf, (n + 1) * sizeof(wchar_t));
    };
    join(run, L"\\Run");
    join(approved, L"\\Approved");
    join(classes, L"\\Classes");
    const DesktopRegistryScope scope{HKEY_CURRENT_USER, run, approved, classes, L"MaiCord", L"discord"};
    const auto exe = wide(L"C:\\Program Files\\MaiCord\\MaiCord.exe");
    const auto other = wide(L"C:\\Users\\x\\AppData\\Local\\Discord\\Update.exe");

    check(desktop_startup_query(scope, exe) == DesktopRegistration::absent &&
              desktop_startup_set(scope, exe, true) && desktop_startup_query(scope, exe) == DesktopRegistration::ours &&
              desktop_startup_query(scope, other) == DesktopRegistration::other,
          "startup entry: enable writes the quoted dispatcher path");
    wchar_t command[256];
    DWORD bytes = sizeof(command);
    check(RegGetValueW(HKEY_CURRENT_USER, run, L"MaiCord", RRF_RT_REG_SZ, nullptr, command, &bytes) == ERROR_SUCCESS &&
              same(command, length(command), L"\"C:\\Program Files\\MaiCord\\MaiCord.exe\" --startup"),
          "startup entry: command is \"<exe>\" --startup");
    const u8 disabled[12] = {3};
    HKEY key{};
    const bool approval = RegCreateKeyExW(HKEY_CURRENT_USER, approved, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                          nullptr) == ERROR_SUCCESS &&
                          RegSetValueExW(key, L"MaiCord", 0, REG_BINARY, disabled, sizeof(disabled)) == ERROR_SUCCESS;
    if (key)
        RegCloseKey(key);
    check(approval && desktop_startup_query(scope, exe) == DesktopRegistration::absent &&
              desktop_startup_set(scope, exe, true) && desktop_startup_query(scope, exe) == DesktopRegistration::ours,
          "startup entry: a Task Manager disable reads as off; enabling clears it");
    check(desktop_startup_set(scope, exe, false) && desktop_startup_query(scope, exe) == DesktopRegistration::absent &&
              desktop_startup_set(scope, exe, false),
          "startup entry: disable removes it (idempotent)");

    check(desktop_protocol_query(scope, exe) == DesktopRegistration::absent && desktop_protocol_set(scope, exe, true) &&
              desktop_protocol_query(scope, exe) == DesktopRegistration::ours,
          "protocol: enable registers discord:// for this executable");
    wchar_t command_key[192];
    copy_bytes(command_key, classes, length(classes) * sizeof(wchar_t));
    const wchar_t tail[] = L"\\discord\\shell\\open\\command";
    copy_bytes(command_key + length(classes), tail, sizeof(tail));
    bytes = sizeof(command);
    wchar_t marker[16];
    DWORD marker_bytes = sizeof(marker);
    wchar_t protocol_key[192];
    copy_bytes(protocol_key, classes, length(classes) * sizeof(wchar_t));
    copy_bytes(protocol_key + length(classes), L"\\discord", 9 * sizeof(wchar_t));
    check(RegGetValueW(HKEY_CURRENT_USER, command_key, nullptr, RRF_RT_REG_SZ, nullptr, command, &bytes) ==
                  ERROR_SUCCESS &&
              same(command, length(command), L"\"C:\\Program Files\\MaiCord\\MaiCord.exe\" --url -- \"%1\"") &&
              RegGetValueW(HKEY_CURRENT_USER, protocol_key, L"URL Protocol", RRF_RT_REG_SZ, nullptr, marker,
                           &marker_bytes) == ERROR_SUCCESS,
          "protocol: URL Protocol class with --url -- \"%1\"");
    check(desktop_protocol_set(scope, exe, false) && desktop_protocol_query(scope, exe) == DesktopRegistration::absent,
          "protocol: disable removes our class");
    check(desktop_protocol_set(scope, other, true) && desktop_protocol_query(scope, exe) == DesktopRegistration::other &&
              desktop_protocol_set(scope, exe, false) && desktop_protocol_query(scope, other) == DesktopRegistration::ours,
          "protocol: disabling never deletes another client's handler");
    RegDeleteTreeW(HKEY_CURRENT_USER, base);
    RegDeleteKeyW(HKEY_CURRENT_USER, base);
    HKEY parent{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MaiCordTests", 0, KEY_READ, &parent) == ERROR_SUCCESS) {
        DWORD subkeys = 1, values = 1;
        RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                         nullptr, nullptr);
        RegCloseKey(parent);
        if (!subkeys && !values)
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\MaiCordTests");
    }
    check(RegOpenKeyExW(HKEY_CURRENT_USER, base, 0, KEY_READ, &key) == ERROR_FILE_NOT_FOUND,
          "registry: the private test key is removed");
}

void test_single_instance(Check check) noexcept {
    wchar_t mutex_name[64], window_name[64], other_name[64];
    const auto profile = wide(L"D:\\Profiles\\Test\\AppData\\Local");
    check(desktop_instance_name(L"Instance", profile, mutex_name, 64) &&
              desktop_instance_name(L"Window", wide(L"d:/profiles/test/appdata/local"), window_name, 64) &&
              desktop_instance_name(L"Window", profile, other_name, 64) &&
              bytes_equal(window_name, other_name, (length(other_name) + 1) * sizeof(wchar_t)) &&
              !bytes_equal(mutex_name, other_name, (length(other_name) + 1) * sizeof(wchar_t)) &&
              !desktop_instance_name(L"Window", profile, other_name, 20),
          "single instance: names are per profile, case and separator insensitive");
    // Unique names for this run so the user's client is never reached.
    u64 tag = (static_cast<u64>(GetCurrentProcessId()) << 20) ^ GetTickCount64();
    wchar_t unique[40];
    for (u32 i = 0; i < 16; ++i)
        unique[i] = L"0123456789abcdef"[(tag >> (60 - i * 4)) & 15];
    unique[16] = 0;
    check(desktop_instance_name(L"Instance", {unique, 16}, mutex_name, 64) &&
              desktop_instance_name(L"Window", {unique, 16}, window_name, 64),
          "single instance: test names");
    bool existed = true;
    const HANDLE first = desktop_instance_mutex(mutex_name, &existed);
    const bool first_new = first && !existed;
    const HANDLE second = desktop_instance_mutex(mutex_name, &existed);
    check(first_new && second && existed, "single instance: the second launch sees the first");
    DesktopLaunch& launch = launch_storage;
    desktop_launch_parse(wide(L"MaiCord.exe --url -- discord://-/invite/abc"), &launch);
    check(desktop_instance_forward(window_name, launch, 1000) == DesktopForwardResult::no_window,
          "single instance: no published window yet");
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = forward_window;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"MaiCordDesktopForwardTest";
    RegisterClassExW(&window_class);
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                        window_class.hInstance, nullptr);
    const HANDLE published = desktop_instance_publish(window_name, window);
    received_count = 0;
    const auto forwarded = desktop_instance_forward(window_name, launch, 1000);
    check(window && published && forwarded == DesktopForwardResult::forwarded && received_count == 1 && received_valid &&
              received.url_present && same(received.url, received.url_units, L"discord://-/invite/abc"),
          "single instance: the second launch forwards its link to the first window");
    desktop_launch_parse(wide(L"MaiCord.exe"), &launch);
    check(desktop_instance_forward(window_name, launch, 1000) == DesktopForwardResult::forwarded && received_valid &&
              !received.url_present && !received.url_units,
          "single instance: a plain launch forwards a show request");
    struct {
        u32 version, flags, units;
        wchar_t url[4];
    } forged{1, 0, 3, {L'a', L'b', L'c', 0}};
    COPYDATASTRUCT bad{kDesktopForwardMagic, 12 + 6, &forged};
    COPYDATASTRUCT foreign{0x1234, 12, &forged};
    check(!desktop_instance_receive(&bad, &received) && !desktop_instance_receive(&foreign, &received) &&
              !desktop_instance_receive(nullptr, &received),
          "single instance: malformed or foreign WM_COPYDATA is refused");
    if (window)
        DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    check(desktop_instance_forward(window_name, launch, 1000) == DesktopForwardResult::no_window,
          "single instance: a destroyed window is not a target");
    if (published)
        CloseHandle(published);
    if (second)
        CloseHandle(second);
    if (first) {
        ReleaseMutex(first);
        CloseHandle(first);
    }
}
// The taskbar restore: the shell activates the minimized window, then posts
// SC_RESTORE. Activation work that makes a cross-apartment COM call from the
// UI STA lets COM's modal loop discard that queued WM_SYSCOMMAND with a beep.
constexpr CLSID kRestoreTaskbarListClass = {0x56FDF344, 0xFD6D, 0x11D0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
constexpr IID kRestoreTaskbarList = {0x56FDF342, 0xFD6D, 0x11D0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
struct RestoreProbe {
    HANDLE ready;
    IStream* stream;
    ITaskbarList* remote;
    DWORD server;
    u32 calls;
    bool apartment, created, minimized_kept, active_called;
};
RestoreProbe* restore_probe;
// The object lives in a second STA, so every call from the window thread is
// a cross-apartment call through the COM modal loop, like the toast setting.
DWORD WINAPI restore_server(void* context) noexcept {
    auto* probe = static_cast<RestoreProbe*>(context);
    probe->server = GetCurrentThreadId();
    ITaskbarList* list = nullptr;
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (initialized && SUCCEEDED(CoCreateInstance(kRestoreTaskbarListClass, nullptr, CLSCTX_INPROC_SERVER,
                                                  kRestoreTaskbarList, reinterpret_cast<void**>(&list))) &&
        FAILED(CoMarshalInterThreadInterfaceInStream(kRestoreTaskbarList, list, &probe->stream)))
        probe->stream = nullptr;
    SetEvent(probe->ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
        DispatchMessageW(&message);
    if (list)
        list->Release();
    if (initialized)
        CoUninitialize();
    return 0;
}
LRESULT CALLBACK restore_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    // The main window's WM_ACTIVATE: the notification refresh only when allowed.
    if (message == WM_ACTIVATE) {
        if (restore_probe && restore_probe->remote && desktop_activation_refresh_allowed(wparam)) {
            ++restore_probe->calls;
            restore_probe->remote->HrInit();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool restore_queued(HWND window) noexcept {
    MSG message{};
    return PeekMessageW(&message, window, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_NOREMOVE | PM_NOYIELD) &&
           (message.wParam & 0xFFF0) == SC_RESTORE;
}
void restore_drop(HWND window) noexcept {
    MSG message{};
    while (PeekMessageW(&message, window, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE | PM_NOYIELD)) {
    }
}
DWORD WINAPI restore_client(void* context) noexcept {
    auto* probe = static_cast<RestoreProbe*>(context);
    probe->apartment = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    probe->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE server = probe->apartment && probe->ready ? CreateThread(nullptr, 0, restore_server, probe, 0, nullptr)
                                                           : nullptr;
    if (server && WaitForSingleObject(probe->ready, 5000) == WAIT_OBJECT_0 && probe->stream &&
        SUCCEEDED(CoGetInterfaceAndReleaseStream(probe->stream, kRestoreTaskbarList,
                                                 reinterpret_cast<void**>(&probe->remote))))
        probe->created = true;
    probe->stream = nullptr;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = restore_window;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"MaiCordTaskbarRestoreTest";
    RegisterClassExW(&window_class);
    // Message-only: the COM loop discards WM_SYSCOMMAND for any window, and
    // no real activation (or foreground change) happens in this test.
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                        window_class.hInstance, nullptr);
    if (window && probe->created) {
        // The shell's restore: activated while minimized, SC_RESTORE queued.
        PostMessageW(window, WM_SYSCOMMAND, SC_RESTORE, 0);
        SendMessageW(window, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 1), 0);
        probe->minimized_kept = probe->calls == 0 && restore_queued(window);
        restore_drop(window);
        // The restore's own activation runs the refresh (nothing is queued).
        SendMessageW(window, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        probe->active_called = probe->calls == 1;
    }
    if (window)
        DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    if (probe->remote) {
        probe->remote->Release();
        probe->remote = nullptr;
    }
    if (server) {
        PostThreadMessageW(probe->server, WM_QUIT, 0, 0);
        WaitForSingleObject(server, 5000);
        CloseHandle(server);
    }
    if (probe->ready)
        CloseHandle(probe->ready);
    if (probe->apartment)
        CoUninitialize();
    return 0;
}

void test_taskbar_restore(Check check) noexcept {
    check(desktop_activation_refresh_allowed(MAKEWPARAM(WA_ACTIVE, 0)) &&
              desktop_activation_refresh_allowed(MAKEWPARAM(WA_CLICKACTIVE, 0)),
          "taskbar restore: a restored window refreshes on activation");
    check(!desktop_activation_refresh_allowed(MAKEWPARAM(WA_ACTIVE, 1)) &&
              !desktop_activation_refresh_allowed(MAKEWPARAM(WA_CLICKACTIVE, 1)) &&
              !desktop_activation_refresh_allowed(MAKEWPARAM(WA_INACTIVE, 0)),
          "taskbar restore: no refresh while minimized or on deactivation");
    RestoreProbe probe{};
    restore_probe = &probe;
    const HANDLE thread = CreateThread(nullptr, 0, restore_client, &probe, 0, nullptr);
    if (thread) {
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
    }
    restore_probe = nullptr;
    check(probe.created, "taskbar restore: a cross-apartment COM object is reachable from the window thread");
    check(probe.minimized_kept,
          "taskbar restore: activation while minimized makes no COM call, so the queued SC_RESTORE survives");
    check(probe.active_called, "taskbar restore: the non-minimized activation still refreshes");
}
} // namespace

void test_desktop_integration(Check check) noexcept {
    test_launch(check);
    test_links(check);
    test_tray(check);
    test_registry(check);
    test_single_instance(check);
    test_taskbar_restore(check);
}

} // namespace maicord

#include "application_desktop.hpp"
#include "application_account_logout.hpp"
#include "application_content_input.hpp"
#include "application_guild_invite.hpp"
#include "application_state.hpp"
#include "application_taskbar_attention.hpp"
#include "application_update_startup.hpp"
#include "application_voice.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/system_tray.hpp"

#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {
namespace {
constexpr UINT kTrayMessage = WM_APP + 0x3C;
constexpr wchar_t kLaunchUrlVariable[] = L"MAICORD_LAUNCH_URL";
constexpr wchar_t kLaunchStartupVariable[] = L"MAICORD_LAUNCH_STARTUP";
constexpr u32 kForwardTimeoutMs = 5'000;
// A link waits this long for sign-in and the Gateway snapshot.
constexpr u64 kLinkAccountWaitMs = 120'000;
// ...and this long for its channel once the account is up.
constexpr u64 kLinkChannelWaitMs = 20'000;
constexpr u32 kNameCapacity = 64;

struct Desktop {
    DesktopLaunch launch;
    DesktopLinkTarget link;
    SystemTray tray;
    TrayIconPalette window_palette;
    HANDLE mutex, published;
    HICON window_icons[2];
    u64 link_queued_ms, link_account_ms, tray_retry_ms;
    wchar_t tooltip[128];
    bool owns_mutex, tray_initialized, quit_requested, link_pending;
};
Desktop g_desktop{};

bool instance_names(wchar_t* mutex_name, wchar_t* window_name) noexcept {
    wchar_t profile[1024];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", profile, 1024);
    return n && n < 1024 && desktop_instance_name(L"Instance", {profile, n}, mutex_name, kNameCapacity) &&
           desktop_instance_name(L"Window", {profile, n}, window_name, kNameCapacity);
}
void take_mutex(const wchar_t* name) noexcept {
    if (g_desktop.mutex)
        return;
    bool existed = false;
    g_desktop.mutex = desktop_instance_mutex(name, &existed);
    g_desktop.owns_mutex = g_desktop.mutex && !existed;
}
void clear_launch_environment() noexcept {
    SetEnvironmentVariableW(kLaunchUrlVariable, nullptr);
    SetEnvironmentVariableW(kLaunchStartupVariable, nullptr);
}

bool same_palette(const TrayIconPalette& a, const TrayIconPalette& b) noexcept {
    return bytes_equal(&a, &b, sizeof(a));
}
TrayInput tray_input() noexcept {
    TrayInput input{};
    u32 mentions = 0;
    bool unread = false;
    application_taskbar_attention_counts(&mentions, &unread);
    // As in the official client, the unread state follows the unread badge setting.
    if (g_app.preferences.unread_badge) {
        input.mentions = mentions;
        input.unread = unread || mentions;
    }
    const auto voice = application_voice_view();
    input.connected = voice.state != UiVoicePanelState::hidden;
    input.speaking = voice.speaking;
    input.self_mute = voice.self_mute;
    input.self_deaf = voice.self_deaf;
    input.server_mute = voice.server_mute;
    input.server_deaf = voice.server_deaf;
    return input;
}
// The window and taskbar button carry MaiCord's mark in the theme's colours.
void update_window_icons(const TrayIconPalette& colors) noexcept {
    if (!g_app.window || (g_desktop.window_icons[0] && same_palette(colors, g_desktop.window_palette)))
        return;
    const u32 dpi = maximum(g_app.renderer.dpi, 96u);
    const int metrics[2] = {GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CXICON, dpi)};
    for (u32 i = 0; i < 2; ++i) {
        const u32 extent = static_cast<u32>(metrics[i] < 16 ? 16 : metrics[i] > 64 ? 64 : metrics[i]);
        const HICON icon = system_tray_create_icon(TrayIconState::normal, 0, colors, extent);
        if (!icon)
            continue;
        SendMessageW(g_app.window, WM_SETICON, i ? ICON_BIG : ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        if (g_desktop.window_icons[i])
            DestroyIcon(g_desktop.window_icons[i]);
        g_desktop.window_icons[i] = icon;
    }
    g_desktop.window_palette = colors;
}
void tick_tray(u64 now_ms) noexcept {
    if (!g_desktop.tray_initialized)
        g_desktop.tray_initialized = system_tray_initialize(&g_desktop.tray, g_app.window, kTrayMessage);
    if (!g_desktop.tray_initialized || now_ms < g_desktop.tray_retry_ms)
        return;
    const auto input = tray_input();
    const auto colors = application_tray_palette();
    update_window_icons(colors);
    auto plan = tray_plan(&g_desktop.tray.model, input, true);
    wchar_t tooltip[128];
    tray_tooltip(input, tooltip, 128);
    u32 n = 0;
    while (tooltip[n] && tooltip[n] == g_desktop.tooltip[n])
        ++n;
    if (tooltip[n] != g_desktop.tooltip[n]) {
        copy_bytes(g_desktop.tooltip, tooltip, sizeof(tooltip));
        plan.update = !plan.add;
    }
    if (!plan.add && !plan.update && same_palette(colors, g_desktop.tray.palette))
        return;
    if (!system_tray_apply(&g_desktop.tray, plan, colors, g_desktop.tooltip, maximum(g_app.renderer.dpi, 96u))) {
        // Explorer may not be up yet (startup); TaskbarCreated also re-adds it.
        g_desktop.tray_retry_ms = now_ms + 5'000;
        g_desktop.tooltip[0] = 0;
    }
}
void reveal() noexcept {
    desktop_window_reveal(g_app.window);
    application_request_frame();
}
void tray_command(u32 command) noexcept {
    switch (command) {
    case tray_command_open:
        reveal();
        break;
    case tray_command_mute:
        if (!application_account_logout_blocked())
            application_voice_toggle_mute();
        break;
    case tray_command_deafen:
        if (!application_account_logout_blocked())
            application_voice_toggle_deafen();
        break;
    case tray_command_quit:
        application_desktop_request_quit();
        PostMessageW(g_app.window, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}
void tray_event(UINT event) noexcept {
    switch (event) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        reveal();
        break;
    case WM_CONTEXTMENU: {
        TrayMenuItem items[kTrayMenuItems];
        const u32 count = tray_menu_build(tray_input(), items, kTrayMenuItems);
        tray_command(system_tray_menu(&g_desktop.tray, items, count));
        break;
    }
    default:
        break;
    }
}
void tick_link(u64 now_ms) noexcept {
    if (!g_desktop.link_pending)
        return;
    const auto& link = g_desktop.link;
    if (link.kind == DesktopLinkKind::open) {
        g_desktop.link_pending = false;
        return;
    }
    const bool ready = g_app.account_ui && g_app.account.instance && !g_app.login.visible &&
                       !application_account_logout_blocked();
    if (!ready) {
        g_desktop.link_account_ms = 0;
        if (now_ms - g_desktop.link_queued_ms > kLinkAccountWaitMs)
            g_desktop.link_pending = false;
        return;
    }
    if (!g_desktop.link_account_ms)
        g_desktop.link_account_ms = now_ms;
    const bool expired = now_ms - g_desktop.link_account_ms > kLinkChannelWaitMs;
    if (link.kind == DesktopLinkKind::invite) {
        // Another dialog may be open; the join dialog waits for it briefly.
        if (application_guild_invite_open({link.code, link.code_bytes}) || expired)
            g_desktop.link_pending = false;
        return;
    }
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, link.channel_id);
    if (!channel) {
        if (expired) {
            g_desktop.link_pending = false;
            log_write(LogLevel::info, "desktop", "link_channel_unavailable");
        }
        return;
    }
    g_desktop.link_pending = false;
    if (channel->guild_id != link.guild_id)
        return;
    if (!application_navigate_known_message(link.channel_id, link.message_id))
        log_write(LogLevel::info, "desktop", "link_navigation_refused");
}
} // namespace

bool application_desktop_forward_existing(ConstSlice<wchar_t> command_line) noexcept {
    if (application_update_startup_classify(command_line) != ApplicationUpdateStartupCommandKind::ordinary)
        return false;
    DesktopLaunch& launch = g_desktop.launch;
    if (!desktop_launch_parse(command_line, &launch))
        clear_bytes(&launch, sizeof(launch));
    // A dispatched generation child receives only the updater's protocol
    // arguments; it reads the launch from the inherited environment.
    clear_launch_environment();
    if (launch.url_present && launch.url_units < kDesktopUrlCapacity) {
        wchar_t url[kDesktopUrlCapacity + 1];
        copy_bytes(url, launch.url, launch.url_units * sizeof(wchar_t));
        url[launch.url_units] = 0;
        SetEnvironmentVariableW(kLaunchUrlVariable, url);
    }
    if (launch.startup)
        SetEnvironmentVariableW(kLaunchStartupVariable, L"1");
    wchar_t mutex_name[kNameCapacity], window_name[kNameCapacity];
    if (!instance_names(mutex_name, window_name))
        return false;
    take_mutex(mutex_name);
    if (!g_desktop.mutex || g_desktop.owns_mutex)
        return false;
    // Another instance owns the profile: hand over, or wait while it starts
    // (no window yet) or exits (the mutex is released or abandoned).
    for (u32 attempt = 0; attempt < 40; ++attempt) {
        const auto forwarded = desktop_instance_forward(window_name, launch, kForwardTimeoutMs);
        if (forwarded == DesktopForwardResult::forwarded)
            return true;
        if (forwarded == DesktopForwardResult::refused)
            return true; // A busy or signed-out window; never start a second client.
        const DWORD waited = WaitForSingleObject(g_desktop.mutex, 250);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
            g_desktop.owns_mutex = true;
            return false;
        }
    }
    return false;
}

void application_desktop_prepare(ConstSlice<wchar_t> command_line, bool update_child) noexcept {
    DesktopLaunch& launch = g_desktop.launch;
    if (update_child) {
        clear_bytes(&launch, sizeof(launch));
        wchar_t flag[4];
        launch.startup = GetEnvironmentVariableW(kLaunchStartupVariable, flag, 4) == 1 && flag[0] == L'1';
        const DWORD units = GetEnvironmentVariableW(kLaunchUrlVariable, launch.url, kDesktopUrlCapacity);
        if (units && units < kDesktopUrlCapacity) {
            launch.url_present = true;
            launch.url_units = units;
        }
    } else if (!g_desktop.mutex && !desktop_launch_parse(command_line, &launch)) {
        clear_bytes(&launch, sizeof(launch));
    }
    clear_launch_environment();
    wchar_t mutex_name[kNameCapacity], window_name[kNameCapacity];
    if (instance_names(mutex_name, window_name))
        take_mutex(mutex_name);
    if (launch.url_present)
        application_desktop_open_link({launch.url, launch.url_units});
}

int application_desktop_initial_show() noexcept {
    return desktop_initial_show(g_desktop.launch.startup, g_app.preferences.start_minimized,
                                g_app.preferences.minimize_to_tray);
}

void application_desktop_window_created() noexcept {
    wchar_t mutex_name[kNameCapacity], window_name[kNameCapacity];
    if (g_app.window && !g_desktop.published && instance_names(mutex_name, window_name))
        g_desktop.published = desktop_instance_publish(window_name, g_app.window);
}

void application_desktop_tick(u64 now_ms) noexcept {
    if (!g_app.window)
        return;
    // An update child starts beside the exiting parent; claim the profile once it is gone.
    if (g_desktop.mutex && !g_desktop.owns_mutex) {
        const DWORD waited = WaitForSingleObject(g_desktop.mutex, 0);
        g_desktop.owns_mutex = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    tick_tray(now_ms);
    tick_link(now_ms);
}

bool application_desktop_window_message(UINT message, WPARAM, LPARAM lparam, LRESULT* result) noexcept {
    if (g_desktop.tray_initialized && system_tray_shell_restarted(&g_desktop.tray, message)) {
        g_desktop.tray_retry_ms = 0;
        g_desktop.tooltip[0] = 0;
        application_request_frame();
        *result = 0;
        return true;
    }
    if (message == kTrayMessage) {
        tray_event(LOWORD(lparam));
        *result = 0;
        return true;
    }
    if (message == WM_COPYDATA) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (!data || data->dwData != kDesktopForwardMagic)
            return false;
        static DesktopForwardMessage forwarded;
        const bool valid = desktop_instance_receive(data, &forwarded);
        if (valid) {
            reveal();
            if (forwarded.url_present)
                application_desktop_open_link({forwarded.url, forwarded.url_units});
        }
        *result = valid ? TRUE : FALSE;
        return true;
    }
    return false;
}

bool application_desktop_close_to_tray() noexcept {
    if (!g_app.window || desktop_close_action(g_app.preferences.minimize_to_tray, g_desktop.tray.added,
                                              g_desktop.quit_requested) != DesktopCloseAction::hide)
        return false;
    ShowWindow(g_app.window, SW_HIDE);
    return true;
}

void application_desktop_request_quit() noexcept {
    g_desktop.quit_requested = true;
}

bool application_desktop_open_link(ConstSlice<wchar_t> url) noexcept {
    DesktopLinkTarget target{};
    if (!desktop_link_parse(url, &target)) {
        log_write(LogLevel::info, "desktop", "link_refused");
        return false;
    }
    g_desktop.link = target;
    g_desktop.link_pending = true;
    g_desktop.link_queued_ms = clock_monotonic_milliseconds();
    g_desktop.link_account_ms = 0;
    return true;
}

void application_desktop_release() noexcept {
    if (g_desktop.tray_initialized)
        system_tray_release(&g_desktop.tray);
    for (HICON& icon : g_desktop.window_icons)
        if (icon) {
            DestroyIcon(icon);
            icon = nullptr;
        }
    if (g_desktop.published)
        CloseHandle(g_desktop.published);
    if (g_desktop.mutex) {
        if (g_desktop.owns_mutex)
            ReleaseMutex(g_desktop.mutex);
        CloseHandle(g_desktop.mutex);
    }
    reset_value(&g_desktop);
}

} // namespace maicord::app_detail

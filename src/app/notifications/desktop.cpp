#include "desktop.hpp"
#include "../account/application_account_logout.hpp"
#include "../shell/content_input.hpp"
#include "../guild/invite.hpp"
#include "../shell/state.hpp"
#include "taskbar_attention.hpp"
#include "../packages/packages.hpp"
#include "../voice/voice.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/desktop/system_tray.hpp"
#include "../account/account_ui_runtime.hpp"

#if defined(_WIN32)
#include "maicord/platform/window/win32.hpp"
#include "maicord/sys/shell.hpp"
#else
#include "maicord/os/text.hpp"
#include "maicord/platform/dbus/instance.hpp"
#include "maicord/platform/dbus/tray.hpp"
#include "maicord/platform/window/wayland.hpp"
#endif

namespace maicord::app_detail {
namespace {
#if defined(_WIN32)
constexpr UINT kTrayMessage = WM_APP + 0x3C;
#endif
constexpr u32 kForwardTimeoutMs = 5'000;
// A link waits this long for sign-in and the Gateway snapshot.
constexpr u64 kLinkAccountWaitMs = 120'000;
// ...and this long for its channel once the account is up.
constexpr u64 kLinkChannelWaitMs = 20'000;
#if defined(_WIN32)
constexpr u32 kNameCapacity = 64;
#endif

struct Desktop {
    DesktopLaunch launch;
    DesktopLinkTarget link;
    TrayIconPalette window_palette;
    u64 link_queued_ms, link_account_ms, tray_retry_ms;
#if defined(_WIN32)
    SystemTray tray;
    HANDLE mutex, published;
    wchar_t tooltip[128];
    bool owns_mutex, tray_initialized;
#else
    // The last state handed to the notification-area item, to send it again only when it changed.
    TrayInput tray_input;
    TrayIconPalette tray_palette;
    bool tray_started, tray_sent, instance_checked;
#endif
    bool quit_requested, link_pending, window_icons_set;
};
Desktop g_desktop{};

#if defined(_WIN32)
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
#endif

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
    if (!g_app.window || (g_desktop.window_icons_set && same_palette(colors, g_desktop.window_palette)))
        return;
#if defined(_WIN32)
    const u32 dpi = maximum(g_app.renderer.dpi, 96u);
    const int metrics[2] = {GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CXICON, dpi)};
#else
    const int metrics[2] = {32, 64}; // The compositor scales to the size it wants.
#endif
    constexpr u32 kPlane = kTrayIconMaximumExtent * kTrayIconMaximumExtent;
    ScratchPages<u32[2 * kPlane]> pixels;
    if (!pixels.data)
        return;
    IconImage images[2]{};
    u32 count = 0;
    for (u32 i = 0; i < 2; ++i) {
        const u32 extent = static_cast<u32>(metrics[i] < 16 ? 16 : metrics[i] > 64 ? 64 : metrics[i]);
        u32* plane = *pixels.data + i * kPlane;
        if (tray_icon_render(TrayIconState::normal, 0, colors, extent, {plane, extent * extent}))
            images[count++] = {plane, extent};
    }
    if (!count)
        return;
    window_set_icon(g_app.window, images, count);
    g_desktop.window_icons_set = true;
    g_desktop.window_palette = colors;
}
#if defined(_WIN32)
void tick_tray(u64 now_ms) noexcept {
    if (!g_desktop.tray_initialized)
        g_desktop.tray_initialized = system_tray_initialize(&g_desktop.tray, win32_hwnd(g_app.window), kTrayMessage);
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
#endif
bool tray_visible() noexcept {
#if defined(_WIN32)
    return g_desktop.tray.added;
#else
    return dbus::tray_available();
#endif
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
        window_request_close(g_app.window);
        break;
    default:
        break;
    }
}
#if defined(_WIN32)
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
#else
// The notification-area item (platform/dbus/tray.hpp) reports inside the session bus processing, on the UI thread: the
// host draws the menu itself and sends the chosen command.
void on_tray(void*, const dbus::TrayEvent& event) noexcept {
    switch (event.kind) {
    case dbus::TrayEventKind::activate:
        reveal();
        break;
    case dbus::TrayEventKind::command:
        tray_command(event.command);
        break;
    default:
        break;
    }
}
// A launch another process handed over (a link, or just "show yourself").
void on_launch(void*, const dbus::LaunchRequest& request) noexcept {
    // The second launch brings the compositor's activation token (XDG_ACTIVATION_TOKEN): with it the window may take the focus.
    window_wayland_activate(request.activation_token);
    reveal();
    wchar_t url[kDesktopUrlCapacity];
    const u32 units = request.url_present ? utf8_to_utf16(request.url, request.url_bytes, url, kDesktopUrlCapacity, true)
                                          : 0;
    if (units)
        application_desktop_open_link({url, units});
}
void tick_tray(u64 now_ms) noexcept {
    if (!g_desktop.tray_started) {
        if (now_ms < g_desktop.tray_retry_ms)
            return;
        g_desktop.tray_started = dbus::tray_start("maicord", on_tray, nullptr);
        if (!g_desktop.tray_started) {
            g_desktop.tray_retry_ms = now_ms + 5'000;
            return;
        }
    }
    const auto input = tray_input();
    const auto colors = application_tray_palette();
    update_window_icons(colors);
    if (g_desktop.tray_sent && bytes_equal(&input, &g_desktop.tray_input, sizeof(input)) &&
        same_palette(colors, g_desktop.tray_palette))
        return;
    TrayModel model{};
    const TrayPlan plan = tray_plan(&model, input, true);
    constexpr u32 kSizes[] = {16, 24, 32, 48, 64};
    constexpr u32 kPixels = 16 * 16 + 24 * 24 + 32 * 32 + 48 * 48 + 64 * 64;
    ScratchPages<u32[kPixels]> pixels;
    if (!pixels.data)
        return;
    dbus::TrayPixmap icons[5];
    u32 icon_count = 0, used = 0;
    for (const u32 extent : kSizes) {
        u32* plane = *pixels.data + used;
        if (!tray_icon_render(plan.state, plan.badge, colors, extent, {plane, extent * extent}))
            continue;
        icons[icon_count++] = {extent, extent, plane};
        used += extent * extent;
    }
    if (!icon_count)
        return;
    wchar_t tooltip[128];
    char tooltip_text[128 * 3];
    const u32 tooltip_bytes = utf16_to_utf8(tooltip, tray_tooltip(input, tooltip, 128), tooltip_text,
                                            sizeof(tooltip_text) - 1, false);
    tooltip_text[tooltip_bytes] = 0;
    TrayMenuItem items[kTrayMenuItems];
    const u32 count = tray_menu_build(input, items, kTrayMenuItems);
    char labels[kTrayMenuItems][64];
    dbus::TrayMenuEntry menu[kTrayMenuItems];
    for (u32 i = 0; i < count; ++i) {
        u32 units = 0;
        while (items[i].label[units])
            ++units;
        labels[i][utf16_to_utf8(items[i].label, units, labels[i], sizeof(labels[i]) - 1, false)] = 0;
        menu[i] = {labels[i], items[i].command,
                   items[i].command == tray_command_mute || items[i].command == tray_command_deafen, items[i].checked,
                   items[i].separator_before};
    }
    dbus::tray_update({"MaiCord", tooltip_text, icons, icon_count, false, menu, count});
    g_desktop.tray_input = input;
    g_desktop.tray_palette = colors;
    g_desktop.tray_sent = true;
}
#endif
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

#if defined(_WIN32)
bool application_desktop_forward_existing(ConstSlice<wchar_t> command_line) noexcept {
    DesktopLaunch& launch = g_desktop.launch;
    if (!desktop_launch_parse(command_line, &launch))
        clear_bytes(&launch, sizeof(launch));
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
#else
namespace {
// Takes the profile's single-instance claim, or hands the launch to the running client (platform/dbus/instance.hpp).
// The profile is the default one unless XDG_DATA_HOME moved the data, as a test profile does.
dbus::InstanceResult acquire_instance(ConstSlice<wchar_t> command_line) noexcept {
    DesktopLaunch& launch = g_desktop.launch;
    if (!desktop_launch_parse(command_line, &launch))
        clear_bytes(&launch, sizeof(launch));
    dbus::LaunchRequest request{};
    request.startup = launch.startup;
    if (launch.url_present) {
        request.url_bytes = utf16_to_utf8(launch.url, launch.url_units, request.url, sizeof(request.url), false);
        request.url_present = request.url_bytes != 0;
    }
    wchar_t wide[dbus::kLaunchTokenCapacity];
    u32 units = environment_variable(L"XDG_ACTIVATION_TOKEN", wide, dbus::kLaunchTokenCapacity);
    if (!units)
        units = environment_variable(L"DESKTOP_STARTUP_ID", wide, dbus::kLaunchTokenCapacity);
    if (units && units < dbus::kLaunchTokenCapacity)
        request.activation_token[utf16_to_utf8(wide, units, request.activation_token,
                                               dbus::kLaunchTokenCapacity - 1, false)] = 0;
    char profile[3072];
    const char* profile_key = nullptr;
    wchar_t data[1024];
    const u32 data_units = environment_variable(L"XDG_DATA_HOME", data, 1024);
    if (data_units && data_units < 1024) {
        profile[utf16_to_utf8(data, data_units, profile, sizeof(profile) - 1, false)] = 0;
        profile_key = profile;
    }
    g_desktop.instance_checked = true;
    return dbus::instance_acquire({"io.github.maj113.MaiCord", profile_key}, request, kForwardTimeoutMs);
}
} // namespace

bool application_desktop_forward_existing(ConstSlice<wchar_t> command_line) noexcept {
    const auto result = acquire_instance(command_line);
    // A refusal is a busy or signed-out owner: never start a second client.
    return result == dbus::InstanceResult::forwarded || result == dbus::InstanceResult::refused;
}
#endif

#if defined(_WIN32)
void application_desktop_prepare(ConstSlice<wchar_t> command_line) noexcept {
    DesktopLaunch& launch = g_desktop.launch;
    if (!g_desktop.mutex && !desktop_launch_parse(command_line, &launch))
        clear_bytes(&launch, sizeof(launch));
    wchar_t mutex_name[kNameCapacity], window_name[kNameCapacity];
    if (instance_names(mutex_name, window_name))
        take_mutex(mutex_name);
    if (launch.url_present)
        application_desktop_open_link({launch.url, launch.url_units});
}
#else
void application_desktop_prepare(ConstSlice<wchar_t> command_line) noexcept {
    if (!g_desktop.instance_checked)
        acquire_instance(command_line);
    const DesktopLaunch& launch = g_desktop.launch;
    if (launch.url_present)
        application_desktop_open_link({launch.url, launch.url_units});
}
#endif

int application_desktop_initial_show() noexcept {
    return desktop_initial_show(g_desktop.launch.startup, g_app.preferences.start_minimized,
                                g_app.preferences.minimize_to_tray);
}

void application_desktop_window_created() noexcept {
#if defined(_WIN32)
    wchar_t mutex_name[kNameCapacity], window_name[kNameCapacity];
    if (g_app.window && !g_desktop.published && instance_names(mutex_name, window_name))
        g_desktop.published = desktop_instance_publish(window_name, win32_hwnd(g_app.window));
#else
    // Requests that arrived before the window existed are delivered now.
    dbus::instance_set_handler(on_launch, nullptr);
#endif
}

void application_desktop_tick(u64 now_ms) noexcept {
    if (!g_app.window)
        return;
#if defined(_WIN32)
    // A launch that started while another instance was exiting claims the
    // profile once that instance is gone.
    if (g_desktop.mutex && !g_desktop.owns_mutex) {
        const DWORD waited = WaitForSingleObject(g_desktop.mutex, 0);
        g_desktop.owns_mutex = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
#endif
    tick_tray(now_ms);
    tick_link(now_ms);
    application_packages_tick(now_ms);
}

#if defined(_WIN32)
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
#endif

bool application_desktop_close_to_tray() noexcept {
    if (!g_app.window || desktop_close_action(g_app.preferences.minimize_to_tray, tray_visible(),
                                              g_desktop.quit_requested) != DesktopCloseAction::hide)
        return false;
    window_show(g_app.window, ShowMode::hide);
    return true;
}

void application_desktop_request_quit() noexcept {
    g_desktop.quit_requested = true;
}

bool application_desktop_open_link(const ConstSlice<wchar_t>& url) noexcept {
    if (url.count > 8 && (url[0] | 0x20) == L'm' && (url[1] | 0x20) == L'a' && (url[2] | 0x20) == L'i' &&
        (url[3] | 0x20) == L'c' && (url[4] | 0x20) == L'o' && (url[5] | 0x20) == L'r' && (url[6] | 0x20) == L'd' &&
        url[7] == L':')
        return application_packages_open_link(url);
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
#if defined(_WIN32)
    if (g_desktop.tray_initialized)
        system_tray_release(&g_desktop.tray);
    if (g_desktop.published)
        CloseHandle(g_desktop.published);
    if (g_desktop.mutex) {
        if (g_desktop.owns_mutex)
            ReleaseMutex(g_desktop.mutex);
        CloseHandle(g_desktop.mutex);
    }
#else
    if (g_desktop.tray_started)
        dbus::tray_stop();
    if (g_desktop.instance_checked)
        dbus::instance_release();
#endif
    reset_value(&g_desktop);
}

} // namespace maicord::app_detail

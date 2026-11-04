// Integration test of the Wayland window backend against a real compositor (opt-in: tools/test_wayland_window.sh).
//
//   --lifecycle   open a window, see it configured and presented, answer frame callbacks, wake, veto and accept a
//                 close, then create it again. Any compositor.
//   --input       the same start, then drive a virtual pointer and a virtual keyboard through the compositor
//                 (wlroots' virtual-pointer and virtual-keyboard protocols, on a second connection of this process)
//                 and assert the WindowEvents the window gets: motion, buttons, double clicks, capture, wheel,
//                 key codes, characters, compose, repeat and held-key state, under US, German, French, Dvorak and
//                 Russian keymaps. sway and the other wlroots compositors; elsewhere it reports a skip.
//
// The driver script (tools/test_wayland_window.sh) and this program talk through files in $MAICORD_WAYLAND_SYNC_DIR:
// `ready` (written here once the window is up and presented), `origin` ("x y width height": where the window's content
// starts in the compositor's layout and how large the layout is; written by the script, which asks sway) and
// `injected` (lifecycle: the script has taken its screenshot).
//
// Exit status: 0 pass, 1 fail, 77 skipped (no compositor, or one without the protocols needed), as ctest's
// SKIP_RETURN_CODE expects.
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/platform/window/pump.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/wayland/xkb.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "wayland_helper.hpp"
#include "wayland_inject.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

using namespace wayland;

constexpr u32 kRecordCapacity = 16384;
constexpr u32 kNone = 0xFFFFFFFFu;
constexpr u32 kClientWidth = 640;
constexpr u32 kClientHeight = 400;

float width_px() noexcept;  // the window's width in client pixels now

// Linux button and key codes (<linux/input-event-codes.h>) the scenario presses.
constexpr u32 kBtnLeft = 0x110;
constexpr u32 kBtnRight = 0x111;
constexpr u32 kBtnMiddle = 0x112;
constexpr u32 kBtnSide = 0x113;
constexpr u32 kBtnExtra = 0x114;

struct Record {
    WindowEvent event;
    bool captured;     // window_pointer_captured() while the event was handled
    bool key_held;     // window_key_down() for the event's key or button while it was handled
    bool global_held;  // global_key_down() for the event's key
};

Record g_log[kRecordCapacity];
u32 g_count;
u32 g_failures;
Window g_window;
bool g_capture_on_press;
bool g_frame_hits;  // the sink answers hit_test like an app with a caption and a resizable frame
inject::Helper* g_helper;
FrameHit frame_hit_at(float x, float y) noexcept;
bool g_veto_close;
u32 g_frame_callbacks;
u32 g_listener_calls;
u32 g_cursor_queries;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

// The events ---------------------------------------------------------------------------------------------------------

Key key_of_button(u8 button) noexcept {
    switch (static_cast<PointerButton>(button)) {
    case PointerButton::left:
        return key::lbutton;
    case PointerButton::right:
        return key::rbutton;
    case PointerButton::middle:
        return key::mbutton;
    case PointerButton::back:
        return key::xbutton1;
    default:
        return key::xbutton2;
    }
}

void sink(void*, const WindowEvent& event, WindowEventResult* result) noexcept {
    Record* record = g_count < kRecordCapacity ? &g_log[g_count++] : nullptr;
    switch (event.type) {
    case EventType::pointer_down:
        if (g_capture_on_press && event.button == static_cast<u8>(PointerButton::left))
            window_capture_pointer(g_window);
        break;
    case EventType::cursor_query:
        ++g_cursor_queries;
        window_set_cursor(g_window, event.pointer.x < width_px() / 2 ? UiCursor::hand : UiCursor::ibeam);
        result->handled = true;
        break;
    case EventType::hit_test:
        if (g_frame_hits) {
            result->handled = true;
            result->value = static_cast<i32>(frame_hit_at(event.pointer.x, event.pointer.y));
        }
        break;
    case EventType::close_requested:
        result->handled = g_veto_close;
        break;
    case EventType::frame_done:
        ++g_frame_callbacks;
        break;
    default:
        break;
    }
    if (record) {
        record->event = event;
        record->captured = window_pointer_captured(g_window);
        if (event.type == EventType::key_down || event.type == EventType::key_up) {
            record->key_held = window_key_down(g_window, static_cast<Key>(event.key.key));
            record->global_held = global_key_down(static_cast<Key>(event.key.key));
        } else if (event.type == EventType::pointer_down || event.type == EventType::pointer_up) {
            record->key_held = window_key_down(g_window, key_of_button(event.button));
        }
    }
}

void frame_listener(void*, u32) noexcept {
    ++g_listener_calls;
}

template <typename Predicate> u32 find_event(u32 from, EventType type, Predicate predicate) noexcept {
    for (u32 i = from; i < g_count; ++i)
        if (g_log[i].event.type == type && predicate(g_log[i]))
            return i;
    return kNone;
}

u32 count_events(u32 from, EventType type) noexcept {
    u32 count = 0;
    for (u32 i = from; i < g_count; ++i)
        count += g_log[i].event.type == type;
    return count;
}

float absolute(float value) noexcept {
    return value < 0 ? -value : value;
}

float width_px() noexcept {
    return static_cast<float>(window_metrics(g_window).width_px);
}

// Files shared with the driver script ---------------------------------------------------------------------------------

struct Paths {
    char directory[256];
    bool enabled;
};

Paths g_paths;

bool path_in_directory(char* output, u32 capacity, const char* name) noexcept {
    if (!g_paths.enabled)
        return false;
    u32 length = 0;
    for (const char* part = g_paths.directory; *part && length + 1 < capacity; ++part)
        output[length++] = *part;
    if (length + 1 < capacity)
        output[length++] = '/';
    for (const char* part = name; *part && length + 1 < capacity; ++part)
        output[length++] = *part;
    output[length] = 0;
    return true;
}

bool file_exists(const char* name) noexcept {
    char path[320];
    if (!path_in_directory(path, sizeof(path), name))
        return false;
    const long fd = sys_open(path, kOpenReadOnly);
    if (syscall_failed(fd))
        return false;
    sys_close(static_cast<int>(fd));
    return true;
}

void touch_file(const char* name) noexcept {
    char path[320];
    if (!path_in_directory(path, sizeof(path), name))
        return;
    const long fd = sys_open(path, kOpenWriteOnly | kOpenCreate | kOpenTruncate, 0644);
    if (!syscall_failed(fd))
        sys_close(static_cast<int>(fd));
}

// The whitespace-separated integers of a small file; how many there were.
u32 read_numbers(const char* name, i32* values, u32 capacity) noexcept {
    char path[320];
    if (!path_in_directory(path, sizeof(path), name))
        return 0;
    const long fd = sys_open(path, kOpenReadOnly);
    if (syscall_failed(fd))
        return 0;
    char text[128] = {};
    const long length = sys_read(static_cast<int>(fd), text, sizeof(text) - 1);
    sys_close(static_cast<int>(fd));
    u32 count = 0;
    i32 value = 0;
    bool negative = false;
    bool digits = false;
    for (long i = 0; i <= length && count < capacity; ++i) {
        const char c = i < length ? text[i] : ' ';
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '-') {
            negative = true;
        } else if (digits) {
            values[count++] = negative ? -value : value;
            value = 0;
            digits = false;
            negative = false;
        }
    }
    return count;
}

// Pumping ------------------------------------------------------------------------------------------------------------

bool g_quit;
int g_exit_code;

void pump_once(u32 wait_ms) noexcept {
    const WindowWaitResult waited = window_wait(nullptr, 0, wait_ms, 0);
    check(!waited.failed, "window_wait does not fail");
    const WindowPumpHooks hooks{};
    const WindowPumpResult pumped = window_pump(256, 2000, hooks);
    if (pumped.quit) {
        g_quit = true;
        g_exit_code = pumped.exit_code;
    }
}

template <typename Done> bool pump_until(Done done, u32 timeout_ms) noexcept {
    const u64 start = clock_monotonic_milliseconds();
    while (!done()) {
        if (clock_monotonic_milliseconds() - start >= timeout_ms)
            return false;
        pump_once(20);
        if (g_quit && !done())
            return false;
    }
    return true;
}

// Lets the compositor's events arrive: for the assertions that something did not happen.
void settle(u32 milliseconds) noexcept {
    pump_until([] { return false; }, milliseconds);
}

bool arg_is(const char* argument, const char* expected) noexcept {
    while (*argument && *argument == *expected) {
        ++argument;
        ++expected;
    }
    return *argument == *expected;
}

[[noreturn]] void skip(const char* reason) noexcept {
    test_write_text("SKIP ");
    test_write_line(reason);
    sys_exit_group(77);
}

// The compositor's globals against the protocols MaiCord wants: the support matrix.
void print_support_matrix() noexcept {
    struct Wanted {
        const char* name;
        const char* use;
    };
    const Wanted wanted[] = {
        {"wl_compositor", "surfaces (v4+ required)"},
        {"wl_shm", "bring-up clear, cursor fallback"},
        {"xdg_wm_base", "toplevel (required)"},
        {"wl_seat", "pointer and keyboard"},
        {"wp_viewporter", "fractional scale buffers"},
        {"wp_fractional_scale_manager_v1", "fractional scale"},
        {"wp_cursor_shape_manager_v1", "cursor shapes"},
        {"zxdg_decoration_manager_v1", "server-side decorations"},
        {"zwp_text_input_manager_v3", "IME (L7)"},
        {"zwp_primary_selection_device_manager_v1", "primary selection (L6)"},
        {"wl_data_device_manager", "clipboard and drop (L6, L8)"},
        {"xdg_activation_v1", "attention (P9)"},
        {"zwp_idle_inhibit_manager_v1", "idle inhibit (calls)"},
        {"ext_idle_notifier_v1", "system idle (L9)"},
        {"wp_presentation", "frame timing"},
        {"xdg_toplevel_icon_manager_v1", "window icon"},
        {"zwlr_virtual_pointer_manager_v1", "tests: pointer injection"},
        {"zwp_virtual_keyboard_manager_v1", "tests: key injection"},
    };
    test_write_line("protocol support matrix (name, version or absent):");
    for (const Wanted& entry : wanted) {
        u32 found_version = 0;
        bool found = false;
        const char* name = nullptr;
        u32 version = 0;
        for (u32 i = 0; window_wayland_global(i, &name, &version); ++i) {
            if (arg_is(name, entry.name)) {
                found = true;
                found_version = version;
            }
        }
        test_write_text("  ");
        test_write_text(entry.name);
        test_write_text(found ? " v" : " absent");
        if (found)
            test_write_u64(found_version);
        test_write_text("   [");
        test_write_text(entry.use);
        test_write_line("]");
    }
    const WindowCaps& caps = window_caps();
    test_write_text("  caps: csd=");
    test_write_u64(caps.csd);
    test_write_text(" fractional_scale=");
    test_write_u64(caps.fractional_scale);
    test_write_text(" window_menu=");
    test_write_u64(caps.window_menu);
    test_write_text(" maximize=");
    test_write_u64(caps.maximize);
    test_write_text(" minimize=");
    test_write_u64(caps.minimize);
    test_write_text(" fullscreen=");
    test_write_u64(caps.fullscreen);
    const WindowMetrics metrics = window_metrics(g_window);
    test_write_text(" size=");
    test_write_u64(metrics.width_px);
    test_write_text("x");
    test_write_u64(metrics.height_px);
    test_write_text(" scale_120=");
    test_write_u64(metrics.scale_120);
    test_write_line("");
}

bool create_window() noexcept {
    WindowCreateInfo info{};
    info.title = Utf8View{"maicord wayland test", 20};
    info.app_id = Utf8View{"maicord", 7};
    info.client_width_dips = kClientWidth;
    info.client_height_dips = kClientHeight;
    info.visible = true;
    return window_create(info, sink, nullptr, &g_window);
}

// A window that is up, presented and answering frame callbacks.
void test_window_up() noexcept {
    const WindowMetrics metrics = window_metrics(g_window);
    check(metrics.width_px > 0 && metrics.height_px > 0 && metrics.dpi >= 96 && metrics.scale_120 >= 120,
          "metrics after create");
    check(window_surface_native(g_window) != nullptr, "the wl_surface is exposed");
    WaylandSurfaceInfo surface{};
    check(window_wayland_info(g_window, &surface) && surface.display && surface.surface == window_surface_native(g_window) &&
              surface.width_px == metrics.width_px,
          "surface info");

    window_set_frame_listener(g_window, frame_listener, nullptr);
    window_frame_callback_request(g_window);
    const u32 before = g_frame_callbacks;
    check(pump_until([&] { return g_frame_callbacks > before; }, 5000), "a frame callback arrives");
    check(g_listener_calls >= 1, "the frame listener is called");
    check(find_event(0, EventType::resize, [](const Record& r) { return r.event.resize.width_px > 0 && r.event.resize.height_px > 0; }) != kNone,
          "a resize event reports the configured size");
    check(find_event(0, EventType::state_changed, [](const Record& r) { return (r.event.window_state.state & state_visible) != 0; }) != kNone,
          "state_changed reports the window visible");
    window_set_title(g_window, Utf8View{"maicord wayland test (retitled)", 31});
    window_wayland_set_clear_color(g_window, 0x1A598C);
    window_frame_callback_request(g_window);
    const u32 after = g_frame_callbacks;
    check(pump_until([&] { return g_frame_callbacks > after; }, 5000), "a frame callback follows the repaint");
}

void test_lifecycle() noexcept {
    // A wake is one wake event, and window_wait returns for it.
    window_wake(g_window);
    const WindowWaitResult waited = window_wait(nullptr, 0, 2000, 0);
    check(waited.window_input && !waited.failed, "a wake ends window_wait");
    const u32 first_wake = count_events(0, EventType::wake);
    pump_once(0);
    check(count_events(0, EventType::wake) == first_wake + 1, "a wake raises one wake event");

    // A close the app vetoes keeps the window; an unhandled one ends the pump.
    g_veto_close = true;
    window_request_close(g_window);
    pump_once(50);
    check(!g_quit && count_events(0, EventType::close_requested) == 1, "a vetoed close keeps running");
    g_veto_close = false;
    window_request_close(g_window);
    pump_once(50);
    check(g_quit && g_exit_code == 0 && count_events(0, EventType::close_requested) == 2, "an unhandled close quits");
}

// The desktop settings reach the window through the session bus (tools/test_dbus.sh's mock portal, when the script
// provides one): PlatformSettings at the start, then a SettingChanged signal that wakes window_wait and raises a
// settings_changed event.
void set_mock_setting(dbus::Connection* control, const char* ns, const char* key, bool boolean, u32 number) noexcept {
    dbus::Builder body;
    dbus::builder_init(&body);
    dbus::builder_string(&body, ns);
    dbus::builder_string(&body, key);
    if (boolean) {
        dbus::builder_open_variant(&body, "b");
        dbus::builder_bool(&body, number != 0);
    } else {
        dbus::builder_open_variant(&body, "u");
        dbus::builder_u32(&body, number);
    }
    dbus::builder_close_variant(&body);
    const dbus::CallTarget target{"org.maicord.Mock", "/org/maicord/Mock", "org.maicord.Mock", "SetSetting"};
    dbus::Message reply;
    const dbus::Status status = dbus::connection_call_sync(control, target, &body, 3000, &reply);
    dbus::builder_release(&body);
    check(status == dbus::Status::ok, "the mock accepts a setting");
    if (status == dbus::Status::ok || status == dbus::Status::error_reply)
        dbus::message_release(&reply);
}

void test_desktop_settings() noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    if (!bus || !address || !session || !arg_is(bus, "mock") || !arg_is(address, session)) {
        test_write_line("SKIP desktop settings: no mock portal (tools/test_dbus.sh provides one)");
        return;
    }
    const PlatformSettings settings = platform_settings_read();
    check(settings.dark_scheme && settings.double_click_ms == 350 && settings.caret_blink_ms == 500 &&
              !settings.client_area_animation,
          "PlatformSettings read the portal: dark, 350 ms double click, 500 ms blink, no animation");
    check(find_event(0, EventType::settings_changed, [](const Record& r) { return (r.event.settings.areas & 0xF) == 0xF; }) != kNone,
          "the first load raised settings_changed for every area");

    dbus::Connection* control = dbus::connection_open_address(address);
    check(control != nullptr, "a control connection to the mock");
    if (!control)
        return;
    const u32 before = g_count;
    set_mock_setting(control, "org.freedesktop.appearance", "color-scheme", false, 2);
    // The signal wakes the loop: a wait of seconds ends at once.
    const u64 started = clock_monotonic_milliseconds();
    const WindowWaitResult waited = window_wait(nullptr, 0, 5000, 0);
    check(waited.window_input && clock_monotonic_milliseconds() - started < 2500, "bus traffic wakes window_wait");
    pump_until([&] { return find_event(before, EventType::settings_changed, [](const Record&) { return true; }) != kNone; }, 3000);
    const u32 changed = find_event(before, EventType::settings_changed, [](const Record&) { return true; });
    check(changed != kNone && g_log[changed].event.settings.areas == 8, "settings_changed names the scheme area");
    check(!platform_settings_read().dark_scheme, "and the scheme is light now");

    set_mock_setting(control, "org.gnome.desktop.interface", "cursor-blink", true, 0);
    pump_until([] { return platform_settings_read().caret_blink_ms == 0xFFFFFFFFu; }, 3000);
    check(platform_settings_read().caret_blink_ms == 0xFFFFFFFFu, "a caret that does not blink reads as INFINITE");
    dbus::connection_close(control);
}

// The GlobalShortcuts portal (the mock's) holds a key down for global_key_down while the window loop runs: bound with
// window_wayland_global_shortcut, set by the portal's Activated and Deactivated signals, which wake window_wait.
void test_global_shortcut() noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    if (!bus || !address || !session || !arg_is(bus, "mock") || !arg_is(address, session)) {
        test_write_line("SKIP global shortcut: no mock portal (tools/test_dbus.sh provides one)");
        return;
    }
    dbus::Connection* control = dbus::connection_open_address(address);
    check(control != nullptr, "a control connection to the mock");
    if (!control)
        return;
    check(window_wayland_global_shortcut('P', mod_ctrl | mod_alt, "Push to talk"), "a push-to-talk key is bound");
    check(pump_until([] { return window_wayland_global_shortcut_ready(); }, 5000), "the portal binds it");
    check(!global_key_down('P') && !global_key_down('O'), "nothing is held yet");
    const auto set_state = [&](bool active) {
        dbus::Builder body;
        dbus::builder_init(&body);
        dbus::builder_string(&body, "maicord-hold-to-talk");
        dbus::builder_bool(&body, active);
        const dbus::CallTarget target{"org.maicord.Mock", "/org/maicord/Mock", "org.maicord.Mock", "ShortcutState"};
        dbus::Message reply;
        const dbus::Status status = dbus::connection_call_sync(control, target, &body, 3000, &reply);
        dbus::builder_release(&body);
        check(status == dbus::Status::ok, "the mock changes the shortcut's state");
        if (status == dbus::Status::ok || status == dbus::Status::error_reply)
            dbus::message_release(&reply);
    };
    set_state(true);
    check(pump_until([] { return global_key_down('P'); }, 3000), "Activated holds the key down");
    check(!global_key_down('O'), "and only that key");
    set_state(false);
    check(pump_until([] { return !global_key_down('P'); }, 3000), "Deactivated lets it go");
    set_state(true);
    pump_until([] { return global_key_down('P'); }, 3000);
    window_wayland_global_shortcut_clear();
    check(!global_key_down('P') && !window_wayland_global_shortcut_ready(), "clearing the shortcut releases the key");
    dbus::connection_close(control);
}

void test_recreate() noexcept {
    window_destroy(g_window);
    g_window = nullptr;
    g_quit = false;
    g_count = 0;
    check(create_window(), "the window can be created again");
    if (g_window) {
        test_window_up();
        window_destroy(g_window);
        g_window = nullptr;
    }
}

// The input scenario -------------------------------------------------------------------------------------------------

const char* type_name(EventType type) noexcept {
    switch (type) {
    case EventType::pointer_move: return "pointer_move";
    case EventType::pointer_down: return "pointer_down";
    case EventType::pointer_up: return "pointer_up";
    case EventType::pointer_leave: return "pointer_leave";
    case EventType::wheel: return "wheel";
    case EventType::capture_lost: return "capture_lost";
    case EventType::cursor_query: return "cursor_query";
    case EventType::key_down: return "key_down";
    case EventType::key_up: return "key_up";
    case EventType::character: return "character";
    case EventType::focus_changed: return "focus_changed";
    case EventType::activation_changed: return "activation_changed";
    case EventType::state_changed: return "state_changed";
    case EventType::resize: return "resize";
    case EventType::resize_begin: return "resize_begin";
    case EventType::resize_end: return "resize_end";
    case EventType::scale_changed: return "scale_changed";
    case EventType::close_requested: return "close_requested";
    case EventType::frame_done: return "frame_done";
    case EventType::wake: return "wake";
    default: return "other";
    }
}

// What the window saw, one event a line: the first thing to read when an assertion fails.
void dump_log() noexcept {
    test_write_line("-- event trace (type, then key/button/unit, x y, flags, modifiers):");
    for (u32 i = 0; i < g_count && i < 800; ++i) {
        const WindowEvent& e = g_log[i].event;
        if (e.type == EventType::frame_done || e.type == EventType::cursor_query)
            continue;
        test_write_text("  ");
        test_write_u64(i);
        test_write_text(" ");
        test_write_text(type_name(e.type));
        switch (e.type) {
        case EventType::key_down:
        case EventType::key_up:
            test_write_text(" key=");
            test_write_u64(e.key.key);
            test_write_text(" scancode=");
            test_write_u64(e.key.scancode);
            break;
        case EventType::character:
            test_write_text(" unit=");
            test_write_u64(e.character.unit);
            break;
        case EventType::pointer_down:
        case EventType::pointer_up:
            test_write_text(" button=");
            test_write_u64(e.button);
            [[fallthrough]];
        case EventType::pointer_move:
            test_write_text(" x=");
            test_write_u64(static_cast<u64>(static_cast<i64>(e.pointer.x)));
            test_write_text(" y=");
            test_write_u64(static_cast<u64>(static_cast<i64>(e.pointer.y)));
            test_write_text(" buttons=");
            test_write_u64(e.pointer.buttons);
            break;
        case EventType::wheel:
            test_write_text(" dx120=");
            test_write_u64(static_cast<u64>(static_cast<i64>(e.wheel.dx120)));
            test_write_text(" dy120=");
            test_write_u64(static_cast<u64>(static_cast<i64>(e.wheel.dy120)));
            test_write_text(" source=");
            test_write_u64(e.wheel.source);
            break;
        case EventType::focus_changed:
        case EventType::activation_changed:
            test_write_text(e.focus.gained ? " gained" : " lost");
            break;
        default:
            break;
        }
        test_write_text(" flags=");
        test_write_u64(e.flags);
        test_write_text(" mods=");
        test_write_u64(e.modifiers);
        test_write_line("");
    }
}

inject::Injector g_injector;
i32 g_origin_x;
i32 g_origin_y;
u32 g_mark;  // the first record of the stage in progress

// Pumps until an event of `type` after the mark satisfies `predicate`; its index or kNone, and the check.
template <typename Predicate>
u32 expect(const char* label, EventType type, Predicate predicate, u32 timeout_ms = 2500) noexcept {
    pump_until([&] { return find_event(g_mark, type, predicate) != kNone; }, timeout_ms);
    const u32 index = find_event(g_mark, type, predicate);
    check(index != kNone, label);
    return index;
}

void mark() noexcept {
    g_mark = g_count;
}

// Client pixels of the window's content, moved into layout coordinates (the compositor's, in logical units).
void pointer_to(i32 x, i32 y) noexcept {
    const u32 dpi = window_metrics(g_window).dpi;
    const i32 logical_x = (x * 96 + static_cast<i32>(dpi) / 2) / static_cast<i32>(dpi);
    const i32 logical_y = (y * 96 + static_cast<i32>(dpi) / 2) / static_cast<i32>(dpi);
    inject::pointer_move_to(&g_injector, static_cast<u32>(g_origin_x + logical_x), static_cast<u32>(g_origin_y + logical_y));
}

bool near_point(const Record& r, float x, float y) noexcept {
    return absolute(r.event.pointer.x - x) <= 1.5f && absolute(r.event.pointer.y - y) <= 1.5f;
}

bool is_button(const Record& r, PointerButton button) noexcept {
    return r.event.button == static_cast<u8>(button);
}

void test_pointer() noexcept {
    g_capture_on_press = true;

    // Motion: client pixels, with a cursor query after it; the position is readable.
    mark();
    pointer_to(100, 80);
    expect("pointer motion to (100, 80)", EventType::pointer_move, [](const Record& r) { return near_point(r, 100, 80); });
    expect("a cursor query follows the motion", EventType::cursor_query, [](const Record& r) { return near_point(r, 100, 80); });
    float x = 0;
    float y = 0;
    check(window_pointer_position(g_window, &x, &y) && absolute(x - 100) <= 1.5f && absolute(y - 80) <= 1.5f,
          "the pointer position is readable and inside");

    // A click: press, release; the button reads as down while its press is handled, and is up after.
    mark();
    inject::pointer_button(&g_injector, kBtnLeft, true);
    const u32 down = expect("left press", EventType::pointer_down, [](const Record& r) {
        return is_button(r, PointerButton::left) && (r.event.pointer.buttons & 1) != 0 && near_point(r, 100, 80);
    });
    check(down != kNone && (g_log[down].event.flags & ev_double_click) == 0, "the first press is not a double click");
    check(down != kNone && g_log[down].key_held && g_log[down].captured, "the press reads as held and captured");
    inject::pointer_button(&g_injector, kBtnLeft, false);
    const u32 up = expect("left release", EventType::pointer_up, [](const Record& r) {
        return is_button(r, PointerButton::left) && r.event.pointer.buttons == 0;
    });
    check(up != kNone && !g_log[up].key_held, "the release reads as up");
    check(expect("the capture ends with the last release", EventType::capture_lost, [](const Record&) { return true; }) != kNone &&
              !window_pointer_captured(g_window),
          "capture_lost, and no capture afterwards");

    // A double click: press, release, press quickly; the second press carries ev_double_click, the third does not.
    settle(700);
    mark();
    for (int i = 0; i < 2; ++i) {
        inject::pointer_button(&g_injector, kBtnLeft, true);
        inject::pointer_button(&g_injector, kBtnLeft, false);
    }
    expect("the second quick press is a double click", EventType::pointer_down, [](const Record& r) {
        return is_button(r, PointerButton::left) && (r.event.flags & ev_double_click) != 0;
    });
    mark();
    inject::pointer_button(&g_injector, kBtnLeft, true);
    inject::pointer_button(&g_injector, kBtnLeft, false);
    expect("the press after a double click is a plain one", EventType::pointer_down, [](const Record& r) {
        return is_button(r, PointerButton::left) && (r.event.flags & ev_double_click) == 0;
    });
    settle(100);

    // The other buttons.
    const struct {
        u32 code;
        PointerButton button;
        const char* down;
        const char* up;
    } buttons[] = {
        {kBtnRight, PointerButton::right, "right press", "right release"},
        {kBtnMiddle, PointerButton::middle, "middle press", "middle release"},
        {kBtnSide, PointerButton::back, "back (side) press", "back (side) release"},
        {kBtnExtra, PointerButton::forward, "forward (extra) press", "forward (extra) release"},
    };
    for (const auto& entry : buttons) {
        mark();
        inject::pointer_button(&g_injector, entry.code, true);
        const PointerButton button = entry.button;
        const u32 pressed = expect(entry.down, EventType::pointer_down, [&](const Record& r) { return is_button(r, button); });
        check(pressed != kNone && g_log[pressed].key_held && (g_log[pressed].event.pointer.buttons & (1u << static_cast<u8>(button))) != 0,
              "the button reads as held with its bit set");
        inject::pointer_button(&g_injector, entry.code, false);
        expect(entry.up, EventType::pointer_up, [&](const Record& r) { return is_button(r, button); });
        if (button == PointerButton::right) {
            // Unhandled, the right release becomes the context menu, as DefWindowProc does on Windows.
            const u32 menu = expect("a right click the app left alone raises context_menu", EventType::context_menu,
                                    [](const Record&) { return true; });
            check(menu != kNone && (g_log[menu].event.flags & ev_keyboard) == 0, "the pointer context menu has no keyboard flag");
        }
    }

    // Two buttons: the capture lasts until the last one is up.
    settle(100);
    mark();
    inject::pointer_button(&g_injector, kBtnLeft, true);
    inject::pointer_button(&g_injector, kBtnRight, true);
    expect("right press while left is held", EventType::pointer_down, [](const Record& r) {
        return is_button(r, PointerButton::right) && r.event.pointer.buttons == 3;
    });
    inject::pointer_button(&g_injector, kBtnLeft, false);
    expect("left release while right is held", EventType::pointer_up, [](const Record& r) {
        return is_button(r, PointerButton::left) && r.event.pointer.buttons == 2;
    });
    check(window_pointer_captured(g_window) && count_events(g_mark, EventType::capture_lost) == 0,
          "the capture holds while a button is down");
    inject::pointer_button(&g_injector, kBtnRight, false);
    expect("the last release ends the capture", EventType::capture_lost, [](const Record&) { return true; });

    // An explicit release raises capture_lost at once, and the release of the button adds none.
    settle(100);
    mark();
    inject::pointer_button(&g_injector, kBtnLeft, true);
    expect("press to capture", EventType::pointer_down, [](const Record& r) { return is_button(r, PointerButton::left); });
    check(window_pointer_captured(g_window), "captured after the press");
    window_release_capture(g_window);
    check(!window_pointer_captured(g_window) && count_events(g_mark, EventType::capture_lost) == 1,
          "window_release_capture raises capture_lost synchronously");
    inject::pointer_button(&g_injector, kBtnLeft, false);
    expect("release after the explicit release", EventType::pointer_up, [](const Record& r) { return is_button(r, PointerButton::left); });
    check(count_events(g_mark, EventType::capture_lost) == 1, "no second capture_lost");

    // A drag out of the window: motion and the release keep arriving past the right edge.
    settle(100);
    mark();
    pointer_to(50, 50);
    expect("motion to (50, 50)", EventType::pointer_move, [](const Record& r) { return near_point(r, 50, 50); });
    inject::pointer_button(&g_injector, kBtnLeft, true);
    const u32 drag = expect("drag press", EventType::pointer_down, [](const Record& r) { return is_button(r, PointerButton::left); });
    check(drag != kNone && g_log[drag].captured, "the drag press is captured");
    pointer_to(300, 50);
    pointer_to(static_cast<i32>(width_px()) + 60, 50);
    expect("motion outside the window continues while the button is held", EventType::pointer_move, [](const Record& r) {
        return r.event.pointer.x > width_px() && (r.event.pointer.buttons & 1) != 0;
    });
    inject::pointer_button(&g_injector, kBtnLeft, false);
    expect("the release arrives outside the window", EventType::pointer_up, [](const Record& r) {
        return r.event.pointer.x > width_px();
    });
    expect("the drag's capture ends", EventType::capture_lost, [](const Record&) { return true; });
    expect("the pointer leaves", EventType::pointer_leave, [](const Record&) { return true; });
    check(!window_pointer_position(g_window, nullptr, nullptr), "the pointer is outside afterwards");

    // The wheel: a notch is 120, down is negative as on Windows; continuous scrolling carries pixels as well.
    mark();
    pointer_to(100, 80);
    expect("the pointer comes back", EventType::pointer_move, [](const Record& r) { return near_point(r, 100, 80); });
    mark();
    inject::pointer_notch(&g_injector, false, 1);
    expect("a notch down", EventType::wheel, [](const Record& r) {
        return r.event.wheel.dy120 == -120 && r.event.wheel.dx120 == 0 && r.event.wheel.source == wl_pointer_axis_source_wheel &&
               absolute(r.event.wheel.x - 100) <= 1.5f;
    });
    mark();
    inject::pointer_notch(&g_injector, false, -2);
    expect("two notches up", EventType::wheel, [](const Record& r) { return r.event.wheel.dy120 == 240 && r.event.wheel.dx120 == 0; });
    mark();
    inject::pointer_notch(&g_injector, true, 1);
    expect("a notch right", EventType::wheel, [](const Record& r) { return r.event.wheel.dx120 == 120 && r.event.wheel.dy120 == 0; });
    mark();
    inject::pointer_scroll(&g_injector, false, 30);
    expect("continuous scroll down", EventType::wheel, [](const Record& r) {
        // (wlroots reports a virtual pointer's finger source as a wheel one: the source is not asserted.)
        return r.event.wheel.dy120 == -240 && r.event.wheel.dx120 == 0 &&
               absolute(r.event.wheel.dy_px + 30.0f * static_cast<float>(window_metrics(g_window).dpi) / 96.0f) < 0.5f;
    });
    check(g_cursor_queries >= 3, "cursor queries followed the motion");
}

// The compositor serves `layout` (and variant) to the window: the keymap goes through a virtual keyboard.
bool use_layout(const char* layout, const char* variant) noexcept {
    const wayland::XkbApi* api = wayland::xkb_api();
    wayland::XkbContext* context = api ? api->context_new(0) : nullptr;
    if (!context)
        return false;
    api->context_set_log_level(context, wayland::kXkbLogLevelCritical);
    const wayland::XkbRuleNames names{"evdev", "pc105", layout, variant, nullptr};
    wayland::XkbKeymap* keymap = api->keymap_new_from_names(context, &names, 0);
    char* text = keymap ? api->keymap_get_as_string(keymap, wayland::kXkbKeymapFormatTextV1) : nullptr;
    bool ok = false;
    if (text) {
        u32 length = 0;
        while (text[length])
            ++length;
        ok = inject::keyboard_set_keymap(&g_injector, text, length);
        free(text);
    }
    if (keymap)
        api->keymap_unref(keymap);
    api->context_unref(context);
    return ok;
}

// Presses and releases one key and waits for both.
struct Typed {
    u32 down;
    u32 up;
};

Typed type_key(u32 evdev, Key expected) noexcept {
    mark();
    inject::keyboard_key(&g_injector, evdev, true);
    Typed typed{};
    typed.down = expect("key down", EventType::key_down, [&](const Record& r) {
        return r.event.key.key == expected && (r.event.flags & ev_repeat) == 0;
    });
    inject::keyboard_key(&g_injector, evdev, false);
    typed.up = expect("key up", EventType::key_up, [&](const Record& r) { return r.event.key.key == expected; });
    return typed;
}

// The text a key produced between its press and release.
u32 text_of(const Typed& typed) noexcept {
    if (typed.down == kNone || typed.up == kNone)
        return 0;
    for (u32 i = typed.down + 1; i < typed.up; ++i)
        if (g_log[i].event.type == EventType::character)
            return g_log[i].event.character.unit;
    return 0;
}

// A key with the layout of the moment: its virtual key and its character.
void expect_key(const char* label, u32 evdev, Key key_code, u32 text) noexcept {
    const Typed typed = type_key(evdev, key_code);
    check(typed.down != kNone && typed.up != kNone && text_of(typed) == text, label);
}

// The keys DefWindowProc turns into a context menu when the app leaves them alone: Shift+F10 and the Menu key, on the press.
void test_context_menu_keys() noexcept {
    check(use_layout("us", nullptr), "the US keymap for the context menu keys");
    mark();
    inject::keyboard_key(&g_injector, 42, true);  // left shift
    inject::keyboard_key(&g_injector, 68, true);  // F10
    inject::keyboard_key(&g_injector, 68, false);
    inject::keyboard_key(&g_injector, 42, false);
    const u32 menu = expect("Shift+F10 raises context_menu", EventType::context_menu, [](const Record&) { return true; });
    const u32 pressed = find_event(g_mark, EventType::key_down, [](const Record& r) { return r.event.key.key == key::f10; });
    const u32 released = find_event(g_mark, EventType::key_up, [](const Record& r) { return r.event.key.key == key::f10; });
    check(menu != kNone && (g_log[menu].event.flags & ev_keyboard) != 0, "it carries the keyboard flag");
    check(menu != kNone && pressed != kNone && menu > pressed && (released == kNone || menu < released),
          "it comes on the press, inside that key's dispatch");
    settle(50);
    mark();
    inject::keyboard_key(&g_injector, 127, true);  // Menu
    inject::keyboard_key(&g_injector, 127, false);
    const u32 by_menu_key = expect("the Menu key raises context_menu", EventType::context_menu, [](const Record&) { return true; });
    check(by_menu_key != kNone && (g_log[by_menu_key].event.flags & ev_keyboard) != 0, "the Menu key's one is a keyboard one");
    settle(50);
    mark();
    inject::keyboard_key(&g_injector, 68, true);  // F10 alone
    inject::keyboard_key(&g_injector, 68, false);
    settle(150);
    check(count_events(g_mark, EventType::context_menu) == 0, "F10 alone raises no context menu");
}

void test_keyboard() noexcept {
    // US.
    check(use_layout("us", nullptr), "the US keymap reaches the compositor");
    mark();
    inject::keyboard_key(&g_injector, 30, true);
    const u32 first = expect("a first key arrives with the new keymap", EventType::key_down, [](const Record& r) { return r.event.key.key == 'A'; });
    check(first != kNone && g_log[first].key_held && g_log[first].global_held && window_key_down(g_window, 'A') &&
              global_key_down('A'),
          "a pressed key reads as down in the window and in the global keys");
    check(first != kNone && g_log[first].event.key.scancode == 30, "the scancode is the evdev code");
    inject::keyboard_key(&g_injector, 30, false);
    expect("its release", EventType::key_up, [](const Record& r) { return r.event.key.key == 'A'; });
    settle(50);
    check(!window_key_down(g_window, 'A') && !global_key_down('A'), "released keys read as up");

    expect_key("us: a types a", 30, 'A', 'a');
    expect_key("us: 1 types 1", 2, '1', '1');
    expect_key("us: minus", 12, key::minus, '-');
    expect_key("us: Return is key::enter and types a carriage return", 28, key::enter, '\r');
    expect_key("us: Backspace", 14, key::backspace, '\b');
    expect_key("us: Tab", 15, key::tab, '\t');
    expect_key("us: Escape", 1, key::escape, 0x1B);
    expect_key("us: Space", 57, key::space, ' ');
    expect_key("us: Delete is key::del and types nothing", 111, key::del, 0);
    expect_key("us: F1 types nothing", 59, key::f1, 0);
    expect_key("us: Up types nothing", 103, key::up, 0);
    expect_key("us: Home types nothing", 102, key::home, 0);

    // Shift, Ctrl and Alt are the generic codes; chords carry the modifiers.
    mark();
    inject::keyboard_key(&g_injector, 42, true);
    expect("Shift is key::shift", EventType::key_down, [](const Record& r) { return r.event.key.key == key::shift; });
    check(window_key_down(g_window, key::shift) && window_key_down(g_window, key::left_shift) && !window_key_down(g_window, key::right_shift),
          "left shift reads as shift and left shift");
    mark();
    inject::keyboard_key(&g_injector, 30, true);
    const u32 shifted = expect("a under Shift", EventType::key_down, [](const Record& r) {
        return r.event.key.key == 'A' && (r.event.modifiers & mod_shift) != 0;
    });
    check(shifted != kNone && find_event(shifted + 1, EventType::character, [](const Record& r) { return r.event.character.unit == 'A'; }) != kNone,
          "Shift+a types A");
    inject::keyboard_key(&g_injector, 30, false);
    inject::keyboard_key(&g_injector, 42, false);
    settle(50);
    check(!window_key_down(g_window, key::shift) && (window_modifiers(g_window) & mod_shift) == 0, "Shift released");

    mark();
    inject::keyboard_key(&g_injector, 29, true);
    expect("Ctrl is key::ctrl", EventType::key_down, [](const Record& r) { return r.event.key.key == key::ctrl; });
    mark();
    inject::keyboard_key(&g_injector, 30, true);
    const u32 chord = expect("a under Ctrl", EventType::key_down, [](const Record& r) {
        return r.event.key.key == 'A' && (r.event.modifiers & mod_ctrl) != 0 && (r.event.flags & ev_system) == 0;
    });
    check(chord != kNone && find_event(chord + 1, EventType::character, [](const Record& r) { return r.event.character.unit == 0x01; }) != kNone,
          "Ctrl+a types the control code U+0001");
    inject::keyboard_key(&g_injector, 30, false);
    inject::keyboard_key(&g_injector, 29, false);

    mark();
    inject::keyboard_key(&g_injector, 56, true);
    const u32 alt = expect("Alt is key::alt and a system key", EventType::key_down, [](const Record& r) {
        return r.event.key.key == key::alt && (r.event.flags & ev_system) != 0;
    });
    check(alt != kNone, "alt is flagged as a system key");
    mark();
    inject::keyboard_key(&g_injector, 30, true);
    const u32 alt_a = expect("a under Alt", EventType::key_down, [](const Record& r) {
        return r.event.key.key == 'A' && (r.event.modifiers & mod_alt) != 0 && (r.event.flags & ev_system) != 0;
    });
    check(alt_a != kNone && find_event(alt_a + 1, EventType::character, [](const Record& r) {
                                return r.event.character.unit == 'a' && (r.event.flags & ev_system) != 0;
                            }) != kNone,
          "Alt+a is a system key with its character");
    inject::keyboard_key(&g_injector, 30, false);
    inject::keyboard_key(&g_injector, 56, false);

    // Caps Lock: the toggle state, and its effect on text.
    check(!window_toggle_state(g_window, key::caps_lock), "caps lock starts off");
    mark();
    inject::keyboard_key(&g_injector, 58, true);
    inject::keyboard_key(&g_injector, 58, false);
    expect("Caps Lock is key::caps_lock", EventType::key_down, [](const Record& r) { return r.event.key.key == key::caps_lock; });
    settle(100);
    check(window_toggle_state(g_window, key::caps_lock) && (window_modifiers(g_window) & mod_caps) != 0, "caps lock is on");
    expect_key("us: a types A under Caps Lock", 30, 'A', 'A');
    inject::keyboard_key(&g_injector, 58, true);
    inject::keyboard_key(&g_injector, 58, false);
    settle(100);
    check(!window_toggle_state(g_window, key::caps_lock), "caps lock is off again");

    // Several keys held at once.
    inject::keyboard_key(&g_injector, 30, true);
    inject::keyboard_key(&g_injector, 31, true);
    settle(100);
    check(window_key_down(g_window, 'A') && window_key_down(g_window, 'S') && !window_key_down(g_window, 'D'),
          "two held keys read as down");
    inject::keyboard_key(&g_injector, 30, false);
    settle(100);
    check(!window_key_down(g_window, 'A') && window_key_down(g_window, 'S'), "one released, one held");
    inject::keyboard_key(&g_injector, 31, false);
    settle(50);

    // Repeat: a held key repeats after the delay with its character, until it is released.
    mark();
    inject::keyboard_key(&g_injector, 30, true);
    settle(1300);
    u32 repeats = 0;
    u32 repeated_text = 0;
    for (u32 i = g_mark; i < g_count; ++i) {
        const WindowEvent& e = g_log[i].event;
        repeats += e.type == EventType::key_down && e.key.key == 'A' && (e.flags & ev_repeat) != 0;
        repeated_text += e.type == EventType::character && e.character.unit == 'a' && (e.flags & ev_repeat) != 0;
    }
    check(repeats >= 3, "a held key repeats");
    check(repeated_text == repeats, "each repeat types its character");
    inject::keyboard_key(&g_injector, 30, false);
    expect("the held key's release", EventType::key_up, [](const Record& r) { return r.event.key.key == 'A'; });
    const u32 after_release = g_count;
    settle(300);
    check(count_events(after_release, EventType::key_down) == 0, "repeat stops at the release");
    mark();
    inject::keyboard_key(&g_injector, 29, true);  // a modifier never repeats
    settle(900);
    check(count_events(g_mark, EventType::key_down) == 1, "a modifier does not repeat");
    inject::keyboard_key(&g_injector, 29, false);
    settle(50);

    // German: Z and Y trade places, AltGr is its own modifier and types symbols.
    check(use_layout("de", nullptr), "the German keymap reaches the compositor");
    expect_key("de: the y position types z", 21, 'Z', 'z');
    expect_key("de: the z position types y", 44, 'Y', 'y');
    expect_key("de: the u-umlaut key keeps its position", 26, key::left_bracket, 0xFC);
    mark();
    inject::keyboard_key(&g_injector, 100, true);
    expect("AltGr is the right alt", EventType::key_down, [](const Record& r) { return r.event.key.key == key::right_alt; });
    settle(80);
    check(window_key_down(g_window, key::right_alt) && (window_modifiers(g_window) & mod_altgr) != 0 &&
              (window_modifiers(g_window) & (mod_ctrl | mod_alt)) == 0,
          "AltGr is mod_altgr, neither ctrl nor alt");
    mark();
    inject::keyboard_key(&g_injector, 16, true);
    const u32 at = expect("q under AltGr", EventType::key_down, [](const Record& r) {
        return r.event.key.key == 'Q' && (r.event.modifiers & mod_altgr) != 0 && (r.event.flags & ev_system) == 0;
    });
    check(at != kNone && find_event(at + 1, EventType::character, [](const Record& r) { return r.event.character.unit == '@'; }) != kNone,
          "AltGr+q types @");
    inject::keyboard_key(&g_injector, 16, false);
    inject::keyboard_key(&g_injector, 100, false);
    settle(50);
    check(window_layout_types_text(g_window, 'Q', 0) && window_layout_types_text(g_window, 'Q', mod_altgr) &&
              !window_layout_types_text(g_window, 'Q', mod_ctrl) && !window_layout_types_text(g_window, key::f1, 0),
          "the layout types text for q, with AltGr, not with Ctrl, not for F1");

    // French AZERTY: the digit row types symbols; the key is still the digit.
    check(use_layout("fr", nullptr), "the French keymap reaches the compositor");
    expect_key("fr: the 1 key types &", 2, '1', '&');
    expect_key("fr: the q position types a", 16, 'A', 'a');
    mark();
    inject::keyboard_key(&g_injector, 42, true);
    inject::keyboard_key(&g_injector, 2, true);
    const u32 digit = expect("fr: Shift+1", EventType::key_down, [](const Record& r) { return r.event.key.key == '1'; });
    check(digit != kNone && find_event(digit + 1, EventType::character, [](const Record& r) { return r.event.character.unit == '1'; }) != kNone,
          "fr: Shift+& types 1");
    inject::keyboard_key(&g_injector, 2, false);
    inject::keyboard_key(&g_injector, 42, false);

    // Dvorak and Russian.
    check(use_layout("us", "dvorak"), "the Dvorak keymap reaches the compositor");
    expect_key("dvorak: the s position types o", 31, 'O', 'o');
    expect_key("dvorak: the q position types an apostrophe", 16, key::quote, '\'');
    check(use_layout("ru", nullptr), "the Russian keymap reaches the compositor");
    expect_key("ru: the a position is the A key and types a Cyrillic ef", 30, 'A', 0x444);
    expect_key("ru: digits stay digits", 2, '1', '1');

    // Compose and dead keys: US international's dead acute and a.
    check(use_layout("us", "intl"), "the US international keymap reaches the compositor");
    mark();
    inject::keyboard_key(&g_injector, 40, true);
    inject::keyboard_key(&g_injector, 40, false);
    expect("the dead key is a key", EventType::key_up, [](const Record& r) { return r.event.key.key == key::quote; });
    settle(80);
    check(count_events(g_mark, EventType::character) == 0, "a dead key types nothing by itself");
    inject::keyboard_key(&g_injector, 30, true);
    expect("a after the dead key", EventType::character, [](const Record& r) { return r.event.character.unit == 0xE1; });
    inject::keyboard_key(&g_injector, 30, false);
    settle(50);
    // AltGr+5 is the euro sign.
    mark();
    inject::keyboard_key(&g_injector, 100, true);
    inject::keyboard_key(&g_injector, 6, true);
    expect("AltGr+5 types the euro sign", EventType::character, [](const Record& r) { return r.event.character.unit == 0x20AC; });
    inject::keyboard_key(&g_injector, 6, false);
    inject::keyboard_key(&g_injector, 100, false);
    settle(50);
}

// Client-side decorations -------------------------------------------------------------------------------------------

// The app's frame as the sink reports it: an 8 px band on the edges, a 32 px caption with a 24 px system-menu corner.
FrameHit frame_hit_at(float x, float y) noexcept {
    const float w = width_px();
    const float h = static_cast<float>(window_metrics(g_window).height_px);
    if (x < 0 || y < 0 || x >= w || y >= h)
        return FrameHit::client;
    constexpr float band = 8;
    const bool left = x < band, right = x >= w - band, top = y < band, bottom = y >= h - band;
    if (top && left)
        return FrameHit::top_left;
    if (top && right)
        return FrameHit::top_right;
    if (bottom && left)
        return FrameHit::bottom_left;
    if (bottom && right)
        return FrameHit::bottom_right;
    if (left)
        return FrameHit::left;
    if (right)
        return FrameHit::right;
    if (top)
        return FrameHit::top;
    if (bottom)
        return FrameHit::bottom;
    if (y < 32)
        return x < 24 ? FrameHit::system_menu : FrameHit::caption;
    return FrameHit::client;
}

void test_frame() noexcept {
    g_frame_hits = true;
    g_capture_on_press = false;
    settle(300);
    check(window_caps().csd, "the compositor left the frame to the app (client-side decorations)");
    const u32 original_width = static_cast<u32>(width_px());

    // A press on the caption belongs to the window: it starts the compositor's move, the app sees no press and no
    // release. Moving the pointer away and back carries the window there and back.
    mark();
    pointer_to(300, 12);
    expect("motion over the caption is delivered", EventType::pointer_move, [](const Record& r) { return near_point(r, 300, 12); });
    inject::pointer_button(&g_injector, kBtnLeft, true);
    settle(200);  // the window starts the move on the press; the pointer moves afterwards
    pointer_to(360, 52);
    pointer_to(300, 12);
    inject::pointer_button(&g_injector, kBtnLeft, false);
    settle(300);
    check(count_events(g_mark, EventType::pointer_down) == 0 && count_events(g_mark, EventType::pointer_up) == 0,
          "a press on the caption starts a move and the app sees neither press nor release");
    mark();
    pointer_to(100, 80);
    expect("the window is back where it was", EventType::pointer_move, [](const Record& r) { return near_point(r, 100, 80); });

    // A double press on the caption asks for maximize; whether the compositor honours it for a floating window is its
    // business: the app must not see the presses.
    settle(700);
    mark();
    pointer_to(300, 12);
    for (int i = 0; i < 2; ++i) {
        inject::pointer_button(&g_injector, kBtnLeft, true);
        inject::pointer_button(&g_injector, kBtnLeft, false);
    }
    settle(400);
    check(count_events(g_mark, EventType::pointer_down) == 0, "a double press on the caption is the window's");
    if (find_event(g_mark, EventType::state_changed, [](const Record& r) { return (r.event.window_state.state & state_maximized) != 0; }) != kNone) {
        window_toggle_maximize(g_window);  // the compositor maximized it: put it back
        settle(500);
    }

    // The window menu is a right press on the caption; it is taken, too.
    mark();
    inject::pointer_button(&g_injector, kBtnRight, true);
    inject::pointer_button(&g_injector, kBtnRight, false);
    settle(200);
    check(count_events(g_mark, EventType::pointer_down) == 0, "a right press on the caption opens the window menu");

    // An edge starts a resize: drag the right edge in, then out again.
    settle(300);
    mark();
    const i32 edge = static_cast<i32>(original_width) - 3;
    pointer_to(edge, 200);
    expect("motion to the right edge", EventType::pointer_move, [&](const Record& r) { return near_point(r, static_cast<float>(edge), 200); });
    inject::pointer_button(&g_injector, kBtnLeft, true);
    settle(200);  // the window starts the resize on the press; the pointer moves afterwards
    pointer_to(edge - 60, 200);
    const u32 shrunk = expect("the window shrinks while the edge is dragged", EventType::resize, [&](const Record& r) {
        return r.event.resize.width_px < original_width;
    });
    check(shrunk != kNone && find_event(g_mark, EventType::resize_begin, [](const Record&) { return true; }) != kNone,
          "resize_begin brackets the resize");
    inject::pointer_button(&g_injector, kBtnLeft, false);
    expect("resize_end when the grab ends", EventType::resize_end, [](const Record&) { return true; });
    check(count_events(g_mark, EventType::pointer_down) == 0, "the edge press is the window's");
    settle(200);
    const i32 inner = static_cast<i32>(width_px()) - 3;
    mark();
    pointer_to(inner, 200);
    settle(100);
    inject::pointer_button(&g_injector, kBtnLeft, true);
    settle(200);  // the window starts the resize on the press; the pointer moves afterwards
    pointer_to(inner + 60, 200);
    expect("the window grows back", EventType::resize, [&](const Record& r) { return r.event.resize.width_px == original_width; });
    inject::pointer_button(&g_injector, kBtnLeft, false);
    expect("resize_end again", EventType::resize_end, [](const Record&) { return true; });
    settle(200);
    g_frame_hits = false;
    mark();
    pointer_to(100, 80);
    expect("the pointer is back in the client area", EventType::pointer_move, [](const Record& r) { return near_point(r, 100, 80); });
    g_capture_on_press = true;
}

// The clipboard and the primary selection, against the helper client (a clipboard manager) --------------------------------

// Pumps the window while reading the pipe `fd` to its end: what a selection the window owns answers through its own events.
u32 read_pumped(int fd, u8* buffer, u32 capacity, u32 timeout_ms) noexcept {
    u32 used = 0;
    const u64 start = clock_monotonic_milliseconds();
    while (fd >= 0 && clock_monotonic_milliseconds() - start < timeout_ms) {
        pump_once(10);
        PollFd poll{fd, kPollIn, 0};
        const TimeSpec none{0, 0};
        if (sys_ppoll(&poll, 1, &none) <= 0)
            continue;
        if (used == capacity)
            break;
        const long count = sys_read(fd, buffer + used, capacity - used);
        if (count <= 0)
            break;  // 0: the writer closed the pipe
        used += static_cast<u32>(count);
    }
    if (fd >= 0)
        sys_close(fd);
    return used;
}

bool text_matches(const u8* bytes, u32 count, const char* expected) noexcept {
    u32 length = 0;
    while (expected[length])
        ++length;
    return count == length && bytes_equal(bytes, expected, length);
}

bool wide_path_is(const DroppedFiles* files, u32 index, const char* ascii_expected, u32 extra_unit = 0, u32 extra_at = 0xFFFFFFFF) noexcept {
    wchar_t buffer[256];
    const u32 length = dropped_files_path(files, index, Slice<wchar_t>{buffer, 256});
    u32 expected_length = 0;
    while (ascii_expected[expected_length])
        ++expected_length;
    if (length != expected_length)
        return false;
    for (u32 i = 0; i < length; ++i) {
        const u32 want = i == extra_at ? extra_unit : static_cast<u8>(ascii_expected[i]);
        if (static_cast<u16>(buffer[i]) != want)
            return false;
    }
    return true;
}

void test_clipboard() noexcept {
    inject::Helper& helper = *g_helper;
    if (!helper.data_manager) {
        test_write_line("SKIP clipboard: the compositor has no wlr-data-control for the helper to play a clipboard manager");
        return;
    }
    constexpr char kText[] = "h\xC3\xA9llo \xF0\x9F\x98\x80 clipboard";
    const Utf8View text{kText, sizeof(kText) - 1};

    // The window writes text: another client sees the formats and reads the bytes.
    u32 events = helper.selection_events;
    check(clipboard_write_utf8(text), "clipboard_write_utf8 after input");
    check(inject::helper_wait([&] { return helper.selection_events > events; }, 2000) && helper.selection &&
              (helper.selection->mimes & 1) && (helper.selection->mimes & 2) && (helper.selection->mimes & 4),
          "the clipboard manager sees the text formats");
    u8 buffer[256] = {};
    u32 count = read_pumped(inject::helper_receive(&helper, false, "text/plain;charset=utf-8"), buffer, sizeof(buffer), 2000);
    check(text_matches(buffer, count, kText), "the clipboard manager reads the text the window wrote");
    count = read_pumped(inject::helper_receive(&helper, false, "UTF8_STRING"), buffer, sizeof(buffer), 2000);
    check(text_matches(buffer, count, kText), "UTF8_STRING carries it too");
    char read_back[64];
    ClipboardTextResult result = clipboard_read_utf8(Slice<char>{read_back, sizeof(read_back)});
    check(clipboard_has_text() && result.ok() && text_matches(reinterpret_cast<u8*>(read_back), result.bytes, kText),
          "the window reads its own clipboard back from memory");
    result = clipboard_read_utf8(Slice<char>{read_back, 4});
    check(result.error == ClipboardTextError::capacity, "a clipboard that does not fit reports capacity");

    // The window writes an image: PNG, and the linked-image HTML.
    u8 rgba[8 * 8 * 4];
    for (u32 i = 0; i < 64; ++i) {
        rgba[i * 4] = static_cast<u8>(i * 4);
        rgba[i * 4 + 1] = static_cast<u8>(255 - i * 4);
        rgba[i * 4 + 2] = 40;
        rgba[i * 4 + 3] = 255;
    }
    events = helper.selection_events;
    constexpr char kUrl[] = "https://cdn.example/image.png?x=1&y=2";
    check(clipboard_write_image(rgba, 8, 8, 32, Utf8View{kUrl, sizeof(kUrl) - 1}), "clipboard_write_image");
    check(inject::helper_wait([&] { return helper.selection_events > events; }, 2000) && helper.selection &&
              (helper.selection->mimes & 8) && (helper.selection->mimes & 16) && !(helper.selection->mimes & 1),
          "the clipboard manager sees PNG and HTML, and no text any more");
    static u8 png[4096];
    const u32 png_bytes = read_pumped(inject::helper_receive(&helper, false, "image/png"), png, sizeof(png), 2000);
    check(png_bytes > 40 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G', "the PNG has its signature");
    count = read_pumped(inject::helper_receive(&helper, false, "text/html"), buffer, sizeof(buffer), 2000);
    check(text_matches(buffer, count, "<img src=\"https://cdn.example/image.png?x=1&amp;y=2\">"), "the HTML is the escaped linked image");
    check(clipboard_capture_attachment() == nullptr, "what the window itself wrote is not an attachment");

    // Another client owns the clipboard: the window learns of it and reads the text.
    constexpr char kOutside[] = "from outside \xC3\xA9";
    const inject::Offered text_formats[] = {{"text/plain;charset=utf-8", reinterpret_cast<const u8*>(kOutside), sizeof(kOutside) - 1},
                                            {"text/plain", reinterpret_cast<const u8*>(kOutside), sizeof(kOutside) - 1},
                                            {"UTF8_STRING", reinterpret_cast<const u8*>(kOutside), sizeof(kOutside) - 1}};
    mark();
    inject::helper_set_selection(&helper, false, text_formats, 3);
    expect("clipboard_changed arrives", EventType::clipboard_changed, [](const Record&) { return true; });
    result = clipboard_read_utf8(Slice<char>{read_back, sizeof(read_back)});
    check(clipboard_has_text() && result.ok() && text_matches(reinterpret_cast<u8*>(read_back), result.bytes, kOutside),
          "the window reads another client's clipboard through a pipe");

    // Files: a uri-list with a percent-encoded name, a non-ASCII name, a comment and a remote host.
    constexpr char kList[] = "file:///tmp/a%20b.txt\r\nfile:///tmp/c%C3%A9.png\r\n# a comment\r\nfile://otherhost/x\r\n";
    const inject::Offered list_formats[] = {{"text/uri-list", reinterpret_cast<const u8*>(kList), sizeof(kList) - 1}};
    mark();
    inject::helper_set_selection(&helper, false, list_formats, 1);
    expect("clipboard_changed for the file list", EventType::clipboard_changed, [](const Record&) { return true; });
    ClipboardCapture* capture = clipboard_capture_attachment();
    check(capture && clipboard_capture_kind(capture) == ClipboardCaptureKind::files &&
              dropped_files_count(clipboard_capture_files(capture)) == 2 &&
              wide_path_is(clipboard_capture_files(capture), 0, "/tmp/a b.txt") &&
              wide_path_is(clipboard_capture_files(capture), 1, "/tmp/c?.png", 0xE9, 6),
          "the file attachments are the two local paths, decoded");
    clipboard_capture_release(capture);
    constexpr char kGnome[] = "copy\nfile:///tmp/g.txt";
    const inject::Offered gnome_formats[] = {{"x-special/gnome-copied-files", reinterpret_cast<const u8*>(kGnome), sizeof(kGnome) - 1}};
    mark();
    inject::helper_set_selection(&helper, false, gnome_formats, 1);
    expect("clipboard_changed for GNOME's list", EventType::clipboard_changed, [](const Record&) { return true; });
    capture = clipboard_capture_attachment();
    check(capture && dropped_files_count(clipboard_capture_files(capture)) == 1 && wide_path_is(clipboard_capture_files(capture), 0, "/tmp/g.txt"),
          "GNOME's copied-files format gives its file");
    clipboard_capture_release(capture);

    // An image from another client.
    const inject::Offered image_formats[] = {{"image/png", png, png_bytes}};
    mark();
    inject::helper_set_selection(&helper, false, image_formats, 1);
    expect("clipboard_changed for the image", EventType::clipboard_changed, [](const Record&) { return true; });
    capture = clipboard_capture_attachment();
    check(capture && clipboard_capture_kind(capture) == ClipboardCaptureKind::image_png &&
              clipboard_capture_png(capture).count == png_bytes && bytes_equal(clipboard_capture_png(capture).data, png, png_bytes),
          "the image attachment is the PNG, byte for byte");
    clipboard_capture_release(capture);

    // The primary selection, both ways.
    events = helper.primary_events;
    check(primary_selection_write_utf8(Utf8View{"primary \xC3\xA9", 10}), "primary_selection_write_utf8");
    check(inject::helper_wait([&] { return helper.primary_events > events; }, 2000) && helper.primary && (helper.primary->mimes & 1),
          "the clipboard manager sees the primary selection");
    count = read_pumped(inject::helper_receive(&helper, true, "text/plain;charset=utf-8"), buffer, sizeof(buffer), 2000);
    check(text_matches(buffer, count, "primary \xC3\xA9"), "it reads the primary text");
    constexpr char kPrimary[] = "selected elsewhere";
    const inject::Offered primary_formats[] = {{"text/plain;charset=utf-8", reinterpret_cast<const u8*>(kPrimary), sizeof(kPrimary) - 1}};
    inject::helper_set_selection(&helper, true, primary_formats, 1);
    bool read_other = false;
    inject::helper_wait([&] {
        pump_once(5);
        const ClipboardTextResult other = primary_selection_read_utf8(Slice<char>{read_back, sizeof(read_back)});
        read_other = other.ok() && text_matches(reinterpret_cast<u8*>(read_back), other.bytes, kPrimary);
        return read_other;
    }, 3000);
    check(read_other, "the window learns of another client's primary selection and reads it");
}

// Text input: the helper plays the input method ---------------------------------------------------------------------------

bool g_ime_text_matches(const u16* units, u32 count, const u16* expected, u32 expected_count) noexcept {
    if (count != expected_count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (units[i] != expected[i])
            return false;
    return true;
}

void test_text_input() noexcept {
    inject::Helper& helper = *g_helper;
    if (!helper.ime_manager || !window_wayland_text_input_available(g_window)) {
        test_write_line("SKIP text input: no zwp_input_method_manager_v2 for the helper to play an IME, or no text-input-v3");
        return;
    }
    TextInputRequest request{};
    request.surrounding = Utf8View{"h\xC3\xA9llo", 6};
    request.cursor_byte = 6;
    request.anchor_byte = 6;
    request.purpose = TextInputPurpose::normal;
    request.x = 10;
    request.y = 20;
    request.width = 2;
    request.height = 18;
    check(window_wayland_text_input_enable(g_window, request), "text input enabled");
    check(inject::helper_wait([&] { return helper.ime_active != 0 && helper.ime_done_count >= 1; }, 3000),
          "the input method is activated");
    check(helper.ime_surrounding_bytes == 6 && helper.ime_cursor == 6 && helper.ime_anchor == 6 &&
              inject::helper_detail::same(helper.ime_surrounding, "h\xC3\xA9llo"),
          "it is told the text around the caret");
    check(helper.ime_purpose == zwp_text_input_v3_content_purpose_normal, "and the content purpose");

    // A preedit, then a commit with the preedit cleared, then a deletion.
    mark();
    inject::helper_ime_preedit(&helper, "\xE3\x81\x8B\xE3\x81\xAA", 0, 6);
    inject::helper_ime_commit(&helper);
    static const u16 kana[] = {0x304B, 0x306A};
    const u32 preedit = expect("a preedit arrives", EventType::ime_preedit, [](const Record& r) { return r.event.ime.count == 2; });
    check(preedit != kNone && g_ime_text_matches(g_log[preedit].event.ime.text, g_log[preedit].event.ime.count, kana, 2) &&
              g_log[preedit].event.ime.cursor_begin == 0 && g_log[preedit].event.ime.cursor_end == 2,
          "its text and cursor are in UTF-16 units");
    mark();
    inject::helper_ime_commit_string(&helper, "\xE6\xBC\xA2\xE5\xAD\x97\xF0\x9F\x98\x80");
    inject::helper_ime_commit(&helper);
    static const u16 committed[] = {0x6F22, 0x5B57, 0xD83D, 0xDE00};
    const u32 commit = expect("a commit arrives", EventType::ime_commit, [](const Record&) { return true; });
    check(commit != kNone && g_ime_text_matches(g_log[commit].event.ime.text, g_log[commit].event.ime.count, committed, 4),
          "the committed text is UTF-16, an emoji as a surrogate pair");
    expect("the preedit is cleared by the commit", EventType::ime_preedit, [](const Record& r) { return r.event.ime.count == 0; });
    mark();
    inject::helper_ime_delete(&helper, 3, 0);
    inject::helper_ime_commit(&helper);
    const u32 deleted = expect("a deletion arrives", EventType::ime_delete_surrounding, [](const Record&) { return true; });
    check(deleted != kNone && g_log[deleted].event.ime.delete_before == 3 && g_log[deleted].event.ime.delete_after == 0,
          "three bytes before the caret are three UTF-16 units");
    window_wayland_text_input_disable(g_window);
    check(inject::helper_wait([&] { return helper.ime_active == 0; }, 3000), "disabling deactivates the input method");

    // A password field: hidden text, no surrounding text.
    request.sensitive = true;
    request.purpose = TextInputPurpose::password;
    check(window_wayland_text_input_enable(g_window, request), "text input enabled for a password");
    check(inject::helper_wait([&] { return helper.ime_active != 0 && helper.ime_hint != 0; }, 3000) && (helper.ime_hint & 0xC0) == 0xC0 &&
              helper.ime_purpose == zwp_text_input_v3_content_purpose_password && helper.ime_surrounding_bytes == 0,
          "a password is hidden and sensitive, with nothing around the caret");
    window_wayland_text_input_disable(g_window);
    check(inject::helper_wait([&] { return helper.ime_active == 0; }, 3000), "and deactivates again");
}

// window_post, attention and idle inhibit -----------------------------------------------------------------------------------

struct PostJob {
    Waker waker;
    u32 count;
};

u32 post_thread(void* argument) noexcept {
    const PostJob& job = *static_cast<const PostJob*>(argument);
    for (u32 i = 0; i < job.count; ++i) {
        while (!window_post(job.waker, i, ~static_cast<u64>(i)))
            thread_sleep(1);
    }
    return 0;
}

void test_extras() noexcept {
    // Posts from another thread come out as wake events, in order, with their arguments.
    PostJob job{Waker{window_surface_native(g_window), wake_notifications}, 600};
    mark();
    Thread* thread = thread_start(post_thread, &job, 128 * 1024, nullptr);
    check(thread != nullptr, "the posting thread starts");
    check(pump_until([] { return count_events(g_mark, EventType::wake) >= 600; }, 5000), "all posts arrive");
    u32 expected = 0;
    bool ordered = true;
    for (u32 i = g_mark; i < g_count; ++i) {
        const WindowEvent& e = g_log[i].event;
        if (e.type != EventType::wake)
            continue;
        ordered = ordered && e.wake.cause == wake_notifications && e.wake.a == expected && e.wake.b == ~static_cast<u64>(expected);
        ++expected;
    }
    check(ordered && expected == 600, "posts keep their order and arguments");
    if (thread) {
        thread_wait(thread, 2000);
        thread_close(thread);
    }

    // Attention asks for a token and activates with it.
    const u32 tokens = window_wayland_activation_count();
    window_request_attention(g_window, Attention::flash);
    check(pump_until([&] { return window_wayland_activation_count() > tokens; }, 2000), "an activation token comes back");

    // Idle inhibit: the compositor reports it (the driver reads sway's tree).
    if (window_wayland_set_idle_inhibit(g_window, true)) {
        touch_file("idle-on");
        i32 answer[1] = {-1};
        check(pump_until([&] { return read_numbers("idle-on.result", answer, 1) == 1; }, 5000) && answer[0] == 1,
              "the compositor sees the idle inhibitor");
        window_wayland_set_idle_inhibit(g_window, false);
        touch_file("idle-off");
        answer[0] = -1;
        check(pump_until([&] { return read_numbers("idle-off.result", answer, 1) == 1; }, 5000) && answer[0] == 0,
              "and sees it removed");
    } else {
        test_write_line("SKIP idle inhibit: no zwp_idle_inhibit_manager_v1");
    }
}

// The window's wl_egl_window with a real EGL window surface -----------------------------------------------------------------

struct EglApi {
    void* (*get_platform_display)(u32 platform, void* native, const i32* attributes) noexcept;
    u32 (*initialize)(void* display, i32* major, i32* minor) noexcept;
    u32 (*bind_api)(u32 api) noexcept;
    u32 (*choose_config)(void* display, const i32* attributes, void** configs, i32 capacity, i32* count) noexcept;
    void* (*create_context)(void* display, void* config, void* share, const i32* attributes) noexcept;
    void* (*create_window_surface)(void* display, void* config, void* window, const i32* attributes) noexcept;
    u32 (*make_current)(void* display, void* draw, void* read, void* context) noexcept;
    u32 (*swap_buffers)(void* display, void* surface) noexcept;
    u32 (*swap_interval)(void* display, i32 interval) noexcept;
    u32 (*terminate)(void* display) noexcept;
    void (*clear_color)(float red, float green, float blue, float alpha) noexcept;
    void (*clear)(u32 mask) noexcept;
};

template <typename Function> bool resolve_symbol(const SharedLibrary& library, const char* name, Function* slot) noexcept {
    void* address = shared_library_symbol(library, name);
    *slot = reinterpret_cast<Function>(address);
    return address != nullptr;
}

// A window drawn by EGL: green. The driver screenshots it and reads a pixel in the middle; the test reads the answer.
void run_egl() noexcept {
    SharedLibrary egl_library{};
    SharedLibrary gles_library{};
    EglApi api{};
    if (!shared_library_open(&egl_library, "libEGL.so.1") || !shared_library_open(&gles_library, "libGLESv2.so.2"))
        skip("libEGL.so.1 or libGLESv2.so.2 is not installed");
    const bool resolved = resolve_symbol(egl_library, "eglGetPlatformDisplay", &api.get_platform_display) &&
                          resolve_symbol(egl_library, "eglInitialize", &api.initialize) &&
                          resolve_symbol(egl_library, "eglBindAPI", &api.bind_api) &&
                          resolve_symbol(egl_library, "eglChooseConfig", &api.choose_config) &&
                          resolve_symbol(egl_library, "eglCreateContext", &api.create_context) &&
                          resolve_symbol(egl_library, "eglCreateWindowSurface", &api.create_window_surface) &&
                          resolve_symbol(egl_library, "eglMakeCurrent", &api.make_current) &&
                          resolve_symbol(egl_library, "eglSwapBuffers", &api.swap_buffers) &&
                          resolve_symbol(egl_library, "eglSwapInterval", &api.swap_interval) &&
                          resolve_symbol(egl_library, "eglTerminate", &api.terminate) &&
                          resolve_symbol(gles_library, "glClearColor", &api.clear_color) &&
                          resolve_symbol(gles_library, "glClear", &api.clear);
    check(resolved, "EGL and GLES entry points");
    void* native = window_wayland_egl_window(g_window);
    if (!resolved || !native)
        skip("libwayland-egl.so.1 is not installed, or EGL is unusable");
    WaylandSurfaceInfo surface{};
    window_wayland_info(g_window, &surface);
    constexpr u32 kPlatformWayland = 0x31D8;
    void* display = api.get_platform_display(kPlatformWayland, surface.display, nullptr);
    i32 major = 0;
    i32 minor = 0;
    check(display && api.initialize(display, &major, &minor), "eglInitialize on the window's wl_display");
    api.bind_api(0x30A0);  // EGL_OPENGL_ES_API
    const i32 config_attributes[] = {0x3033, 0x4, 0x3040, 0x4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    void* config = nullptr;
    i32 configs = 0;
    check(api.choose_config(display, config_attributes, &config, 1, &configs) && configs == 1, "an RGBA8 window config");
    const i32 context_attributes[] = {0x3098, 2, 0x3038};
    void* context = api.create_context(display, config, nullptr, context_attributes);
    void* egl_surface = api.create_window_surface(display, config, native, nullptr);
    check(context && egl_surface, "an EGL window surface on the wl_egl_window");
    check(api.make_current(display, egl_surface, egl_surface, context) != 0, "eglMakeCurrent");
    api.swap_interval(display, 0);
    bool ready_written = false;
    const u64 start = clock_monotonic_milliseconds();
    u32 frames = 0;
    while (clock_monotonic_milliseconds() - start < 20000 && !file_exists("injected")) {
        api.clear_color(0.0f, 1.0f, 0.0f, 1.0f);
        api.clear(0x4000);
        api.swap_buffers(display, egl_surface);
        ++frames;
        if (frames == 5 && !ready_written) {
            touch_file("ready");
            ready_written = true;
        }
        pump_once(16);
    }
    check(frames > 5 && !g_quit, "frames were presented");
    i32 pixel[3] = {0, 0, 0};
    check(read_numbers("pixel", pixel, 3) == 3 && pixel[0] < 40 && pixel[1] > 215 && pixel[2] < 40,
          "the compositor shows the EGL frame: the middle of the window is green");
    api.make_current(display, nullptr, nullptr, nullptr);
    api.terminate(display);
    window_wayland_egl_release(g_window);
}

void run_input() noexcept {
    touch_file("ready");
    check(pump_until([] { return file_exists("origin"); }, 30000), "the driver wrote the window origin");
    i32 numbers[4] = {0, 0, 1280, 720};
    check(read_numbers("origin", numbers, 4) == 4, "origin holds x, y, width and height");
    g_origin_x = numbers[0];
    g_origin_y = numbers[1];
    if (!inject::open(&g_injector, static_cast<u32>(numbers[2]), static_cast<u32>(numbers[3]))) {
        inject::close(&g_injector);
        window_destroy(g_window);
        skip("the compositor has no virtual pointer or virtual keyboard: input cannot be injected");
    }
    // The window has keyboard focus and the pointer is somewhere: let the first events settle.
    settle(300);
    check(find_event(0, EventType::focus_changed, [](const Record& r) { return r.event.focus.gained; }) != kNone,
          "keyboard focus arrives");
    check(find_event(0, EventType::activation_changed, [](const Record& r) { return r.event.focus.gained; }) != kNone,
          "the window is activated");
    test_pointer();
    test_frame();
    test_keyboard();
    test_context_menu_keys();
    inject::Helper helper;
    g_helper = &helper;
    if (inject::helper_open(&helper)) {
        test_clipboard();
        test_text_input();
    } else {
        test_write_line("SKIP clipboard and text input: no second connection");
    }
    inject::helper_close(&helper);
    g_helper = nullptr;
    test_extras();
    check(!window_pointer_captured(g_window), "no capture at the end");
    check(!window_key_down(g_window, 'A') && !global_key_down('A') && !global_key_down(key::shift), "no key reads as down at the end");
    inject::close(&g_injector);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_wayland_window_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    bool input = false;
    bool lifecycle = false;
    bool egl = false;
    for (int i = 1; i < startup.argc; ++i) {
        input |= arg_is(startup.argv[i], "--input");
        egl |= arg_is(startup.argv[i], "--egl");
        lifecycle |= arg_is(startup.argv[i], "--lifecycle");
    }
    if (!input && !lifecycle && !egl)
        test_exit(1, "usage: --lifecycle, --input or --egl");
    const char* directory = environment_get("MAICORD_WAYLAND_SYNC_DIR");
    if (directory && *directory) {
        u32 length = 0;
        for (; directory[length] && length + 1 < sizeof(g_paths.directory); ++length)
            g_paths.directory[length] = directory[length];
        g_paths.directory[length] = 0;
        g_paths.enabled = true;
    }
    if (!environment_get("WAYLAND_DISPLAY") && !environment_get("WAYLAND_SOCKET"))
        skip("WAYLAND_DISPLAY is not set: no compositor to test against");
    if ((input || egl) && !g_paths.enabled)
        skip("MAICORD_WAYLAND_SYNC_DIR is not set: this test is run by tools/test_wayland_window.sh");
    if (!create_window()) {
        test_write_line("the window could not be created (see the log lines above)");
        test_exit(1, "wayland_window");
    }
    print_support_matrix();
    test_window_up();
    if (egl)
        run_egl();
    if (input) {
        run_input();
        if (g_failures)
            dump_log();
    }
    if (lifecycle)
        test_desktop_settings();
    if (lifecycle)
        test_global_shortcut();
    if (lifecycle) {
        // A token nobody issued is ignored by the compositor; an empty one is refused here.
        check(!window_wayland_activate_with_token(g_window, ""), "an empty activation token is refused");
        window_wayland_activate_with_token(g_window, "maicord-test-token");
        settle(50);
        check(!g_quit, "an unknown activation token does no harm");
    }
    if (lifecycle) {
        // With a driver (a screenshot is wanted), keep the presented window up until it says it is done.
        if (g_paths.enabled) {
            touch_file("ready");
            pump_until([] { return file_exists("injected"); }, 15000);
        }
        test_lifecycle();
    }
    if (g_window)
        window_destroy(g_window);
    g_window = nullptr;
    if (lifecycle)
        test_recreate();
    test_exit(g_failures, egl ? "wayland_window_egl" : input ? "wayland_window_input" : "wayland_window_lifecycle");
}

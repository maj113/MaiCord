#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/key.hpp"
#include "../../support/test_output.hpp"

// Windows behaviours the window seam depends on and that Windows does not document tightly, captured in a hidden
// harness window of this process (never the user's client, never injected input): when ReleaseCapture reports
// WM_CAPTURECHANGED, and when DefWindowProc turns the context-menu keys into WM_CONTEXTMENU(-1). The
// translation of window messages into window events (win32_translate) and the application's handlers rely on
// exactly these; the headless backend (src/platform/window/null) reproduces them for the tests that run
// everywhere. The AltGr rule (Alt with Ctrl is not a system key) is decided by the system's input thread and can
// only be observed with injected input, which this suite never sends; the event translation takes the system key
// bit from the message that arrives (translate.cpp) and tests/platform/window/win32_events.cpp checks that.
namespace maicord {
namespace {

struct Logged {
    HWND window;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
};
constexpr u32 kLogCapacity = 64;
Logged g_log[kLogCapacity];
u32 g_count;
u32 failures;
const wchar_t kClassName[] = L"MaiCordWindowBehaviourHarness";

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

LRESULT CALLBACK harness_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_CONTEXTMENU:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        if (g_count < kLogCapacity)
            g_log[g_count++] = {window, message, wparam, lparam};
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND make_window() noexcept {
    return CreateWindowExW(0, kClassName, L"MaiCord window behaviour harness", WS_POPUP, -16000, -16000, 200, 200,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

u32 logged(UINT message) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < g_count; ++i)
        count += g_log[i].message == message;
    return count;
}

const Logged* find(UINT message) noexcept {
    for (u32 i = 0; i < g_count; ++i)
        if (g_log[i].message == message)
            return &g_log[i];
    return nullptr;
}

// Messages of this kind waiting in the window's queue (posted, not sent).
bool queued(HWND window, UINT message) noexcept {
    MSG pending{};
    return PeekMessageW(&pending, window, message, message, PM_REMOVE) != FALSE;
}

void drain(HWND window) noexcept {
    MSG pending{};
    while (PeekMessageW(&pending, window, 0, 0, PM_REMOVE))
        DispatchMessageW(&pending);
}

LPARAM key_lparam(bool released, WORD code) noexcept {
    // Repeat count 1, the key's scan code and extended bit as a keyboard reports them. Bits 30 and 31: previous
    // state and transition, set for a release.
    const u32 scan = code == key::menu ? 0x5Du : 0x44u;
    const u32 extended = code == key::menu ? 1u << 24 : 0u;
    return static_cast<LPARAM>((released ? 0xC0000001u : 0x00000001u) | (scan << 16) | extended);
}

// What one press and release of a key made the default procedure do.
struct KeyOutcome {
    u32 sent_on_press, queued_on_press, sent_on_release, queued_on_release;
    LPARAM menu_lparam;
    bool menu_to_window;
};

void set_keys(bool shift) noexcept {
    BYTE keys[256]{};
    if (shift)
        keys[key::shift] = 0x80;
    SetKeyboardState(keys);
}

KeyOutcome key_press_release(HWND window, bool system, WORD code, bool shift, bool alt_context) noexcept {
    KeyOutcome outcome{};
    set_keys(shift);
    const LPARAM context = alt_context ? 1ll << 29 : 0;
    drain(window);
    g_count = 0;
    SendMessageW(window, system ? WM_SYSKEYDOWN : WM_KEYDOWN, code, key_lparam(false, code) | context);
    outcome.sent_on_press = logged(WM_CONTEXTMENU);
    if (const Logged* menu = find(WM_CONTEXTMENU)) {
        outcome.menu_lparam = menu->lparam;
        outcome.menu_to_window = menu->wparam == reinterpret_cast<WPARAM>(window);
    }
    outcome.queued_on_press = queued(window, WM_CONTEXTMENU);
    g_count = 0;
    SendMessageW(window, system ? WM_SYSKEYUP : WM_KEYUP, code, key_lparam(true, code) | context);
    outcome.sent_on_release = logged(WM_CONTEXTMENU);
    outcome.queued_on_release = queued(window, WM_CONTEXTMENU);
    return outcome;
}


} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_window_win32_behaviour_entry) {
    using namespace maicord;
    BYTE saved[256]{};
    const bool have_saved = GetKeyboardState(saved) != FALSE;
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = harness_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kClassName;
    const bool registered = RegisterClassW(&window_class) != 0;
    check(have_saved && registered, "win32 behaviour: harness class registered and keyboard state saved");
    HWND first = registered ? make_window() : nullptr;
    HWND second = registered ? make_window() : nullptr;
    check(first && second, "win32 behaviour: two hidden harness windows exist");
    if (first && second) {
        // 1. Gaining capture sends nothing.
        g_count = 0;
        SetCapture(first);
        check(GetCapture() == first && g_count == 0, "capture: SetCapture reports nothing to the new owner");
        g_count = 0;
        SetCapture(first);
        const Logged* again = find(WM_CAPTURECHANGED);
        check(GetCapture() == first && logged(WM_CAPTURECHANGED) == 1 && again && again->window == first &&
                  again->lparam == reinterpret_cast<LPARAM>(first),
              "capture: capturing again for the same window notifies it synchronously, naming itself");

        // 2. ReleaseCapture reports WM_CAPTURECHANGED to the window that held it, with a null new owner, before
        //    it returns.
        g_count = 0;
        ReleaseCapture();
        check(GetCapture() == nullptr && logged(WM_CAPTURECHANGED) == 1,
              "capture: ReleaseCapture delivers WM_CAPTURECHANGED synchronously, once");
        const Logged* lost = find(WM_CAPTURECHANGED);
        check(lost && lost->window == first && lost->lparam == 0 && lost->wparam == 0,
              "capture: the released window is told, and the new owner is null");
        check(queued(first, WM_CAPTURECHANGED) == false, "capture: the notification is sent, never queued");

        // 3. Releasing nothing reports nothing.
        g_count = 0;
        ReleaseCapture();
        check(g_count == 0, "capture: ReleaseCapture without capture reports nothing");

        // 4. Taking the capture from another window of the thread reports it to the old owner, naming the thief.
        SetCapture(first);
        g_count = 0;
        SetCapture(second);
        lost = find(WM_CAPTURECHANGED);
        check(GetCapture() == second && logged(WM_CAPTURECHANGED) == 1 && lost && lost->window == first &&
                  lost->lparam == reinterpret_cast<LPARAM>(second),
              "capture: stealing the capture notifies the old owner synchronously, with the new owner in lparam");
        g_count = 0;
        ReleaseCapture();
        check(logged(WM_CAPTURECHANGED) == 1 && g_log[0].window == second,
              "capture: only the current owner hears the release");

        // 5. Destroying the owner releases the capture.
        SetCapture(second);
        g_count = 0;
        DestroyWindow(second);
        check(GetCapture() == nullptr && logged(WM_CAPTURECHANGED) <= 1,
              "capture: destroying the owner leaves no capture");
        second = nullptr;

        // 6. Context-menu keys through DefWindowProc. Shift+F10 is turned into WM_CONTEXTMENU(window, -1) while the
        //    default procedure handles the press, as a nested send (the application's handler runs inside the
        //    key's own dispatch, after it declined the key); nothing waits for the release. F10 alone, and the
        //    menu key, make none through the default procedure (a real menu key reaches the window from the system's
        //    input thread, which only injected input could observe).
        const KeyOutcome shift_f10 = key_press_release(first, false, key::f10, true, false);
        check(shift_f10.sent_on_press == 1 && shift_f10.queued_on_press == 0 && shift_f10.sent_on_release == 0 &&
                  shift_f10.queued_on_release == 0 && shift_f10.menu_lparam == -1 && shift_f10.menu_to_window,
              "context menu: Shift+F10 sends WM_CONTEXTMENU(window, -1) synchronously during the press");
        const KeyOutcome shift_f10_system = key_press_release(first, true, key::f10, true, false);
        check(shift_f10_system.sent_on_press == 1 && shift_f10_system.queued_on_press == 0 &&
                  shift_f10_system.sent_on_release == 0 && shift_f10_system.queued_on_release == 0 &&
                  shift_f10_system.menu_lparam == -1 && shift_f10_system.menu_to_window,
              "context menu: Shift+F10 arriving as system keys does the same");
        const KeyOutcome shift_f10_alt = key_press_release(first, true, key::f10, true, true);
        check(shift_f10_alt.sent_on_press == 0 && shift_f10_alt.sent_on_release == 0 &&
                  shift_f10_alt.queued_on_press == 0 && shift_f10_alt.queued_on_release == 0,
              "context menu: Alt+Shift+F10 makes none");
        const KeyOutcome f10 = key_press_release(first, true, key::f10, false, false);
        check(f10.sent_on_press == 0 && f10.queued_on_press == 0 && f10.sent_on_release == 0 &&
                  f10.queued_on_release == 0,
              "context menu: F10 without Shift makes none");
        const KeyOutcome menu_key = key_press_release(first, false, key::menu, false, false);
        check(menu_key.sent_on_press == 0 && menu_key.queued_on_press == 0 && menu_key.sent_on_release == 0 &&
                  menu_key.queued_on_release == 0,
              "context menu: the menu key makes none through the default procedure");
        set_keys(false);

        DestroyWindow(first);
    }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
    if (have_saved)
        SetKeyboardState(saved);
    test_exit(failures, "window win32 behaviour");
}

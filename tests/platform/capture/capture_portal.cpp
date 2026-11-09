// Linux screen capture's portal half (src/platform/capture/linux/screen_capture.cpp) against the ScreenCast mock of
// tests/platform/dbus/mock_services.py on a private bus (tools/test_dbus.sh): the requests in order with their options
// (the session token, the source kinds, one source, the cursor mode, the parent window), the cancel in the portal's
// picker ending the capture as denied, a remote with no PipeWire behind it ending it as failed, the session closed each
// time, and capture_stop in the middle of the portal's dialog. On the bare bus (--bare) there is no portal: the capture
// ends as unsupported. Without libpipewire the capture cannot start at all, which is checked instead.
#include "maicord/platform/capture/screen_capture.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "../../../src/platform/capture/linux/pipewire_stream.hpp"
#include "../dbus/mock_control.hpp"

namespace maicord {
namespace {
using namespace mock;

struct Outcome {
    Waitable done;
    volatile LONG ended; // CaptureEnd + 1
    volatile LONG frames;
};
Outcome g_outcome;

void on_frame(void* context, const CaptureFrame&) noexcept {
    InterlockedIncrement(&static_cast<Outcome*>(context)->frames);
}
void on_ended(void* context, CaptureEnd reason) noexcept {
    auto* outcome = static_cast<Outcome*>(context);
    InterlockedExchange(&outcome->ended, static_cast<LONG>(reason) + 1);
    event_set(outcome->done);
}

// Runs one capture to its end (or `stop_after_ms` then capture_stop); the end, or -1 when it never came.
i32 run_capture(bool cursor, u8 kinds, u32 stop_after_ms = 0) noexcept {
    event_reset(g_outcome.done);
    g_outcome.ended = 0;
    g_outcome.frames = 0;
    CaptureOptions options{};
    options.kinds = kinds;
    options.cursor = cursor;
    options.max_fps = 30;
    ScreenCapture* capture = capture_start(options, {&g_outcome, on_frame, on_ended, nullptr});
    if (!capture)
        return -2;
    const u32 waited = wait_one(g_outcome.done, stop_after_ms ? stop_after_ms : 15'000);
    capture_stop(capture);
    return waited == WAIT_OBJECT_0 ? static_cast<i32>(g_outcome.ended) - 1 : -1;
}

bool bus_gate(const char* kind) noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    return bus && address && session && text_equal(bus, kind) && text_equal(address, session);
}

void test_mock() noexcept {
    clear_log();
    set_mode("screencast-cancel", true);
    check(run_capture(true, 3) == static_cast<i32>(CaptureEnd::denied), "portal: a cancel in the picker ends as denied");
    take_log();
    const char* create = find_line("ScreenCast.CreateSession");
    const char* select = find_line("ScreenCast.SelectSources");
    const char* start = find_line("ScreenCast.Start");
    check(create && contains(create, "s:handle_token=") && contains(create, "s:session_handle_token="),
          "portal: CreateSession carries the request and session tokens");
    check(select && contains(select, "s:types=v(u:3)") && contains(select, "s:multiple=v(b:0)") &&
              contains(select, "s:cursor_mode=v(u:2)"),
          "portal: SelectSources asks for screens and windows, one source, the cursor drawn in");
    check(start != nullptr, "portal: Start shows the portal's picker");
    check(!find_line("ScreenCast.OpenPipeWireRemote"), "portal: nothing is opened after a cancel");
    check(find_line("Session.Close /org/freedesktop/portal/desktop/session/") != nullptr,
          "portal: the session is closed after a cancel");
    if (!create || !select || !start)
        dump_log();

    clear_log();
    set_mode("screencast-cancel", false);
    check(run_capture(false, 1) == static_cast<i32>(CaptureEnd::failed),
          "portal: a remote without PipeWire behind it ends as failed");
    take_log();
    select = find_line("ScreenCast.SelectSources");
    check(select && contains(select, "s:types=v(u:1)"), "portal: screens only when only screens are asked for");
    check(select && contains(select, "s:cursor_mode=v(u:1)"), "portal: the cursor hidden when not wanted");
    check(find_line("Session.Close") != nullptr, "portal: the session is closed when the capture ends");
    check(g_outcome.frames == 0, "portal: no frame without a stream");

    // A stop while the portal answers: no end is reported and nothing is left open.
    clear_log();
    set_mode("screencast-cancel", true);
    check(run_capture(true, 3, 1) == -1, "portal: capture_stop during the dialog reports no end");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_capture_portal_entry) {
    using namespace maicord;
    using namespace maicord::mock;
    const ProcessStartup& startup = process_startup();
    const bool bare = startup.argc > 1 && text_equal(startup.argv[1], "--bare");
    if (!bus_gate(bare ? "bare" : "mock")) {
        test_write_line("SKIP capture_portal (the private bus is not set up: tools/test_dbus.sh)");
        test_process_exit(77);
    }
    g_outcome.done = event_create(true, false);
    if (!pipewire::api()) {
        check(!capture_start(CaptureOptions{}, {&g_outcome, on_frame, on_ended, nullptr}),
              "portal: without libpipewire nothing starts");
        test_write_line("SKIP capture_portal (no libpipewire-0.3.so.0: only the refusal was checked)");
        test_exit(g_failures, "capture_portal");
    }
    if (bare) {
        check(run_capture(true, 3) == static_cast<i32>(CaptureEnd::unsupported),
              "portal: a desktop without the ScreenCast portal ends as unsupported");
        test_exit(g_failures, "capture_portal_bare");
    }
    g_control = connection_open_address(environment_get("MAICORD_DBUS_TEST_ADDRESS"));
    check(g_control != nullptr, "a control connection to the mock");
    if (!g_control)
        test_exit(g_failures, "capture_portal");
    control("Reset", nullptr);
    test_mock();
    connection_close(g_control);
    test_exit(g_failures, "capture_portal");
}

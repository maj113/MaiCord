#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/platform/desktop/notifications.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

// The Linux toast layer (src/platform/desktop/linux/notifications.cpp). Without a session bus (the registration points
// DBUS_SESSION_BUS_ADDRESS at nothing, so no toast ever reaches a daemon) what is checked is the contract the application
// relies on: validation before any send, the failure of a send without a bus, the removals and the activation queue being
// harmless when empty, and the lifecycle. With --mock, a daemon (see below).

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr wchar_t kAppId[] = L"io.github.maj113.MaiCord";

DesktopNotificationPayload payload(Utf8View title, Utf8View body) noexcept {
    DesktopNotificationPayload result{};
    result.target = {1, 2, 3};
    result.title = title;
    result.body = body;
    return result;
}

void test_lifecycle() noexcept {
    DesktopNotifications notifications{};
    check(!desktop_notifications_initialize(nullptr, {}).ok(), "initialize needs a handle");
    check(desktop_notifications_initialize(&notifications, {}).error == DesktopNotificationError::invalid_input,
          "initialize needs an application id");
    const auto early = desktop_notifications_show_once(&notifications, payload(utf8_literal("t"), utf8_literal("b")));
    check(early.state == DesktopNotificationShowState::failed && !early.native_show_attempted,
          "a toast before initialize fails without a send");

    DesktopNotificationConfig config{};
    config.app_id = {kAppId, sizeof(kAppId) / sizeof(wchar_t) - 1};
    config.register_activator = true;
    check(desktop_notifications_initialize(&notifications, config).ok() && notifications.runtime, "initialize");

    u32 setting = 99;
    check(desktop_notifications_setting(&notifications, &setting).ok() && setting == 1,
          "without a notification server the setting reads disabled for the application");

    DesktopNotificationActivation activation{};
    check(!desktop_notifications_take_activation(&notifications, &activation), "no activation is queued");
    desktop_notifications_discard_activations(&notifications);
    bool pending = true;
    check(desktop_notifications_history_step(&notifications, &pending).ok() && !pending &&
              !desktop_notifications_history_pending(&notifications),
          "the history has nothing pending");
    check(desktop_notifications_set_wake(&notifications, {}).ok(), "the wake target can be replaced");
    check(desktop_notifications_remove_message(&notifications, {1, 2, 3}).ok() &&
              desktop_notifications_remove_channel(&notifications, 1, 2).ok() &&
              desktop_notifications_remove_profile(&notifications, 1).ok(),
          "removals of toasts that were never shown succeed");

    desktop_notifications_release(&notifications);
    check(notifications.runtime == nullptr, "release clears the handle");
    check(desktop_notifications_remove_message(&notifications, {1, 2, 3}).error == DesktopNotificationError::closed,
          "calls after release report closed");
    desktop_notifications_release(&notifications);
}

void test_show() noexcept {
    DesktopNotifications notifications{};
    DesktopNotificationConfig config{};
    config.app_id = {kAppId, sizeof(kAppId) / sizeof(wchar_t) - 1};
    check(desktop_notifications_initialize(&notifications, config).ok(), "initialize for the show cases");

    const auto empty = desktop_notifications_show_once(&notifications, payload({}, utf8_literal("body")));
    check(empty.state == DesktopNotificationShowState::failed && empty.result.error == DesktopNotificationError::invalid_input &&
              !empty.native_show_attempted,
          "an empty title is refused before any send");
    const char with_nul[] = {'a', 0, 'b'};
    const auto nul = desktop_notifications_show_once(&notifications, payload({with_nul, 3}, utf8_literal("body")));
    check(nul.result.error == DesktopNotificationError::invalid_input && !nul.native_show_attempted,
          "a NUL in the title is refused");
    const char invalid[] = {'\xC3', '('};
    const auto bad = desktop_notifications_show_once(&notifications, payload({invalid, 2}, utf8_literal("body")));
    check(bad.result.error == DesktopNotificationError::invalid_input, "invalid UTF-8 is refused");
    static char big[8192];
    for (char& c : big)
        c = 'x';
    const auto too_big_title = desktop_notifications_show_once(&notifications, payload({big, 2000}, utf8_literal("b")));
    check(too_big_title.result.error == DesktopNotificationError::capacity && !too_big_title.native_show_attempted,
          "a title over the limit is refused");
    const auto too_big_body = desktop_notifications_show_once(&notifications, payload(utf8_literal("t"), {big, 8000}));
    check(too_big_body.result.error == DesktopNotificationError::capacity, "a body over the limit is refused");

    DesktopNotificationPayload valid = payload(utf8_literal("Alice"), utf8_literal("hello"));
    valid.avatar_file_uri = utf8_literal("file:///tmp/maicord%20avatar.png");
    const auto sent = desktop_notifications_show_once(&notifications, valid);
    check(sent.native_show_attempted && sent.state == DesktopNotificationShowState::failed &&
              sent.result.error == DesktopNotificationError::transport,
          "without a session bus a valid toast reaches the send and fails there");
    check(desktop_notification_show_state(sent.result) == DesktopNotificationShowState::failed &&
              desktop_notification_show_state({}) == DesktopNotificationShowState::submitted,
          "the result classification");
    desktop_notifications_release(&notifications);
}

// With a daemon ------------------------------------------------------------------------------------------------------------
//
// --mock: the toast layer over the private bus of tools/test_dbus.sh, whose mock daemon logs every call it receives in a
// canonical form (tests/platform/dbus/mock_services.py). What reaches the daemon for a message, as the user sees it: the
// title, the body as text (a daemon that reads markup gets it escaped), the avatar as its file, the default action, the
// sound, and what a click and the removal of the toast do.

dbus::Connection* g_control;
char g_lines[16][1024];
u32 g_line_count;

bool starts_with(const char* text, const char* prefix) noexcept {
    while (*prefix && *text == *prefix)
        ++text, ++prefix;
    return *prefix == 0;
}

bool same_text(const char* a, const char* b) noexcept {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

dbus::CallTarget mock(const char* member) noexcept {
    return dbus::CallTarget{"org.maicord.Mock", "/org/maicord/Mock", "org.maicord.Mock", member};
}

void control(const char* member, const dbus::Builder* body) noexcept {
    dbus::Message reply;
    const dbus::Status status = dbus::connection_call_sync(g_control, mock(member), body, 3000, &reply);
    check(status == dbus::Status::ok, member);
    if (status == dbus::Status::ok || status == dbus::Status::error_reply)
        dbus::message_release(&reply);
}

// What the mock logged since the last take.
void take_log() noexcept {
    g_line_count = 0;
    dbus::Message reply;
    if (dbus::connection_call_sync(g_control, mock("Take"), nullptr, 3000, &reply) != dbus::Status::ok)
        return;
    dbus::Reader r;
    dbus::message_reader(reply, &r);
    if (dbus::reader_enter_array(&r)) {
        while (dbus::reader_more(r) && g_line_count < 16) {
            const char* text = nullptr;
            u32 length = 0;
            if (!dbus::reader_string(&r, &text, &length))
                break;
            if (length >= sizeof(g_lines[0]))
                length = sizeof(g_lines[0]) - 1;
            copy_bytes(g_lines[g_line_count], text, length);
            g_lines[g_line_count][length] = 0;
            ++g_line_count;
        }
    }
    dbus::message_release(&reply);
}

const char* find_line(const char* prefix) noexcept {
    for (u32 i = 0; i < g_line_count; ++i)
        if (starts_with(g_lines[i], prefix))
            return g_lines[i];
    return nullptr;
}

// Serves the session bus for `milliseconds`, taking the mock's log each round, until it holds a line starting with `prefix`.
const char* wait_for_line(const char* prefix, u32 milliseconds) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    for (;;) {
        const u64 now = clock_monotonic_milliseconds();
        Waitable waitable = dbus::session_bus_waitable();
        if (waitable)
            wait_many(&waitable, 1, 30, 0);
        dbus::session_bus_process(now);
        take_log();
        if (const char* line = find_line(prefix))
            return line;
        if (now >= deadline)
            return nullptr;
    }
}

void serve(u32 milliseconds) noexcept {
    const u64 until = clock_monotonic_milliseconds() + milliseconds;
    while (clock_monotonic_milliseconds() < until) {
        Waitable waitable = dbus::session_bus_waitable();
        if (waitable)
            wait_many(&waitable, 1, 20, 0);
        dbus::session_bus_process(clock_monotonic_milliseconds());
    }
}

void test_mock() noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    if (!bus || !address || !session || !same_text(bus, "mock") || !same_text(address, session)) {
        test_write_line("SKIP linux_notifications --mock (the private bus is not set up: tools/test_dbus.sh)");
        test_process_exit(77);
    }
    g_control = dbus::connection_open_address(address);
    check(g_control != nullptr, "a control connection to the mock");
    if (!g_control)
        return;
    control("Reset", nullptr);

    DesktopNotifications notifications{};
    DesktopNotificationConfig config{};
    config.app_id = {kAppId, sizeof(kAppId) / sizeof(wchar_t) - 1};
    config.register_activator = true;
    check(desktop_notifications_initialize(&notifications, config).ok(), "initialize");
    u32 setting = 99;
    check(desktop_notifications_setting(&notifications, &setting).ok() && setting == 0,
          "with a notification server the setting reads enabled");

    // The message of someone with markup characters in their name and text, and an avatar.
    DesktopNotificationPayload message = payload(utf8_literal("R&D <team>"), utf8_literal("<b>hi</b> & bye"));
    message.target = {1, 2, 3};
    message.avatar_file_uri = utf8_literal("file:////tmp/maicord%20avatar.png");
    message.silent = true;
    take_log();
    const auto shown = desktop_notifications_show_once(&notifications, message);
    check(shown.native_show_attempted && shown.state == DesktopNotificationShowState::submitted && shown.result.ok(),
          "the toast is submitted");
    const char* notify = wait_for_line("Notify s:", 4000);
    check(notify != nullptr, "the daemon received it");
    if (notify) {
        const char* want =
            "Notify s:MaiCord u:0 s:/tmp/maicord avatar.png s:R&D <team> s:&lt;b&gt;hi&lt;/b&gt; &amp; bye "
            "[s:default,s:Open] {s:category=v(s:im.received),s:desktop-entry=v(s:io.github.maj113.MaiCord),"
            "s:suppress-sound=v(b:1),s:urgency=v(y:1)} i:-1";
        if (!same_text(notify, want)) {
            test_write_text("  got:  ");
            test_write_line(notify);
            test_write_text("  want: ");
            test_write_line(want);
        }
        check(same_text(notify, want),
              "the title, the body as text, the avatar as a plain path, the default action and the sound request");
    }

    // The daemon says how it numbered the toast (the mock logs it); a click on the toast is an activation of the message.
    const char* numbered = find_line("NotifyId ");
    u32 id = 0;
    for (const char* digit = numbered ? numbered + 9 : ""; *digit >= '0' && *digit <= '9'; ++digit)
        id = id * 10 + static_cast<u32>(*digit - '0');
    check(id != 0, "the daemon numbered it");
    dbus::Builder b;
    dbus::builder_init(&b);
    dbus::builder_string(&b, "default");
    dbus::builder_u32(&b, id);
    control("NotifyAction", &b);
    dbus::builder_release(&b);
    serve(300);
    DesktopNotificationActivation activation{};
    check(desktop_notifications_take_activation(&notifications, &activation) && activation.profile_id == 1 &&
              activation.channel_id == 2 && activation.message_id == 3,
          "a click on the toast comes back as the activation of its message");
    check(!desktop_notifications_take_activation(&notifications, &activation), "and only once");

    // The message was read elsewhere: its toast is withdrawn.
    take_log();
    check(desktop_notifications_remove_message(&notifications, {1, 2, 3}).ok(), "the toast of the message is removed");
    const char* closed = wait_for_line("CloseNotification", 3000);
    char want_close[48] = "CloseNotification u:";
    u32 want_length = 20;
    char reversed[12];
    u32 digits = 0;
    for (u32 n = id; n || digits == 0; n /= 10)
        reversed[digits++] = static_cast<char>('0' + n % 10);
    while (digits)
        want_close[want_length++] = reversed[--digits];
    want_close[want_length] = 0;
    check(closed != nullptr && same_text(closed, want_close), "the daemon was told to close that toast");
    desktop_notifications_release(&notifications);
    dbus::connection_close(g_control);
    dbus::session_bus_close();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_notifications_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    if (startup.argc > 1 && same_text(startup.argv[1], "--mock")) {
        test_mock();
        test_exit(g_failures, "linux_notifications_mock");
    }
    test_lifecycle();
    test_show();
    test_exit(g_failures, "linux_notifications");
}

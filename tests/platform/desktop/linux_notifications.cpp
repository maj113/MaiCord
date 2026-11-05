#include "maicord/core/utf.hpp"
#include "maicord/platform/desktop/notifications.hpp"
#include "../../support/test_output.hpp"

// The Linux toast layer (src/platform/desktop/linux/notifications.cpp) without a session bus: the registration points
// DBUS_SESSION_BUS_ADDRESS at nothing, so no toast ever reaches a daemon. What is checked is the contract the
// application relies on: validation before any send, the failure of a send without a bus, the removals and the
// activation queue being harmless when empty, and the lifecycle.

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

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_notifications_entry) {
    using namespace maicord;
    test_lifecycle();
    test_show();
    test_exit(g_failures, "linux_notifications");
}

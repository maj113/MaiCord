#include "maicord/app/browser_sign_in_receiver.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
// Default fixture is offline: it never starts Winsock, opens a listener,
// launches a browser or sends a request. Native lifecycle acceptance belongs
// to the separately opt-in receiver probe.
void test_browser_sign_in_receiver(void (*check)(bool, const char*) noexcept) noexcept {
    BrowserSignInReceiver receiver{};
    check(browser_sign_in_receiver_status(&receiver).phase == BrowserSignInReceiverPhase::empty &&
        !browser_sign_in_receiver_notification_handle(&receiver), "empty browser receiver has no handles, retained credentials or published candidate");
    check(browser_sign_in_receiver_start(nullptr, 1, 1000).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_start(&receiver, 0, 1000).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_start(&receiver, 1, 999).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_start(&receiver, 1, 300001).error == BrowserSignInReceiverError::invalid_input && !receiver.runtime,
        "invalid generations/deadlines fail before creating a worker/listener");
    SecureBuffer output{};
    const auto initialized = secure_buffer_initialize(&output, 256);
    check(initialized.ok(), "browser fixture uses externally owned locked snapshot output");
    if (initialized.ok()) {
        constexpr Utf8View marker = utf8_literal("synthetic-preserved");
        secure_buffer_assign(&output, {reinterpret_cast<const u8*>(marker.data), marker.bytes});
        check(!browser_sign_in_receiver_copy_url(&receiver, 1, &output).ok() &&
            !browser_sign_in_receiver_take_exchange(&receiver, 1, 1, &output).ok() &&
            output.bytes == marker.bytes && bytes_equal(output.data, marker.data, marker.bytes),
            "absent/stale owners reject secret snapshots without modifying caller output");
        secure_buffer_release(&output);
    }
    check(browser_sign_in_receiver_finish_exchange(&receiver, 1, 1, true).error == BrowserSignInReceiverError::invalid_input &&
        browser_sign_in_receiver_finish_exchange(&receiver, 1, 1, false).error == BrowserSignInReceiverError::invalid_input,
        "an absent owner cannot accept or reject an exchange as an authenticated session");
    check(browser_sign_in_receiver_stop(&receiver, 0).ok() && !receiver.runtime,
        "stopping an empty receiver is idempotent and does not allocate");
    check(browser_sign_in_receiver_runtime_bytes() >= 40 * 1024 && browser_sign_in_receiver_runtime_bytes() <= 48 * 1024,
        "receiver has one bounded locked request/credential arena and a page-sized metadata owner");
}
} // namespace maicord

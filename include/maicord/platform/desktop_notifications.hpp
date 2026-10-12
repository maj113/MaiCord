#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

#include <guiddef.h>

namespace maicord {
inline constexpr u32 kDesktopNotificationXmlBytes = 4096;
inline constexpr u32 kDesktopNotificationActivationCapacity = 8;
enum class DesktopNotificationError : u8 { none, invalid_input, capacity, memory, apartment, identity, transport, closed };
struct DesktopNotificationResult {
    DesktopNotificationError error{};
    u32 native_error{};
    bool ok() const noexcept { return error == DesktopNotificationError::none; }
};
struct DesktopNotificationActivation { u64 profile_id{}, channel_id{}, message_id{}; };
struct DesktopNotificationPayload {
    DesktopNotificationActivation target;
    Utf8View title, body;
    // URI of an already-written local file for this account. The caller keeps the
    // file until OS history no longer needs it. No download or media cache is created.
    Utf8View avatar_file_uri;
    bool silent{}, suppress_popup{};
};
struct DesktopNotificationXml { char bytes[kDesktopNotificationXmlBytes]; u32 count{}; };
struct DesktopNotificationConfig {
    ConstSlice<wchar_t> app_id;
    GUID activator_id{};
    HWND wake_window{};
    u32 wake_message{};
    // True only after durable identity enrollment. False selects a fully
    // in-memory factory/queue mode, which cannot deliver any desktop toast.
    bool register_activator{};
};
struct DesktopNotifications { void* runtime{}; };
enum class DesktopNotificationShowState : u8 { submitted, failed, retry_required };
struct DesktopNotificationShowResult {
    DesktopNotificationShowState state{DesktopNotificationShowState::failed};
    DesktopNotificationResult result{DesktopNotificationError::closed,0};
    bool native_show_attempted{};
};
// Pure conversion: rejects invalid UTF-8/XML controls and overflow.
// Policy must redact spoilers/NUL and select eligible messages before this call.
DesktopNotificationResult desktop_notification_xml(const DesktopNotificationPayload&, DesktopNotificationXml*) noexcept;
bool desktop_notification_activation_parse(ConstSlice<wchar_t>, DesktopNotificationActivation*) noexcept;
bool desktop_notification_activation_format(DesktopNotificationActivation, Slice<wchar_t>, u32*) noexcept;
bool desktop_notification_hex_id(u64, Slice<wchar_t>) noexcept;
// Call on the UI thread. Register early, before account/renderer startup, so a cold
// COM launch can answer Windows. No worker, timer or polling loop is created.
DesktopNotificationResult desktop_notifications_initialize(DesktopNotifications*, const DesktopNotificationConfig&) noexcept;
// Pure result classification; recoverable disconnection is not submission.
DesktopNotificationShowState desktop_notification_show_state(DesktopNotificationResult) noexcept;
// Makes zero or one IToastNotifier::Show call, including on every failure path.
// A recoverable disconnect drops the cached broker/snapshot refs and returns retry;
// the caller revalidates the copied intent on a later loop turn (at most 2 attempts).
// native_show_attempted distinguishes earlier XML/init/transport failures from
// the synchronous Show call. Submitted does not prove a displayed banner.
DesktopNotificationShowResult desktop_notifications_show_once(DesktopNotifications*, const DesktopNotificationPayload&) noexcept;
// Also attempts only once. Retry is reported as a non-ok transport error;
// there is no implicit retry, worker, timer or queued payload.
DesktopNotificationResult desktop_notifications_show(DesktopNotifications*, const DesktopNotificationPayload&) noexcept;
DesktopNotificationResult desktop_notifications_remove_message(DesktopNotifications*, DesktopNotificationActivation) noexcept;
// Only queues the request: copies/coalesces at most 8 profile/channel cleanup requests.
// Never enumerates history here. The UI thread completes the work with history_step.
DesktopNotificationResult desktop_notifications_remove_channel(DesktopNotifications*, u64 profile, u64 channel) noexcept;
// Handles at most 4 history items per UI turn and stops between items after a 2 ms
// QPC deadline. A single synchronous OS broker call can still exceed that budget.
// Failed requests are dropped; pending reports any remaining requests.
DesktopNotificationResult desktop_notifications_history_step(DesktopNotifications*, bool* pending) noexcept;
bool desktop_notifications_history_pending(const DesktopNotifications*) noexcept;
// Cancels queued/snapshot cleanup for this profile before the OS group removal.
DesktopNotificationResult desktop_notifications_remove_profile(DesktopNotifications*, u64 profile) noexcept;
// OS NotificationSetting enum value; Enabled does not promise a visible banner.
DesktopNotificationResult desktop_notifications_setting(DesktopNotifications*, u32*) noexcept;
DesktopNotificationResult desktop_notifications_set_wake(DesktopNotifications*, HWND, u32 message) noexcept;
bool desktop_notifications_take_activation(DesktopNotifications*, DesktopNotificationActivation*) noexcept;
void desktop_notifications_discard_activations(DesktopNotifications*) noexcept;
// Returns an AddRef'd IClassFactory for the non-emitting native probe. The caller
// releases it. This accessor does not modify the class registry.
DesktopNotificationResult desktop_notifications_factory(DesktopNotifications*, void**) noexcept;
// Call on the same UI thread. Clears the wake target/queue, revokes the factory and
// balances its COM apartment initialization. An outstanding COM callback keeps its own
// inert state alive.
void desktop_notifications_release(DesktopNotifications*) noexcept;
// Counts the attached runtime storage only; outstanding external COM references may
// keep the inert callback object alive until their final Release.
u32 desktop_notifications_owned_bytes(const DesktopNotifications*) noexcept;
}

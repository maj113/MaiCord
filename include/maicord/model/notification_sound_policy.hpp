#pragma once
#include "maicord/model/message_notifications.hpp"

namespace maicord {
enum class NotificationSoundDecision : u8 { unknown, denied, allowed };
enum class NotificationMessageSoundRoute : u8 { none, direct, popup };
// Normalized inputs for the selected-channel sound decision. Selection is the
// original channel's MAIN selection, before THREAD_STARTER_MESSAGE parent
// normalization. The effective channel/author facts and allow_no_messages use that
// normalized parent. allow_no_messages includes guild/category/channel mute, NONE
// and opt-in status. This helper does not use popup eligibility: selected sound
// has no SUPPRESS_NOTIFICATIONS, age, lurking, request, RPC or own-activity check.
struct NotificationSelectedSoundInput {
    u64 self_id, author_id;
    MessageNotificationGlobal global; // caller supplies effective presence/expiry
    u32 known_gates, gates;
    bool selection_known, main_selected, scope_known, allow_no_messages;
};
NotificationSoundDecision notification_selected_sound_admission(const NotificationSelectedSoundInput&) noexcept;

// Inputs for the MESSAGE_CREATE sound decision. ordinary must be the message
// eligibility BEFORE popup transport/streamer/overlay checks, computed with
// the selected channel eligible only while unfocused. selected includes the
// device selected-sound preference and the selected-channel check above. The
// selected path can produce message3 even when ordinary is not allowed.
struct NotificationMessageSoundInput {
    NotificationSoundDecision ordinary, selected;
    NotificationSoundDecision restrictions; // restricted hours / required action / changelog
    NotificationSoundDecision overlay; // ordinary-path overlay early-return condition
    bool ready, live, duplicate, optimistic;
    bool focus_known, focused, preference_known, message1_enabled;
    bool desktop_type_known, desktop_never;
};
struct NotificationMessageSoundDecision {
    bool selected_direct; // unthrottled message3, using MESSAGE1's disable setting
    NotificationMessageSoundRoute ordinary;
    // Store text/RPC/popup/direct branch continues even when message1 sound is
    // disabled. An unfocused selected path returns before that branch regardless
    // of message1_enabled.
    bool ordinary_notification{};
};
NotificationMessageSoundDecision notification_message_sound_decide(const NotificationMessageSoundInput&) noexcept;

// The popup adapter checks authorization asynchronously. Every stage describes
// a separate call; native_failed may request a SECOND throttled invocation after
// before_popup already requested fallback sound. Do not coalesce those calls.
enum class NotificationSoundStage : u8 { direct, popup_suppressed, before_popup, native_failed };
// unavailable includes a completed native query returning null. unknown means
// no result yet; it must not be normalized to silent/denied.
enum class NotificationSoundNative : u8 { unknown, unavailable, denied, provisional, authorized_silent, authorized_sound };
enum class NotificationSoundDispatch : u8 { none, direct, throttled, native_attached };
NotificationSoundDispatch notification_sound_dispatch(NotificationSoundStage, NotificationSoundNative,
    bool sound_present, bool play_sound_if_disabled = true) noexcept;
enum class NotificationSoundTransport : u8 { none, native_standalone, fallback };
// Current native settings are checked first. The streamer disable-sounds setting
// applies ONLY to the fallback; an authorized native send has no streamer sound
// check. A failed standalone native send falls through to the fallback with the
// same authorization facts. Never report denied to model a failure.
NotificationSoundTransport notification_sound_transport(NotificationSoundNative, bool streamer_disable_sounds,
    bool native_send_failed = false) noexcept;
// Fallback DEFAULT output is the system default sink, not the voice output
// device. Gain is sampled at audio load/playback, not when a request is queued.
bool notification_message_sound_gain(float output_percent, float* gain) noexcept;

enum class NotificationSoundKind : u8 { message1, message3 }; // supported message-create assets
struct NotificationSoundRequest {
    u64 generation; // nonzero account epoch; zero disables sounds
    u32 soundpack; // opaque caller-owned stable token, copied; message3 is unmapped
    float volume;
    NotificationSoundKind kind;
};
// One process-wide throttle shared by all sound names containing "message".
// It behaves like a Lodash-style throttle with a 1000 ms wait (leading=true,
// trailing=true, maxWait=1000). Direct/selected/native-attached paths bypass it.
// No allocation, audio, message cache, OS timer or callback ownership here.
struct NotificationSoundThrottle {
    u64 generation, last_call_ms, last_invoke_ms, deadline_ms;
    NotificationSoundRequest pending_request;
    bool has_last_call, timer_active, pending;
};
struct NotificationSoundInvocation { NotificationSoundRequest request; bool invoke; };
NotificationSoundInvocation notification_sound_throttle_request(NotificationSoundThrottle*, u64 now_ms,
    NotificationSoundRequest) noexcept;
// Called for the host's elapsed timer, or a monotonic early notification. A
// time earlier than last_call is treated as a clock rollback.
NotificationSoundInvocation notification_sound_throttle_timer(NotificationSoundThrottle*, u64 now_ms, u64 generation) noexcept;
bool notification_sound_throttle_deadline(const NotificationSoundThrottle&, u64* deadline_ms) noexcept;
// Called by the host on account change, shutdown or explicit cancel; not on ACK,
// focus change, preference change or sound completion.
void notification_sound_throttle_cancel(NotificationSoundThrottle*) noexcept;
// Zero disables the throttle; a new nonzero epoch starts with no pending timer.
void notification_sound_throttle_generation(NotificationSoundThrottle*, u64 generation) noexcept;
} // namespace maicord

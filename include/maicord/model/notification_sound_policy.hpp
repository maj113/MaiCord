#pragma once
#include "maicord/model/message_notifications.hpp"

namespace maicord {
enum class NotificationSoundDecision : u8 { unknown, denied, allowed };
enum class NotificationMessageSoundRoute : u8 { none, direct, popup };
// Selection is the original channel's main selection, before THREAD_STARTER_MESSAGE
// parent normalization; the channel/author facts and allow_no_messages (guild,
// category or channel mute, NONE, opt-in) use the normalized parent. Popup
// eligibility checks (SUPPRESS_NOTIFICATIONS, age, lurking, own activity) do not apply.
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
    // Store text/RPC/popup continuation is independent of message1 sound.
    // An unfocused selected message still projects a desktop popup, but skips
    // the ordinary sound route to avoid doubling its message3 sound.
    bool ordinary_notification{};
};
NotificationMessageSoundDecision notification_message_sound_decide(const NotificationMessageSoundInput&) noexcept;

enum class NotificationSoundKind : u8 { message1, message3 }; // supported message-create assets
struct NotificationSoundRequest {
    u64 generation; // nonzero account epoch; zero disables sounds
    u32 soundpack; // opaque caller-owned stable token, copied; message3 is unmapped
    float volume;
    NotificationSoundKind kind;
};
// One process-wide throttle shared by all sound names containing "message".
// Lodash-style throttle: 1000 ms wait, leading=true, trailing=true,
// maxWait=1000. Direct/selected/native-attached paths bypass it. Owns no
// allocation, audio, message cache, OS timer or callback.
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

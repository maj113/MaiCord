#pragma once
#include "maicord/model/notification_sound_policy.hpp"

namespace maicord {
// Gates for ordinary message sounds. They apply before the restricted-hours,
// required-action, changelog and overlay checks and before popup/streamer
// transport. An active embedded application counts as one of these gates.
inline constexpr u32 kNotificationSoundOrdinaryGates=notification_gate_blocked|notification_gate_ignored|
    notification_gate_spammer|notification_gate_managed|notification_gate_lurking|notification_gate_request|
    notification_gate_own_activity|notification_gate_active_application|notification_gate_rpc_suppressed|
    notification_gate_age_access|notification_gate_interaction_relationship;
struct NotificationSoundAdmissionInput {
    // Scope is already normalized to the parent for thread-starter messages.
    // selected/sidebar flags in policy refer to that normalized scope; focused
    // is current focus.
    const MessageNotificationPolicyInput* policy;
    // The selected-channel comparison uses the ORIGINAL channel, not the
    // normalized parent. No policy facts or borrowed views are kept.
    bool original_selection_known, original_main_selected;
    bool selected_preference_known, selected_preference;
};
struct NotificationSoundAdmissions {
    NotificationSoundDecision ordinary, selected;
    MessageNotificationReason ordinary_reason, selected_reason;
};
// Both keep the host's ready/live/duplicate barriers.
MessageNotificationReason notification_sound_ordinary_admit(const MessageNotificationPolicyInput&) noexcept;
MessageNotificationReason notification_sound_selected_admit(const NotificationSoundAdmissionInput&) noexcept;
NotificationSoundAdmissions notification_sound_admissions(const NotificationSoundAdmissionInput&) noexcept;
} // namespace maicord

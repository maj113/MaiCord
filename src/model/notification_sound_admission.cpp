#include "maicord/model/notification_sound_admission.hpp"

namespace maicord {
MessageNotificationReason notification_sound_ordinary_admit(const MessageNotificationPolicyInput& source) noexcept {
    return message_notification_admit(source,kNotificationSoundOrdinaryGates);
}
MessageNotificationReason notification_sound_selected_admit(const NotificationSoundAdmissionInput& value) noexcept {
    using R=MessageNotificationReason;using A=NotificationSoundDecision;
    if(!value.policy)return R::unknown;
    const auto& i=*value.policy;
    if(!i.live)return R::not_live;if(i.duplicate)return R::duplicate;
    if(!i.ready)return R::unknown;
    if(!value.selected_preference_known)return R::unknown;
    if(!value.selected_preference)return R::nothing;
    if(!value.original_selection_known)return R::unknown;
    if(!value.original_main_selected)return R::selected;
    if(!i.identities_known||!i.self_id||!i.author_id||!i.type_known)return R::unknown;
    const auto scope=message_notification_scope_resolve(i,MessageNotificationScopeMode::generic);
    const auto global=message_notification_effective_global(i.global,i.now_ms,i.ready&&i.live);
    if(global.fields&~15u)return R::unknown;
    const NotificationSelectedSoundInput selected{i.self_id,i.author_id,global,i.known_gates,i.gates,
        true,true,scope.reason!=R::unknown,scope.reason!=R::eligible};
    const auto admission=notification_selected_sound_admission(selected);
    if(admission==A::unknown)return R::unknown;
    if(admission==A::allowed)return R::eligible;
    if(i.self_id==i.author_id)return R::self;
    if(scope.reason!=R::eligible)return scope.reason;
    if(global.status==MessageNotificationStatus::dnd)return R::dnd;
    if(global.quiet)return R::quiet;
    return R::special;
}
namespace {
NotificationSoundDecision admission(MessageNotificationReason reason) noexcept {
    return reason==MessageNotificationReason::eligible?NotificationSoundDecision::allowed:
        reason==MessageNotificationReason::unknown?NotificationSoundDecision::unknown:NotificationSoundDecision::denied;
}
}
NotificationSoundAdmissions notification_sound_admissions(const NotificationSoundAdmissionInput& i) noexcept {
    using R=MessageNotificationReason;
    const auto ordinary=i.policy?notification_sound_ordinary_admit(*i.policy):R::unknown;
    const auto selected=notification_sound_selected_admit(i);
    return {admission(ordinary),admission(selected),ordinary,selected};
}
MessageNotificationOutputs message_notification_outputs(const NotificationSoundAdmissionInput& i,
                                                        const MessageNotificationDevice& device) noexcept {
    using A=NotificationSoundDecision;
    MessageNotificationOutputs out{};
    if(!i.policy)return out;
    const auto& p=*i.policy;
    out.badge=message_read_badge(p,device.mention_on_all);
    NotificationSoundAdmissionInput admission=i;
    admission.selected_preference_known=true;admission.selected_preference=device.selected_sounds;
    const auto admissions=notification_sound_admissions(admission);
    const auto gate=[&](u32 mask) noexcept {
        return (p.gates&mask)?A::denied:(p.known_gates&mask)==mask?A::allowed:A::unknown;
    };
    constexpr u32 common=notification_gate_restricted_hours|notification_gate_required_action|notification_gate_changelog;
    const NotificationMessageSoundInput sound{admissions.ordinary,admissions.selected,gate(common),
        gate(notification_gate_overlay),p.ready,p.live,p.duplicate,false,true,p.focused,true,
        device.message_sounds,true,!device.desktop_enabled};
    const auto decided=notification_message_sound_decide(sound);
    out.selected_sound=decided.selected_direct;
    out.ordinary=decided.ordinary_notification;
    out.sound=decided.ordinary;
    // Streamer mode hides the popup and its flash; the sound stays.
    out.popup=out.ordinary&&device.desktop_enabled&&gate(notification_gate_streamer)==A::allowed;
    out.flash=out.popup&&device.taskbar_flash;
    return out;
}
} // namespace maicord

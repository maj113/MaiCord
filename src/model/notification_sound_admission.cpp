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
} // namespace maicord

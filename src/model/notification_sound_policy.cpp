#include "maicord/model/notification_sound_policy.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
NotificationSoundDecision notification_selected_sound_admission(const NotificationSelectedSoundInput& i) noexcept {
    using A=NotificationSoundDecision;
    if(!i.selection_known)return A::unknown;
    if(!i.main_selected)return A::denied;
    if(!i.self_id||!i.author_id||!i.scope_known)return A::unknown;
    if(i.self_id==i.author_id||i.allow_no_messages)return A::denied;
    constexpr u32 required=notification_gate_managed|notification_gate_spammer|notification_gate_blocked|
        notification_gate_ignored|notification_gate_interaction_relationship;
    if(i.gates&required)return A::denied;
    if((i.known_gates&required)!=required)return A::unknown;
    if((i.global.fields&(notification_status|notification_quiet))!=(notification_status|notification_quiet)||
        i.global.status==MessageNotificationStatus::unknown||
        static_cast<u8>(i.global.status)>static_cast<u8>(MessageNotificationStatus::offline))return A::unknown;
    if(i.global.status==MessageNotificationStatus::dnd||i.global.quiet)return A::denied;
    return A::allowed;
}
NotificationMessageSoundDecision notification_message_sound_decide(const NotificationMessageSoundInput& i) noexcept {
    using A=NotificationSoundDecision;
    NotificationMessageSoundDecision d{};
    if(!i.ready||!i.live||i.duplicate||i.optimistic||i.restrictions!=A::allowed||!i.focus_known)return d;
    const bool selected=i.selected==A::allowed;
    d.selected_direct=selected&&i.preference_known&&i.message1_enabled;
    // While unfocused, a selected candidate returns before ordinary notification
    // processing, even when the message1 preference disables the selected sound.
    if((!i.focused&&i.selected!=A::denied)||i.ordinary!=A::allowed||i.overlay!=A::allowed||
        !i.desktop_type_known)return d;
    d.ordinary_notification=true;
    if(!i.preference_known||!i.message1_enabled)return d;
    d.ordinary=i.desktop_never?NotificationMessageSoundRoute::direct:NotificationMessageSoundRoute::popup;
    return d;
}
namespace {
constexpr u64 wait_ms=1000;
u64 deadline(u64 now,u64 wait) noexcept {return now>~u64{0}-wait?~u64{0}:now+wait;}
bool should_invoke(const NotificationSoundThrottle& s,u64 now) noexcept {
    return !s.has_last_call||now<s.last_call_ms||now-s.last_call_ms>=wait_ms||
        (now>=s.last_invoke_ms&&now-s.last_invoke_ms>=wait_ms);
}
NotificationSoundInvocation invoke(NotificationSoundThrottle* s,u64 now) noexcept {
    NotificationSoundInvocation result{s->pending_request,true};
    s->pending=false;s->last_invoke_ms=now;return result;
}
}
NotificationSoundInvocation notification_sound_throttle_request(NotificationSoundThrottle* s,u64 now,
    NotificationSoundRequest request) noexcept {
    if(!s||!request.generation||request.generation!=s->generation)return {};
    const bool invoking=should_invoke(*s,now);
    s->pending_request=request;s->pending=true;s->last_call_ms=now;s->has_last_call=true;
    if(invoking) {
        // The leading edge and the maxWait branch both arm wait_ms.
        s->timer_active=true;s->deadline_ms=deadline(now,wait_ms);
        return invoke(s,now);
    }
    if(!s->timer_active){s->timer_active=true;s->deadline_ms=deadline(now,wait_ms);}
    return {};
}
NotificationSoundInvocation notification_sound_throttle_timer(NotificationSoundThrottle* s,u64 now,u64 generation) noexcept {
    if(!s||!generation||generation!=s->generation||!s->timer_active)return {};
    // An elapsed timer can observe the clock moving backwards. Lodash treats a
    // negative time since the last call as its trailing edge, so that case must
    // not be hidden behind the nominal deadline. Early timer notifications
    // still leave the deadline and pending request untouched.
    if(now<s->deadline_ms&&now>=s->last_call_ms)return {};
    if(should_invoke(*s,now)) {
        s->timer_active=false;
        if(s->pending)return invoke(s,now);
        return {};
    }
    const u64 since_call=now-s->last_call_ms,since_invoke=now-s->last_invoke_ms;
    const u64 call_wait=wait_ms-since_call,invoke_wait=wait_ms-since_invoke;
    s->deadline_ms=deadline(now,call_wait<invoke_wait?call_wait:invoke_wait);
    return {};
}
bool notification_sound_throttle_deadline(const NotificationSoundThrottle& s,u64* out) noexcept {
    if(!out||!s.timer_active)return false;*out=s.deadline_ms;return true;
}
void notification_sound_throttle_cancel(NotificationSoundThrottle* s) noexcept {if(s){const u64 generation=s->generation;reset_value(s);s->generation=generation;}}
void notification_sound_throttle_generation(NotificationSoundThrottle* s,u64 generation) noexcept {if(s){reset_value(s);s->generation=generation;}}
} // namespace maicord

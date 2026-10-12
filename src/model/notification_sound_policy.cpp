#include "maicord/model/notification_sound_policy.hpp"

namespace maicord {
namespace {
bool finite(float value) noexcept {
    u32 bits{};copy_bytes(&bits,&value,sizeof(bits));
    return (bits&0x7f800000u)!=0x7f800000u;
}
}
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
    // T returns before ordinary notification processing while blurred, even
    // when the shared message1 preference disables its actual selected sound.
    if((!i.focused&&i.selected!=A::denied)||i.ordinary!=A::allowed||i.overlay!=A::allowed||
        !i.desktop_type_known)return d;
    d.ordinary_notification=true;
    if(!i.preference_known||!i.message1_enabled)return d;
    d.ordinary=i.desktop_never?NotificationMessageSoundRoute::direct:NotificationMessageSoundRoute::popup;
    return d;
}
NotificationSoundDispatch notification_sound_dispatch(NotificationSoundStage stage,NotificationSoundNative native,
    bool sound,bool when_disabled) noexcept {
    using D=NotificationSoundDispatch;using S=NotificationSoundStage;using N=NotificationSoundNative;
    if(!sound||static_cast<u8>(stage)>static_cast<u8>(S::native_failed)||
        static_cast<u8>(native)>static_cast<u8>(N::authorized_sound))return D::none;
    if(stage==S::direct)return D::direct;
    if(stage==S::popup_suppressed)return when_disabled?D::throttled:D::none;
    if(stage==S::before_popup)return native==N::unknown?D::none:native==N::authorized_sound?D::native_attached:D::throttled;
    if(stage==S::native_failed&&(native==N::authorized_sound||native==N::authorized_silent||native==N::provisional))return D::throttled;
    return D::none;
}
NotificationSoundTransport notification_sound_transport(NotificationSoundNative native,bool disabled,bool failed) noexcept {
    if(native==NotificationSoundNative::unknown||
        static_cast<u8>(native)>static_cast<u8>(NotificationSoundNative::authorized_sound))return NotificationSoundTransport::none;
    if(native==NotificationSoundNative::authorized_sound&&!failed)return NotificationSoundTransport::native_standalone;
    return disabled?NotificationSoundTransport::none:NotificationSoundTransport::fallback;
}
bool notification_message_sound_gain(float percent,float* gain) noexcept {
    // Bit validation remains valid under the application's fast-float mode and
    // needs no math/runtime import for NaN or infinity classification.
    if(!gain||!finite(percent)||percent<0.0f)return false;
    const float value=(percent/100.0f)*0.4f;*gain=value<1.0f?value:1.0f;return true;
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
    if(!s||!request.generation||request.generation!=s->generation||static_cast<u8>(request.kind)>static_cast<u8>(NotificationSoundKind::message3)||
        !finite(request.volume)||request.volume<0.0f||request.volume>1.0f)return {};
    const bool invoking=should_invoke(*s,now);
    s->pending_request=request;s->pending=true;s->last_call_ms=now;s->has_last_call=true;
    if(invoking) {
        // leadingEdge and the maxWait tight-loop branch both arm wait_ms.
        s->timer_active=true;s->deadline_ms=deadline(now,wait_ms);
        return invoke(s,now);
    }
    if(!s->timer_active){s->timer_active=true;s->deadline_ms=deadline(now,wait_ms);}
    return {};
}
NotificationSoundInvocation notification_sound_throttle_timer(NotificationSoundThrottle* s,u64 now,u64 generation) noexcept {
    if(!s||!generation||generation!=s->generation||!s->timer_active)return {};
    // An actual elapsed timer can observe Date.now() moving backwards. Lodash
    // treats a negative time-since-last-call as its trailing edge; do not hide
    // that branch behind the nominal absolute deadline. Ordinary early timer
    // notifications still leave the current deadline/pending args untouched.
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
void notification_sound_throttle_cancel(NotificationSoundThrottle* s) noexcept {if(s){const u64 generation=s->generation;*s={};s->generation=generation;}}
void notification_sound_throttle_generation(NotificationSoundThrottle* s,u64 generation) noexcept {if(s){*s={};s->generation=generation;}}
} // namespace maicord

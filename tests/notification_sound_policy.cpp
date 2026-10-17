#include "maicord/model/notification_sound_policy.hpp"
namespace maicord {
namespace {
NotificationSelectedSoundInput selected_input() noexcept {
    NotificationSelectedSoundInput i{};i.self_id=1;i.author_id=2;
    i.selection_known=i.main_selected=i.scope_known=true;
    i.known_gates=notification_gate_managed|notification_gate_spammer|notification_gate_blocked|
        notification_gate_ignored|notification_gate_interaction_relationship;
    i.global.fields=notification_status|notification_quiet;i.global.status=MessageNotificationStatus::online;return i;
}
NotificationMessageSoundInput message_input() noexcept {
    NotificationMessageSoundInput i{};i.ordinary=i.restrictions=i.overlay=NotificationSoundDecision::allowed;
    i.selected=NotificationSoundDecision::denied;
    i.ready=i.live=i.focus_known=i.preference_known=i.message1_enabled=i.desktop_type_known=true;return i;
}
NotificationSoundRequest request(u64 generation,u32 pack=1,NotificationSoundKind kind=NotificationSoundKind::message1) noexcept {
    return {generation,pack,0.4f,kind};
}
}
void test_notification_sound_policy(void(*check)(bool,const char*)noexcept) noexcept {
    using A=NotificationSoundDecision;using R=NotificationMessageSoundRoute;
    auto selected=selected_input();
    check(notification_selected_sound_admission(selected)==A::allowed,"selected sound independent predicate admits ordinary selected author");
    selected.main_selected=false;check(notification_selected_sound_admission(selected)==A::denied,"sidebar-only selection does not enter main selected sound path");
    selected=selected_input();selected.author_id=1;check(notification_selected_sound_admission(selected)==A::denied,"selected sound does not exempt self mentionable system authors");
    selected=selected_input();selected.allow_no_messages=true;check(notification_selected_sound_admission(selected)==A::denied,"selected allow-no-messages includes mute NONE and opt-in");
    selected=selected_input();selected.known_gates&=~notification_gate_interaction_relationship;
    check(notification_selected_sound_admission(selected)==A::unknown,"unknown modern interaction relationship cannot fabricate selected admission");
    selected=selected_input();selected.gates=notification_gate_spammer;
    check(notification_selected_sound_admission(selected)==A::denied,"selected spammer suppression independent of mentions");
    selected=selected_input();selected.global.status=MessageNotificationStatus::dnd;
    check(notification_selected_sound_admission(selected)==A::denied,"selected DND suppression");
    selected.global.status=MessageNotificationStatus::invisible;
    check(notification_selected_sound_admission(selected)==A::allowed,"selected invisible does not suppress sound");
    selected.global.quiet=true;check(notification_selected_sound_admission(selected)==A::denied,"selected quiet independent of presence");
    selected=selected_input();selected.global.status=static_cast<MessageNotificationStatus>(255);
    check(notification_selected_sound_admission(selected)==A::unknown,"unsupported selected status value cannot fabricate non-DND admission");
    selected=selected_input();selected.gates=notification_gate_age_access|notification_gate_request|notification_gate_own_activity;
    check(notification_selected_sound_admission(selected)==A::allowed,"selected source predicate does not inherit unrelated popup gates");

    auto i=message_input();auto d=notification_message_sound_decide(i);
    check(!d.selected_direct&&d.ordinary==R::popup,"desktop ALL routes ordinary sound via popup adapter");
    i.desktop_never=true;check(notification_message_sound_decide(i).ordinary==R::direct,"desktop NEVER uses direct unthrottled ordinary sound");
    i.selected=A::allowed;i.ordinary=A::denied;
    d=notification_message_sound_decide(i);check(d.selected_direct&&d.ordinary==R::none,"selected-only candidate emits message3 independent of popup admission");
    i.ordinary=A::allowed;d=notification_message_sound_decide(i);
    check(d.selected_direct&&d.ordinary==R::none,"blurred selected candidate returns before ordinary sound");
    i.focused=true;d=notification_message_sound_decide(i);
    check(d.selected_direct&&d.ordinary==R::direct,"focused selected and ordinary candidates may emit both sounds");
    i.overlay=A::denied;d=notification_message_sound_decide(i);
    check(d.selected_direct&&d.ordinary==R::none,"overlay early return is after selected sound");
    i.restrictions=A::denied;d=notification_message_sound_decide(i);
    check(!d.selected_direct&&d.ordinary==R::none,"restricted hours required action and changelog gate both paths");
    i=message_input();i.selected=A::unknown;
    check(notification_message_sound_decide(i).ordinary==R::none,"unknown blurred selected path cannot choose incorrect ordinary sound");
    i=message_input();i.message1_enabled=false;i.selected=A::allowed;
    d=notification_message_sound_decide(i);check(!d.selected_direct&&d.ordinary==R::none,"message1 disabled preference also suppresses message3");
    check(!d.ordinary_notification,"blurred selected path stops popup continuation even when its sound is disabled");
    i.selected=A::denied;d=notification_message_sound_decide(i);
    check(d.ordinary_notification&&!d.selected_direct&&d.ordinary==R::none,
        "disabled message1 sound preserves ordinary text and popup branch");
    i.preference_known=false;d=notification_message_sound_decide(i);
    check(d.ordinary_notification&&d.ordinary==R::none,"unknown sound preference suppresses audio without inventing popup suppression");
    i=message_input();i.duplicate=true;check(notification_message_sound_decide(i).ordinary==R::none,"duplicate sound admission suppresses both transport paths");
    i.duplicate=false;i.optimistic=true;check(notification_message_sound_decide(i).ordinary==R::none,"optimistic create cannot play notification sound");
    i.optimistic=false;i.live=false;check(notification_message_sound_decide(i).ordinary==R::none,"history edit and pre-resume admission do not synthesize sound");

    NotificationSoundThrottle throttle{};
    check(!notification_sound_throttle_request(&throttle,0,request(0)).invoke&&!throttle.has_last_call&&!throttle.timer_active,
        "zero initialized sound owner cannot admit a zero generation request");
    notification_sound_throttle_generation(&throttle,7);
    u64 due=0;auto output=notification_sound_throttle_request(&throttle,0,request(7));
    check(output.invoke&&output.request.generation==7&&notification_sound_throttle_deadline(throttle,&due)&&due==1000,
        "zero epoch first throttled call emits leading sound and owns one exact deadline");
    check(!notification_sound_throttle_timer(&throttle,999,7).invoke&&throttle.timer_active,
        "early timer does not clear or emit leading window");
    check(!notification_sound_throttle_timer(&throttle,1000,7).invoke&&!throttle.timer_active,
        "single leading call never emits duplicate trailing sound");
    notification_sound_throttle_cancel(&throttle);
    notification_sound_throttle_request(&throttle,10,request(7,1));
    check(!notification_sound_throttle_request(&throttle,100,request(7,2)).invoke&&
        !notification_sound_throttle_request(&throttle,900,request(7,3,NotificationSoundKind::message3)).invoke,
        "burst replaces copied pending args instead of dropping all later sounds");
    output=notification_sound_throttle_timer(&throttle,1010,7);
    check(output.invoke&&output.request.soundpack==3&&output.request.kind==NotificationSoundKind::message3&&!throttle.timer_active,
        "trailing edge emits latest pack and sound at leading maxWait deadline");
    check(!notification_sound_throttle_request(&throttle,1011,request(7,4)).invoke&&
        notification_sound_throttle_deadline(throttle,&due)&&due==2011,
        "call immediately after trailing remains deferred until new debounce timer or maxWait request");
    output=notification_sound_throttle_request(&throttle,2010,request(7,5));
    check(output.invoke&&output.request.soundpack==5&&notification_sound_throttle_deadline(throttle,&due)&&due==3010,
        "tight-loop maxWait invokes latest current call and restarts timer without stale pending sound");
    check(!notification_sound_throttle_timer(&throttle,3010,7).invoke,"maxWait leading call without later burst has no trailing duplicate");
    notification_sound_throttle_cancel(&throttle);
    notification_sound_throttle_request(&throttle,4000,request(7,1));
    check(!notification_sound_throttle_request(&throttle,4100,request(7,2)).invoke,
        "rollback timer fixture retains a second request for the trailing edge");
    output=notification_sound_throttle_timer(&throttle,3900,7);
    check(output.invoke&&output.request.soundpack==2&&!throttle.timer_active&&throttle.last_invoke_ms==3900,
        "elapsed timer observing backwards clock emits latest trailing request despite old absolute deadline");
    notification_sound_throttle_cancel(&throttle);
    notification_sound_throttle_request(&throttle,5000,request(7));
    output=notification_sound_throttle_request(&throttle,4900,request(7,6));
    check(output.invoke&&output.request.soundpack==6&&notification_sound_throttle_deadline(throttle,&due)&&due==5900,
        "known backwards clock call follows Lodash invocation branch and restarts deadline");
    notification_sound_throttle_request(&throttle,5000,request(7,8));
    notification_sound_throttle_generation(&throttle,8);
    check(!notification_sound_throttle_deadline(throttle,&due)&&!notification_sound_throttle_timer(&throttle,6000,7).invoke&&
        !notification_sound_throttle_request(&throttle,6000,request(7)).invoke,
        "owner generation change cancels old pending request and rejects stale timer and producer");
    output=notification_sound_throttle_request(&throttle,6000,request(8,9));
    notification_sound_throttle_request(&throttle,6100,request(8,10));
    check(output.invoke&&!notification_sound_throttle_timer(&throttle,7000,7).invoke&&throttle.pending&&
        notification_sound_throttle_timer(&throttle,7000,8).invoke,"stale timer cannot consume current generation pending sound");
    notification_sound_throttle_cancel(&throttle);
    check(throttle.generation==8&&!notification_sound_throttle_deadline(throttle,&due)&&
        !notification_sound_throttle_timer(&throttle,8000,8).invoke,"explicit cancel preserves ownership and emits no stale trailing sound");
    notification_sound_throttle_request(&throttle,9000,request(8));
    notification_sound_throttle_request(&throttle,9100,request(8,11));
    notification_sound_throttle_generation(&throttle,0);
    check(!notification_sound_throttle_request(&throttle,9500,request(0)).invoke&&
        !notification_sound_throttle_timer(&throttle,10000,0).invoke&&!notification_sound_throttle_timer(&throttle,10000,8).invoke&&
        !throttle.timer_active&&!throttle.pending,
        "disabled zero owner cancels pending sound and rejects both zero and old timer generations");
    check(sizeof(NotificationSoundThrottle)<=64&&sizeof(NotificationSoundRequest)<=24,"sound scheduling owns bounded copied scalar state without tables");
}
} // namespace maicord

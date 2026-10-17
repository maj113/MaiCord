#include "maicord/model/notification_sound_admission.hpp"
namespace maicord {
namespace {
using R=MessageNotificationReason;using L=MessageNotificationLevel;using A=NotificationSoundDecision;
struct Fixture {
    MessageNotificationGuildSetting guilds[4]{};
    MessageNotificationOverride overrides[8]{};
    MessageNotificationThreadSetting threads[4]{};
    MessageNotificationStore store{};
    MessageNotificationPolicyInput policy{};
    NotificationSoundAdmissionInput sound{};
    MessageNotificationGuildSetting guild{};
    bool initialize() noexcept {
        if(!message_notifications_initialize(&store,{guilds,4},{overrides,8},{threads,4}))return false;
        guild.guild_id=10;guild.setting.fields=notification_level;guild.setting.level=L::all;
        if(message_notifications_apply_settings(&store,{&guild,1},{},false,1)!=MessageNotificationApplyError::none)return false;
        policy.settings=&store;policy.self_id=1;policy.author_id=2;policy.guild_id=10;policy.channel_id=20;
        policy.parent_id=21;policy.category_id=21;policy.now_ms=100;
        policy.ready=policy.live=policy.identities_known=policy.type_known=policy.guild_default_known=true;
        policy.guild_default=L::all;policy.global.fields=notification_status|notification_quiet;
        policy.global.status=MessageNotificationStatus::online;policy.required_gates=(1u<<16)-1;
        policy.known_gates=policy.required_gates;
        sound={&policy,true,false,true,false};return true;
    }
    bool settings(ConstSlice<MessageNotificationOverride> rows={}) noexcept {
        return message_notifications_apply_settings(&store,{&guild,1},rows,true,store.version+1)==MessageNotificationApplyError::none;
    }
};
MessageNotificationOverride level(u64 id,L value) noexcept {
    MessageNotificationOverride o{};o.guild_id=10;o.channel_id=id;o.setting.fields=notification_level;o.setting.level=value;return o;
}
}
void test_notification_sound_admission(void(*check)(bool,const char*)noexcept) noexcept {
    check(sizeof(NotificationSoundAdmissionInput)<=16&&sizeof(NotificationSoundAdmissions)<=4&&sizeof(MessageNotificationScope)<=4,
        "sound admission passes bounded scalar results and a borrowed policy pointer without owner tables");
    Fixture f{};check(f.initialize(),"sound admission fixture shares typed guild override and thread store");
    auto pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::allowed&&pair.selected==A::denied,"ordinary ALL admission is independent of default-disabled selected preference");
    f.policy.gates=notification_gate_streamer|notification_gate_overlay|notification_gate_restricted_hours|
        notification_gate_required_action|notification_gate_changelog;
    check(message_notification_admit(f.policy)==R::special&&notification_sound_ordinary_admit(f.policy)==R::eligible,
        "ordinary p excludes only later NotificationStore and popup transport gates");
    f.policy.gates=notification_gate_active_application;
    check(notification_sound_ordinary_admit(f.policy)==R::special,"active embedded application remains an ordinary lx gate");
    f.policy.gates=notification_gate_rpc_suppressed;
    check(notification_sound_ordinary_admit(f.policy)==R::special,"normalized applicable RPC suppression remains an ordinary gate");
    f.policy.gates=0;f.policy.focused=f.policy.selected=true;
    check(notification_sound_ordinary_admit(f.policy)==R::selected,"focused normalized main channel suppresses ordinary p");
    f.policy.selected=false;f.policy.sidebar_selected=true;
    check(notification_sound_ordinary_admit(f.policy)==R::selected,"focused sidebar selection also suppresses ordinary p");
    f.policy.focused=false;
    check(notification_sound_ordinary_admit(f.policy)==R::eligible,"blurred selected scope remains eligible for ordinary p");
    f.policy.sidebar_selected=false;f.sound.selected_preference=f.sound.original_main_selected=true;
    f.policy.message_flags=4096;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::denied&&pair.ordinary_reason==R::silent&&pair.selected==A::allowed,
        "selected kY does not inherit ordinary suppress-notifications message flag");
    f.policy.message_flags=0;f.policy.gates=notification_gate_age_access|notification_gate_lurking|
        notification_gate_request|notification_gate_own_activity|notification_gate_active_application|notification_gate_rpc_suppressed;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::denied&&pair.selected==A::allowed,"selected predicate omits ordinary age lurking request activity and RPC gates");
    f.policy.gates=0;f.policy.known_gates&=~notification_gate_spammer;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::unknown&&pair.selected==A::unknown,"unknown author spammer metadata cannot fabricate either sound admission");
    f.policy.known_gates=(1u<<16)-1;f.policy.known_gates&=~notification_gate_interaction_relationship;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::unknown&&pair.selected==A::unknown,"unsupported modern interaction identity fails closed on both predicates");
    f.policy.known_gates=(1u<<16)-1;f.policy.author_id=1;f.policy.self_mentionable_system=true;f.policy.message_type=46;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::allowed&&pair.selected==A::denied&&pair.selected_reason==R::self,
        "ordinary self-mentionable system exemption does not leak into selected sound");
    f.policy.author_id=2;f.policy.self_mentionable_system=false;f.policy.message_type=0;

    MessageNotificationOverride category=level(21,L::none);
    check(f.settings({&category,1})&&message_notification_scope_resolve(f.policy).reason==R::nothing&&
        notification_sound_selected_admit(f.sound)==R::nothing,"normal category NONE is immediate parent priority for both predicates");
    MessageNotificationOverride rows[2]={category,level(20,L::all)};
    check(f.settings({rows,2})&&notification_sound_ordinary_admit(f.policy)==R::eligible,
        "normal channel override takes priority over category notification level");
    rows[0].setting.fields|=notification_muted;rows[0].setting.muted=true;
    check(f.settings({rows,2})&&notification_sound_ordinary_admit(f.policy)==R::muted&&
        notification_sound_selected_admit(f.sound)==R::muted,"channel ALL cannot bypass immediate parent mute in generic scope");
    f.guild.setting.fields=notification_level|notification_flags;f.guild.setting.flags=16384;
    check(f.settings()&&notification_sound_selected_admit(f.sound)==R::not_opted_in,
        "generic selected helper includes guild channel opt-in requirement");
    auto opted=level(21,L::all);opted.setting.fields|=notification_flags;opted.setting.flags=4096;
    check(f.settings({&opted,1})&&notification_sound_selected_admit(f.sound)==R::eligible,
        "generic parent opt-in authorizes its selected child");
    f.guild.setting.flags=0;check(f.settings(),"opt-in fixture resets through typed settings entry");

    f.policy.channel_type=11;f.policy.channel_id=30;f.policy.parent_id=20;f.policy.category_id=21;
    f.sound.original_main_selected=true;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary_reason==R::not_joined&&pair.selected==A::allowed,
        "selected generic scope does not require joined thread membership");
    MessageNotificationThreadSetting thread{};thread.guild_id=10;thread.thread_id=30;
    thread.setting.fields=notification_flags|notification_muted;thread.setting.flags=2;thread.setting.muted=true;
    check(message_notifications_upsert_thread(&f.store,thread)==MessageNotificationApplyError::none,"joined thread mute fixture admitted");
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary_reason==R::muted&&pair.selected==A::allowed,
        "joined thread-specific mute suppresses p but is absent from generic selected kY");
    thread.setting.muted=false;thread.setting.flags=8;message_notifications_upsert_thread(&f.store,thread);
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary_reason==R::nothing&&pair.selected==A::allowed,"thread-specific NONE remains distinct from selected generic level");
    thread.setting.flags=1;message_notifications_upsert_thread(&f.store,thread);
    category=level(21,L::mentions);
    check(f.settings({&category,1})&&message_notification_scope_resolve(f.policy).level==L::mentions&&
        notification_sound_ordinary_admit(f.policy)==R::not_mentioned,
        "inherited thread resolves parent category notification level before guild ALL");
    f.policy.direct_mention=true;
    check(notification_sound_ordinary_admit(f.policy)==R::eligible,"inherited category mentions accepts a direct mention");f.policy.direct_mention=false;
    category.setting.level=L::none;
    check(f.settings({&category,1})&&notification_sound_ordinary_admit(f.policy)==R::nothing,
        "inherited thread parent category NONE suppresses rather than falling back to guild ALL");
    thread.setting.flags=2;message_notifications_upsert_thread(&f.store,thread);
    category.setting.fields|=notification_muted;category.setting.muted=true;
    check(f.settings({&category,1})&&notification_sound_ordinary_admit(f.policy)==R::eligible,
        "explicit joined thread ALL precedes grandparent category level and mute");
    auto parent=level(20,L::none);
    check(f.settings({&parent,1})&&notification_sound_ordinary_admit(f.policy)==R::nothing&&
        notification_sound_selected_admit(f.sound)==R::nothing,"generic immediate parent NONE still rejects even explicit thread ALL");
    check(f.settings(),"type21 fixture restores clear generic authority");

    // Scope is normalized to original thread's parent20; original main selected
    // remains30. Source p checks20 while T first compares raw channel30.
    f.policy.channel_id=20;f.policy.parent_id=21;f.policy.category_id=21;f.policy.channel_type=0;f.policy.message_type=21;
    f.policy.focused=true;f.policy.selected=false;f.sound.original_main_selected=true;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::allowed&&pair.selected==A::allowed,"type21 original selected thread can admit both normalized parent p and raw-selection T");
    f.policy.selected=true;f.sound.original_main_selected=false;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary_reason==R::selected&&pair.selected==A::denied,"type21 selected parent is not fabricated original thread selection");
    f.policy.selected=false;f.policy.focused=false;f.sound.original_main_selected=true;
    f.policy.global.fields=15;f.policy.global.status=MessageNotificationStatus::dnd;f.policy.global.quiet=true;
    f.policy.global.status_expires_ms=f.policy.global.focus_expires_ms=100;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::allowed&&pair.selected==A::allowed,"shared known finite DND and quiet expire at admission endpoint");
    f.policy.global.status_expires_ms=f.policy.global.focus_expires_ms=0;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::denied&&pair.selected==A::denied,"zero expiry preserves indefinite DND and quiet suppression");
    f.policy.global={};pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::unknown&&pair.selected==A::unknown,"missing entire global authority does not receive online quiet defaults");
    f.policy.global.fields=notification_status|notification_quiet;f.policy.global.status=MessageNotificationStatus::unknown;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::allowed&&pair.selected==A::allowed,"known valid UNKNOWN wrapper resolves connected ONLINE for both predicates");

    f.policy.channel_type=2;f.policy.global.status=MessageNotificationStatus::online;
    f.policy.current_voice_channel_known=false;f.policy.current_voice_channel=true;
    check(notification_sound_ordinary_admit(f.policy)==R::unknown,"ALL vocal channel requires voice membership knowledge before unconditional admission");
    f.policy.direct_mention=true;
    check(notification_sound_ordinary_admit(f.policy)==R::eligible,"direct mention does not require unrelated unknown voice-channel fact");
    f.policy.direct_mention=false;f.policy.current_voice_channel_known=true;f.policy.current_voice_channel=false;
    check(notification_sound_ordinary_admit(f.policy)==R::not_mentioned,"known disconnected vocal ALL falls back to mention admission");
    f.policy.current_voice_channel=true;
    check(notification_sound_ordinary_admit(f.policy)==R::eligible,"known current voice channel enables ALL fast path");
    f.policy.channel_type=0;u64 role=99;f.policy.mention_roles={&role,1};f.policy.roles_known=false;
    check(notification_sound_ordinary_admit(f.policy)==R::eligible,"ordinary nonvocal ALL does not demand irrelevant role membership");
    f.policy.duplicate=true;pair=notification_sound_admissions(f.sound);
    check(pair.ordinary_reason==R::duplicate&&pair.selected_reason==R::duplicate,"host duplicate barrier covers both source predicates");
    f.policy.duplicate=false;f.policy.live=false;pair=notification_sound_admissions(f.sound);
    check(pair.ordinary_reason==R::not_live&&pair.selected_reason==R::not_live,"host pre-resume barrier is shared without adding it to source gate mask");
    MessageNotificationGuildSetting private_setting{};private_setting.setting.fields=notification_level|notification_flags;
    private_setting.setting.level=L::all;private_setting.setting.flags=16384;
    check(message_notifications_apply_settings(&f.store,{&private_setting,1},{},true,f.store.version+1)==MessageNotificationApplyError::none,
        "null-guild private setting remains a valid typed scope");
    f.policy.live=true;f.policy.guild_id=0;f.policy.parent_id=f.policy.category_id=0;f.policy.channel_type=1;
    pair=notification_sound_admissions(f.sound);
    check(pair.ordinary==A::allowed&&pair.selected==A::allowed,
        "null guild cannot enable guild-only opt-in gate even when its numeric flag is present");
}
} // namespace maicord

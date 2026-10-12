#include "maicord/model/message_notifications.hpp"
namespace maicord {
namespace {
bool reject_reserve(void*,u32,u32,u32) noexcept{return false;}
MessageNotificationPolicyInput input(const MessageNotificationStore&s) noexcept {
    MessageNotificationPolicyInput i{};i.settings=&s;i.self_id=1;i.author_id=2;i.guild_id=10;i.channel_id=20;
    i.ready=i.live=i.identities_known=i.type_known=i.guild_default_known=true;
    i.guild_default=MessageNotificationLevel::all;i.global.fields=notification_status|notification_quiet;
    i.global.status=MessageNotificationStatus::online;i.desktop_enabled=i.sound_enabled=true;return i;
}
}
void test_notification_policy(void(*check)(bool,const char*)noexcept) noexcept {
    using E=MessageNotificationApplyError;using R=MessageNotificationReason;using L=MessageNotificationLevel;
    MessageNotificationGuildSetting gs[4]{};MessageNotificationOverride os[6]{};MessageNotificationThreadSetting ts[4]{};
    MessageNotificationStore s{};check(message_notifications_initialize(&s,{gs,4},{os,6},{ts,4}),"notification store initialization");
    auto i=input(s);check(message_notification_decide(i).reason==R::unknown,"unknown READY policy does not invent ALL");
    check(message_notifications_apply_settings(&s,{}, {},false,4)==E::none,"empty full READY establishes implicit defaults");
    check(message_notification_decide(i).popup,"ALL defaults admit unrelated live message");
    u64 unresolved_role=9;i.mention_roles={&unresolved_role,1};i.roles_known=false;
    check(message_notification_decide(i).popup,"ALL admits role mention without unresolved role membership blocking unconditional eligibility");i.mention_roles={};
    i.focused=i.selected=true;check(message_notification_decide(i).reason==R::selected,"focused selected suppresses");
    i.focused=false;check(message_notification_decide(i).popup,"unfocused selected can notify");i.selected=false;
    i.desktop_enabled=false;const auto sound=message_notification_decide(i);check(!sound.popup&&sound.sound,"desktop off retains eligible sound");i.desktop_enabled=true;
    i.message_flags=4096;check(message_notification_decide(i).reason==R::silent,"suppress notifications flag");i.message_flags=0;
    i.author_id=1;check(message_notification_decide(i).reason==R::self,"self messages suppressed");i.author_id=2;
    i.global.status=MessageNotificationStatus::dnd;check(message_notification_decide(i).reason==R::dnd,"DND suppresses");i.global.status=MessageNotificationStatus::online;
    i.global.quiet=true;check(message_notification_decide(i).reason==R::quiet,"quiet mode independent");i.global.quiet=false;
    i.required_gates=notification_gate_request;check(message_notification_decide(i).reason==R::unknown,"unknown request classification fails closed");
    i.known_gates=i.required_gates;i.gates=i.required_gates;check(message_notification_decide(i).reason==R::special,"known message request suppressed");i.required_gates=i.known_gates=i.gates=0;
    MessageNotificationGuildSetting guild{};guild.guild_id=10;guild.setting.fields=notification_level|notification_suppress_everyone|notification_suppress_roles;
    guild.setting.level=L::mentions;guild.setting.suppress_everyone=guild.setting.suppress_roles=true;
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,5)==E::none,"partial READY entry");
    i.everyone_mention=true;check(message_notification_decide(i).reason==R::not_mentioned,"suppressed everyone");
    i.direct_mention=true;check(message_notification_decide(i).popup,"direct mention survives suppress toggles");i.direct_mention=i.everyone_mention=false;
    u64 role=90;i.mention_roles={&role,1};i.self_roles={&role,1};i.roles_known=true;
    check(message_notification_decide(i).reason==R::not_mentioned,"suppressed roles");guild.setting.suppress_roles=false;
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,6)==E::none,"role suppression change");check(message_notification_decide(i).popup,"self role mention");
    guild.setting.fields=notification_muted|notification_mute_end;guild.setting.muted=true;guild.setting.mute_end_ms=100;
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,7)==E::none,"temporary mute");i.now_ms=100;i.direct_mention=true;
    check(message_notification_decide(i).reason==R::muted,"mute includes direct mentions and inclusive endpoint");i.now_ms=101;check(message_notification_decide(i).popup,"mute expires without changing stored fields");
    MessageNotificationOverride override{};override.guild_id=10;override.channel_id=21;override.setting.fields=notification_level;override.setting.level=L::none;
    guild.setting.fields=0;check(message_notifications_apply_settings(&s,{&guild,1},{&override,1},true,8)==E::none,"parent override stored");i.parent_id=21;
    check(message_notification_decide(i).reason==R::nothing,"parent NONE suppresses direct mention");
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,9)==E::none&&s.override_count==0,"full entry replaces old override set");
    const u32 before_stale=s.revision;
    check(message_notifications_apply_settings(&s,{&guild,1},{&override,1},true,8)==E::stale&&s.version==9&&s.revision==before_stale&&s.override_count==0,
        "stale complete-store entry preserves version and override authority");
    s.complete=false;
    check(message_notifications_apply_settings(&s,{&guild,1},{&override,1},true,8)==E::stale&&s.version==9&&s.revision==before_stale&&s.override_count==0,
        "known partial-store version rejects stale entry before publication");
    s.complete=true;
    i.channel_type=11;i.channel_id=30;check(message_notification_decide(i).reason==R::not_joined,"unjoined thread cannot notify");
    MessageNotificationThreadSetting thread{};thread.guild_id=10;thread.thread_id=30;thread.setting.fields=notification_flags|notification_muted;thread.setting.flags=4;
    check(message_notifications_upsert_thread(&s,thread)==E::none,"all joined threads independent of selected participant cache");
    i.direct_mention=false;i.everyone_mention=true;check(message_notification_decide(i).popup,"thread mention predicate ignores guild everyone suppression");
    thread.setting.fields=notification_flags;thread.setting.flags=8;check(message_notifications_upsert_thread(&s,thread)==E::none,"sparse thread update preserves muted field");check(message_notification_decide(i).reason==R::nothing,"thread explicit NONE");
    const u32 revision=s.revision;s.reserve=reject_reserve;check(message_notifications_apply_settings(&s,{&guild,1},{},true,10)==E::capacity&&s.revision==revision,"reserve rejection publishes nothing");s.reserve=nullptr;
    MessageNotificationGuildSetting duplicates[2]={guild,guild};check(message_notifications_apply_settings(&s,{duplicates,2},{},true,10)==E::duplicate&&s.revision==revision,"duplicate preflight leaves active projection intact");
    message_notifications_mark_incomplete(&s,10);check(message_notification_decide(i).reason==R::unknown,"scope incomplete suppression");
    message_notifications_remove_guild(&s,10);check(s.thread_count==0,"guild deletion clears self thread state");
    MessageNotificationGlobal global{100,200,15,true,MessageNotificationStatus::dnd},partial{};partial.fields=notification_quiet;partial.quiet=false;
    message_notifications_apply_global(&global,partial,true);check(!global.quiet&&global.status==MessageNotificationStatus::dnd&&global.focus_expires_ms==100,"global partial preserves status and expiry");
    message_notifications_apply_global(&global,partial,false);check(global.status==MessageNotificationStatus::unknown&&global.focus_expires_ms==0,"global full resets omitted proto subset");
    MessageNotificationGuildSetting age_guilds[2]{};MessageNotificationOverride age_overrides[3]{};MessageNotificationThreadSetting age_threads[2]{};
    MessageNotificationStore age_store{};message_notifications_initialize(&age_store,{age_guilds,2},{age_overrides,3},{age_threads,2});
    MessageNotificationGuildSetting clear_guild{};clear_guild.guild_id=10;clear_guild.setting.fields=notification_level;clear_guild.setting.level=L::all;
    MessageNotificationOverride muted_category{};muted_category.guild_id=10;muted_category.channel_id=22;
    muted_category.setting.fields=notification_muted;muted_category.setting.muted=true;
    check(message_notifications_apply_settings(&age_store,{&clear_guild,1},{&muted_category,1},false,1)==E::none,"thread category fixture admitted");
    auto edge=input(age_store);edge.channel_id=30;edge.parent_id=21;edge.category_id=22;edge.channel_type=19;
    check(message_notification_decide(edge).reason==R::not_joined,"MEDIA_THREAD19 uses joined thread admission");
    thread.setting.fields=notification_flags|notification_muted;thread.setting.flags=2;thread.setting.muted=false;
    message_notifications_upsert_thread(&age_store,thread);
    check(message_notification_decide(edge).popup,"explicit thread ALL resolves before grandparent category mute");
    thread.setting.flags=1;message_notifications_upsert_thread(&age_store,thread);
    check(message_notification_decide(edge).reason==R::muted,"inherited thread level checks parent category mute");
    edge.channel_type=18;edge.category_id=0;
    check(message_notification_decide(edge).popup,"DM_SDK18 does not require thread membership or flags");
    edge.required_gates=notification_gate_age_access|notification_gate_interaction_relationship;
    check(message_notification_decide(edge).reason==R::unknown,"missing age and interaction relationship knowledge fails closed");
    edge.known_gates=edge.required_gates;edge.gates=notification_gate_age_access;
    check(message_notification_decide(edge).reason==R::special,"known sensitive age scope suppresses without fabricated consent");
    edge.gates=notification_gate_interaction_relationship;
    check(message_notification_decide(edge).reason==R::special,"known interaction relationship suppresses independently of author");
}
} // namespace maicord

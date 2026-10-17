#include "test_output.hpp"
#include "maicord/model/message_notifications.hpp"
namespace maicord {
namespace {
bool reject_reserve(void*,u32,u32,u32) noexcept{return false;}
MessageNotificationPolicyInput input(const MessageNotificationStore&s) noexcept {
    MessageNotificationPolicyInput i{};i.settings=&s;i.self_id=1;i.author_id=2;i.guild_id=10;i.channel_id=20;
    i.ready=i.live=i.identities_known=i.type_known=i.guild_default_known=true;
    i.guild_default=MessageNotificationLevel::all;i.global.fields=notification_status|notification_quiet;
    i.global.status=MessageNotificationStatus::online;return i;
}
bool eligible(const MessageNotificationPolicyInput& i) noexcept {
    return message_notification_admit(i)==MessageNotificationReason::eligible;
}
}
void test_notification_policy(void(*check)(bool,const char*)noexcept) noexcept {
    using E=MessageNotificationApplyError;using R=MessageNotificationReason;using L=MessageNotificationLevel;
    MessageNotificationGuildSetting gs[4]{};MessageNotificationOverride os[6]{};MessageNotificationThreadSetting ts[4]{};
    MessageNotificationStore s{};check(message_notifications_initialize(&s,{gs,4},{os,6},{ts,4}),"notification store initialization");
    auto i=input(s);check(message_notification_admit(i)==R::unknown,"unknown READY policy does not invent ALL");
    check(message_notifications_apply_settings(&s,{}, {},false,4)==E::none,"empty full READY establishes implicit defaults");
    check(eligible(i),"ALL defaults admit unrelated live message");
    u64 unresolved_role=9;i.mention_roles={&unresolved_role,1};i.roles_known=false;
    check(eligible(i),"ALL admits role mention without unresolved role membership blocking unconditional eligibility");i.mention_roles={};
    i.focused=i.selected=true;check(message_notification_admit(i)==R::selected,"focused selected suppresses");
    i.focused=false;check(eligible(i),"unfocused selected can notify");i.selected=false;
    i.message_flags=4096;check(message_notification_admit(i)==R::silent,"suppress notifications flag");i.message_flags=0;
    i.author_id=1;check(message_notification_admit(i)==R::self,"self messages suppressed");i.author_id=2;
    i.global.status=MessageNotificationStatus::dnd;check(message_notification_admit(i)==R::dnd,"DND suppresses");i.global.status=MessageNotificationStatus::online;
    i.global.quiet=true;check(message_notification_admit(i)==R::quiet,"quiet mode independent");i.global.quiet=false;
    i.required_gates=notification_gate_request;check(message_notification_admit(i)==R::unknown,"unknown request classification fails closed");
    i.known_gates=i.required_gates;i.gates=i.required_gates;check(message_notification_admit(i)==R::special,"known message request suppressed");i.required_gates=i.known_gates=i.gates=0;
    MessageNotificationGuildSetting guild{};guild.guild_id=10;guild.setting.fields=notification_level|notification_suppress_everyone|notification_suppress_roles;
    guild.setting.level=L::mentions;guild.setting.suppress_everyone=guild.setting.suppress_roles=true;
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,5)==E::none,"partial READY entry");
    i.everyone_mention=true;check(message_notification_admit(i)==R::not_mentioned,"suppressed everyone");
    i.direct_mention=true;check(eligible(i),"direct mention survives suppress toggles");i.direct_mention=i.everyone_mention=false;
    u64 role=90;i.mention_roles={&role,1};i.self_roles={&role,1};i.roles_known=true;
    check(message_notification_admit(i)==R::not_mentioned,"suppressed roles");guild.setting.suppress_roles=false;
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,6)==E::none,"role suppression change");check(eligible(i),"self role mention");
    guild.setting.fields=notification_muted|notification_mute_end;guild.setting.muted=true;guild.setting.mute_end_ms=100;
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,7)==E::none,"temporary mute");i.now_ms=100;i.direct_mention=true;
    check(message_notification_admit(i)==R::muted,"mute includes direct mentions and inclusive endpoint");i.now_ms=101;check(eligible(i),"mute expires without changing stored fields");
    MessageNotificationOverride override{};override.guild_id=10;override.channel_id=21;override.setting.fields=notification_level;override.setting.level=L::none;
    guild.setting.fields=0;check(message_notifications_apply_settings(&s,{&guild,1},{&override,1},true,8)==E::none,"parent override stored");i.parent_id=21;
    check(message_notification_admit(i)==R::nothing,"parent NONE suppresses direct mention");
    check(message_notifications_apply_settings(&s,{&guild,1},{},true,9)==E::none&&s.override_count==0,"full entry replaces old override set");
    const u32 before_stale=s.revision;
    check(message_notifications_apply_settings(&s,{&guild,1},{&override,1},true,8)==E::stale&&s.version==9&&s.revision==before_stale&&s.override_count==0,
        "stale complete-store entry preserves version and override authority");
    s.complete=false;
    check(message_notifications_apply_settings(&s,{&guild,1},{&override,1},true,8)==E::stale&&s.version==9&&s.revision==before_stale&&s.override_count==0,
        "known partial-store version rejects stale entry before publication");
    s.complete=true;
    i.channel_type=11;i.channel_id=30;check(message_notification_admit(i)==R::not_joined,"unjoined thread cannot notify");
    MessageNotificationThreadSetting thread{};thread.guild_id=10;thread.thread_id=30;thread.setting.fields=notification_flags|notification_muted;thread.setting.flags=4;
    check(message_notifications_upsert_thread(&s,thread)==E::none,"all joined threads independent of selected participant cache");
    i.direct_mention=false;i.everyone_mention=true;check(eligible(i),"thread mention predicate ignores guild everyone suppression");
    thread.setting.fields=notification_flags;thread.setting.flags=8;check(message_notifications_upsert_thread(&s,thread)==E::none,"sparse thread update preserves muted field");check(message_notification_admit(i)==R::nothing,"thread explicit NONE");
    const u32 revision=s.revision;s.reserve=reject_reserve;check(message_notifications_apply_settings(&s,{&guild,1},{},true,10)==E::capacity&&s.revision==revision,"reserve rejection publishes nothing");s.reserve=nullptr;
    MessageNotificationGuildSetting duplicates[2]={guild,guild};check(message_notifications_apply_settings(&s,{duplicates,2},{},true,10)==E::duplicate&&s.revision==revision,"duplicate preflight leaves active projection intact");
    message_notifications_mark_incomplete(&s,10);check(message_notification_admit(i)==R::unknown,"scope incomplete suppression");
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
    check(message_notification_admit(edge)==R::not_joined,"MEDIA_THREAD19 uses joined thread admission");
    thread.setting.fields=notification_flags|notification_muted;thread.setting.flags=2;thread.setting.muted=false;
    message_notifications_upsert_thread(&age_store,thread);
    check(eligible(edge),"explicit thread ALL resolves before grandparent category mute");
    thread.setting.flags=1;message_notifications_upsert_thread(&age_store,thread);
    check(message_notification_admit(edge)==R::muted,"inherited thread level checks parent category mute");
    edge.channel_type=18;edge.category_id=0;
    check(eligible(edge),"DM_SDK18 does not require thread membership or flags");
    edge.required_gates=notification_gate_age_access|notification_gate_interaction_relationship;
    check(message_notification_admit(edge)==R::unknown,"missing age and interaction relationship knowledge fails closed");
    edge.known_gates=edge.required_gates;edge.gates=notification_gate_age_access;
    check(message_notification_admit(edge)==R::special,"known sensitive age scope suppresses without fabricated consent");
    edge.gates=notification_gate_interaction_relationship;
    check(message_notification_admit(edge)==R::special,"known interaction relationship suppresses independently of author");

    MessageNotificationGuildSetting badge_guilds[2]{};MessageNotificationOverride badge_overrides[3]{};
    MessageNotificationThreadSetting badge_threads[2]{};MessageNotificationStore badge_settings{};
    check(message_notifications_initialize(&badge_settings,{badge_guilds,2},{badge_overrides,3},{badge_threads,2})&&
        message_notifications_apply_settings(&badge_settings,{},{},false,1)==E::none,
        "read badge: complete empty settings establish private defaults");
    auto badge=input(badge_settings);badge.guild_id=0;badge.channel_type=1;
    badge.known_gates=notification_gate_blocked|notification_gate_ignored;
    badge.global.quiet=true;badge.global.status=MessageNotificationStatus::dnd;
    badge.message_flags=4096;badge.focused=badge.selected=true;
    using B=MessageReadBadge;
    check(message_read_badge(badge,false)==B::mention&&message_notification_admit(badge)==R::silent,
        "read badge: private messages count independently of silent, DND, quiet and focus");
    MessageNotificationGuildSetting badge_guild{};badge_guild.guild_id=0;
    badge_guild.setting.fields=notification_muted;badge_guild.setting.muted=true;
    check(message_notifications_apply_settings(&badge_settings,{&badge_guild,1},{},true,2)==E::none&&
        message_read_badge(badge,false)==B::none,"read badge: muted private messages do not count by default");
    badge.direct_mention=true;
    check(message_read_badge(badge,false)==B::mention,"read badge: explicit mention survives private mute");
    badge.message_type=2;
    check(message_read_badge(badge,true)==B::none,"read badge: private recipient removal never invents a mention");
    badge.message_type=0;badge.gates=notification_gate_blocked;
    check(message_read_badge(badge,false)==B::none,"read badge: blocked author cannot count a direct mention");
    badge.gates=notification_gate_ignored;
    check(message_read_badge(badge,false)==B::none,"read badge: ignored author cannot count a direct mention");
    badge.gates=0;badge.known_gates=0;
    check(message_read_badge(badge,false)==B::none,"read badge: unknown relationship authority does not fabricate counts");
    badge.known_gates=notification_gate_blocked|notification_gate_ignored;
    badge.guild_id=10;badge.channel_type=0;badge.direct_mention=false;badge.everyone_mention=true;
    badge_guild.guild_id=10;badge_guild.setting.fields=notification_level|notification_muted|notification_suppress_everyone|notification_suppress_roles;
    badge_guild.setting.level=L::none;badge_guild.setting.muted=true;
    badge_guild.setting.suppress_everyone=badge_guild.setting.suppress_roles=true;
    check(message_notifications_apply_settings(&badge_settings,{&badge_guild,1},{},true,3)==E::none&&
        message_read_badge(badge,false)==B::none,"read badge: suppressed everyone does not count");
    badge.everyone_mention=false;badge.mention_roles={&role,1};badge.self_roles={&role,1};badge.roles_known=true;
    check(message_read_badge(badge,false)==B::none,"read badge: suppressed matching role does not count");
    badge.direct_mention=true;
    check(message_read_badge(badge,false)==B::mention,"read badge: guild direct mention survives mute and notification NONE");
    badge.direct_mention=false;badge_guild.setting.suppress_roles=false;
    check(message_notifications_apply_settings(&badge_settings,{&badge_guild,1},{},true,4)==E::none&&
        message_read_badge(badge,false)==B::mention,"read badge: unsuppressed matching role survives mute and NONE");
    badge.mention_roles={};badge.self_roles={};badge_guild.setting.muted=false;badge_guild.setting.level=L::all;
    check(message_notifications_apply_settings(&badge_settings,{&badge_guild,1},{},true,5)==E::none&&
        message_read_badge(badge,false)==B::none&&message_read_badge(badge,true)==B::low_importance,
        "read badge: all-message count requires explicit account setting and is low importance");
    badge.channel_type=2;
    check(message_read_badge(badge,true)==B::none,"read badge: voice all-message mode does not add non-ping counts");
    badge.channel_type=13;
    check(message_read_badge(badge,true)==B::none,"read badge: stage all-message mode does not add non-ping counts");
    badge.channel_type=0;badge.author_id=1;
    check(message_read_badge(badge,true)==B::none,"read badge: ordinary own messages do not badge themselves");
    badge.author_id=2;badge.channel_type=11;badge.channel_id=30;badge.parent_id=20;
    check(message_read_badge(badge,true)==B::none,"read badge: missing self-thread setting does not inherit ALL");
    MessageNotificationThreadSetting badge_thread{};badge_thread.guild_id=10;badge_thread.thread_id=30;
    badge_thread.setting.fields=notification_flags|notification_muted;badge_thread.setting.flags=8;
    check(message_notifications_upsert_thread(&badge_settings,badge_thread)==E::none&&
        message_read_badge(badge,true)==B::none,"read badge: thread NONE overrides inherited ALL for ordinary messages");
    badge_thread.setting.flags=2;badge_guild.setting.level=L::mentions;
    check(message_notifications_upsert_thread(&badge_settings,badge_thread)==E::none&&
        message_notifications_apply_settings(&badge_settings,{&badge_guild,1},{},true,6)==E::none&&
        message_read_badge(badge,true)==B::low_importance,"read badge: explicit thread ALL overrides inherited mentions");
    badge_guild.setting.muted=true;
    check(message_notifications_apply_settings(&badge_settings,{&badge_guild,1},{},true,7)==E::none&&
        message_read_badge(badge,true)==B::low_importance,"read badge: explicit thread ALL resolves before ancestor mute");
    badge_thread.setting.flags=4;
    check(message_notifications_upsert_thread(&badge_settings,badge_thread)==E::none&&
        message_read_badge(badge,true)==B::none,"read badge: thread mentions mode excludes ordinary messages");
    badge.direct_mention=true;badge_thread.setting.flags=8;
    check(message_notifications_upsert_thread(&badge_settings,badge_thread)==E::none&&
        message_read_badge(badge,true)==B::mention,"read badge: direct thread mention survives thread NONE");
}
} // namespace maicord


namespace maicord {
void test_notification_settings_wire(void (*)(bool, const char*) noexcept) noexcept;
void test_desktop_notification_queue(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_sound_policy(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_sound_admission(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_controls(void (*)(bool, const char*) noexcept) noexcept;
namespace notifications_entry {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_notifications_tests_entry() noexcept {
    maicord::test_notification_policy(maicord::notifications_entry::check);
    maicord::test_notification_settings_wire(maicord::notifications_entry::check);
    maicord::test_desktop_notification_queue(maicord::notifications_entry::check);
    maicord::test_notification_sound_policy(maicord::notifications_entry::check);
    maicord::test_notification_sound_admission(maicord::notifications_entry::check);
    maicord::test_notification_controls(maicord::notifications_entry::check);
    ExitProcess(maicord::notifications_entry::failures ? 1 : 0);
}

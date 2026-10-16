#include "maicord/model/message_notifications.hpp"
#include "maicord/core/byte_range.hpp"
namespace maicord {
namespace {
constexpr u16 all_fields = 255;
bool valid(const MessageNotificationSetting& s) noexcept {
    return !(s.fields & ~all_fields) && static_cast<u8>(s.level) <= 3;
}
MessageNotificationSetting baseline() noexcept {
    MessageNotificationSetting s{}; s.fields=all_fields; s.level=MessageNotificationLevel::inherit;
    s.mobile_push=true; return s;
}
MessageNotificationSetting merge(MessageNotificationSetting a, const MessageNotificationSetting& b) noexcept {
    if(b.fields&notification_level)a.level=b.level;
    if(b.fields&notification_muted)a.muted=b.muted;
    if(b.fields&notification_mute_end)a.mute_end_ms=b.mute_end_ms;
    if(b.fields&notification_suppress_everyone)a.suppress_everyone=b.suppress_everyone;
    if(b.fields&notification_suppress_roles)a.suppress_roles=b.suppress_roles;
    if(b.fields&notification_mobile_push)a.mobile_push=b.mobile_push;
    if(b.fields&notification_flags)a.flags=b.flags;
    if(b.fields&notification_hide_muted)a.hide_muted_channels=b.hide_muted_channels;
    a.fields|=b.fields; return a;
}
template<class T> bool slice_valid(ConstSlice<T> s) noexcept {
    return s.count<=~u32{0}/sizeof(T)&&byte_range_valid({reinterpret_cast<const u8*>(s.data),s.count*static_cast<u32>(sizeof(T))});
}
template<class T,class U> bool overlaps(ConstSlice<T> a,Slice<U>b) noexcept {
    return byte_ranges_overlap({reinterpret_cast<const u8*>(a.data),a.count*static_cast<u32>(sizeof(T))},
        {reinterpret_cast<const u8*>(b.data),b.count*static_cast<u32>(sizeof(U))});
}
bool has_guild(ConstSlice<MessageNotificationGuildSetting> s,u64 id) noexcept {
    for(u32 i=0;i<s.count;++i)if(s[i].guild_id==id)return true;return false;
}
MessageNotificationDecision reject(MessageNotificationReason r) noexcept {return {r,false,false};}
}
bool message_notifications_initialize(MessageNotificationStore* s,Slice<MessageNotificationGuildSetting>g,
    Slice<MessageNotificationOverride>o,Slice<MessageNotificationThreadSetting>t,MessageNotificationReserve r,void*c) noexcept {
    if(!s||!slice_valid(ConstSlice<MessageNotificationGuildSetting>{g.data,g.count})||
       !slice_valid(ConstSlice<MessageNotificationOverride>{o.data,o.count})||
       !slice_valid(ConstSlice<MessageNotificationThreadSetting>{t.data,t.count})||
       overlaps(ConstSlice<MessageNotificationGuildSetting>{g.data,g.count},o)||
       overlaps(ConstSlice<MessageNotificationGuildSetting>{g.data,g.count},t)||
       overlaps(ConstSlice<MessageNotificationOverride>{o.data,o.count},t))return false;
    *s={};s->guilds=g;s->overrides=o;s->threads=t;s->reserve=r;s->reserve_context=c;return true;
}
const MessageNotificationGuildSetting* message_notifications_guild(const MessageNotificationStore&s,u64 id) noexcept {
    for(u32 i=0;i<s.guild_count;++i)if(s.guilds[i].guild_id==id)return &s.guilds[i];return nullptr;
}
const MessageNotificationOverride* message_notifications_override(const MessageNotificationStore&s,u64 g,u64 id) noexcept {
    for(u32 i=0;i<s.override_count;++i)if(s.overrides[i].guild_id==g&&s.overrides[i].channel_id==id)return &s.overrides[i];return nullptr;
}
const MessageNotificationThreadSetting* message_notifications_thread(const MessageNotificationStore&s,u64 id) noexcept {
    for(u32 i=0;i<s.thread_count;++i)if(s.threads[i].thread_id==id)return &s.threads[i];return nullptr;
}
MessageNotificationApplyError message_notifications_apply_settings(MessageNotificationStore*s,
    ConstSlice<MessageNotificationGuildSetting>g,ConstSlice<MessageNotificationOverride>o,bool partial,u64 version) noexcept {
    using E=MessageNotificationApplyError;
    if(!s||!slice_valid(g)||!slice_valid(o)||s->guild_count>s->guilds.count||s->override_count>s->overrides.count||
       overlaps(g,s->guilds)||overlaps(g,s->overrides)||overlaps(g,s->threads)||
       overlaps(o,s->guilds)||overlaps(o,s->overrides)||overlaps(o,s->threads))return E::input;
    if(version<s->version)return E::stale;
    for(u32 i=0;i<g.count;++i){if(!valid(g[i].setting))return E::input;
        for(u32 j=0;j<i;++j)if(g[j].guild_id==g[i].guild_id)return E::duplicate;}
    for(u32 i=0;i<o.count;++i){if(!o[i].channel_id||!valid(o[i].setting)||!has_guild(g,o[i].guild_id))return E::input;
        for(u32 j=0;j<i;++j)if(o[j].guild_id==o[i].guild_id&&o[j].channel_id==o[i].channel_id)return E::duplicate;}
    u32 ng=g.count,no=o.count;
    if(partial){for(u32 i=0;i<s->guild_count;++i)if(!has_guild(g,s->guilds[i].guild_id))++ng;
        for(u32 i=0;i<s->override_count;++i)if(!has_guild(g,s->overrides[i].guild_id))++no;}
    if(ng>s->guilds.count||no>s->overrides.count)return E::capacity;
    if(s->reserve&&!s->reserve(s->reserve_context,ng,no,s->thread_count))return E::capacity;
    // Guild values must merge before old rows are compacted; every incoming ID
    // appears exactly once and there are no remaining failure paths.
    if(!partial){s->guild_count=0;s->override_count=0;}
    else {u32 n=0;for(u32 i=0;i<s->override_count;++i)if(!has_guild(g,s->overrides[i].guild_id))s->overrides[n++]=s->overrides[i];s->override_count=n;}
    for(u32 i=0;i<g.count;++i){u32 at=0;while(at<s->guild_count&&s->guilds[at].guild_id!=g[i].guild_id)++at;
        const auto old=at<s->guild_count?s->guilds[at].setting:baseline();
        if(at==s->guild_count)++s->guild_count;s->guilds[at]={g[i].guild_id,merge(old,g[i].setting)};}
    for(u32 i=0;i<o.count;++i)s->overrides[s->override_count++]={o[i].guild_id,o[i].channel_id,merge(baseline(),o[i].setting)};
    s->version=version;s->complete=s->complete||!partial;++s->revision;return E::none;
}
MessageNotificationApplyError message_notifications_upsert_thread(MessageNotificationStore*s,const MessageNotificationThreadSetting&t) noexcept {
    using E=MessageNotificationApplyError;if(!s||!t.thread_id||!t.guild_id||!valid(t.setting))return E::input;
    u32 at=0;while(at<s->thread_count&&s->threads[at].thread_id!=t.thread_id)++at;
    if(at==s->thread_count){if(at>=s->threads.count||
        (s->reserve&&!s->reserve(s->reserve_context,s->guild_count,s->override_count,at+1)))return E::capacity;
        s->threads[at]={t.guild_id,t.thread_id,{}};++s->thread_count;}
    s->threads[at].guild_id=t.guild_id;s->threads[at].setting=merge(s->threads[at].setting,t.setting);++s->revision;return E::none;
}
void message_notifications_remove_thread(MessageNotificationStore*s,u64 id) noexcept {
    if(!s)return;for(u32 i=0;i<s->thread_count;++i)if(s->threads[i].thread_id==id){s->threads[i]=s->threads[--s->thread_count];++s->revision;return;}}
void message_notifications_remove_guild(MessageNotificationStore*s,u64 id) noexcept {
    if(!s)return;u32 n=0;for(u32 i=0;i<s->guild_count;++i)if(s->guilds[i].guild_id!=id)s->guilds[n++]=s->guilds[i];s->guild_count=n;
    n=0;for(u32 i=0;i<s->override_count;++i)if(s->overrides[i].guild_id!=id)s->overrides[n++]=s->overrides[i];s->override_count=n;
    n=0;for(u32 i=0;i<s->thread_count;++i)if(s->threads[i].guild_id!=id)s->threads[n++]=s->threads[i];s->thread_count=n;++s->revision;
}
void message_notifications_clear(MessageNotificationStore*s) noexcept {if(s){s->guild_count=s->override_count=s->thread_count=0;s->complete=false;s->version=0;++s->revision;}}
void message_notifications_mark_incomplete(MessageNotificationStore*s,u64 id) noexcept {
    if(!s)return;for(u32 n=0;n<s->guild_count;++n)if(s->guilds[n].guild_id==id){s->guilds[n].incomplete=true;++s->revision;return;}
    s->complete=false;++s->revision;
}
bool message_notification_muted(const MessageNotificationSetting&s,u64 now) noexcept {return s.muted&&(!s.mute_end_ms||s.mute_end_ms>=now);}
bool message_notifications_guild_muted(const MessageNotificationStore&s,u64 guild,u64 now) noexcept {
    const auto*g=guild?message_notifications_guild(s,guild):nullptr;return g&&message_notification_muted(g->setting,now);}
bool message_notifications_override_muted(const MessageNotificationStore&s,u64 guild,u64 channel,u64 now) noexcept {
    const auto*o=channel?message_notifications_override(s,guild,channel):nullptr;return o&&message_notification_muted(o->setting,now);}
u64 message_notifications_next_mute_expiry(const MessageNotificationStore&s,u64 now) noexcept {
    u64 next=0;const auto consider=[&](const MessageNotificationSetting&v) noexcept {
        if(v.muted&&v.mute_end_ms&&v.mute_end_ms>=now&&(!next||v.mute_end_ms<next))next=v.mute_end_ms;};
    for(u32 i=0;i<s.guild_count;++i)consider(s.guilds[i].setting);
    for(u32 i=0;i<s.override_count;++i)consider(s.overrides[i].setting);
    for(u32 i=0;i<s.thread_count;++i)consider(s.threads[i].setting);
    return next;
}
void message_notifications_apply_global(MessageNotificationGlobal*a,const MessageNotificationGlobal&b,bool partial) noexcept {
    if(!a)return;if(!partial)*a={0,0,15,false,MessageNotificationStatus::unknown};
    if(b.fields&notification_quiet)a->quiet=b.quiet;
    if(b.fields&notification_focus_expiry)a->focus_expires_ms=b.focus_expires_ms;
    if(b.fields&notification_status)a->status=b.status;
    if(b.fields&notification_status_expiry)a->status_expires_ms=b.status_expires_ms;a->fields|=b.fields;
}
MessageNotificationGlobal message_notification_effective_global(const MessageNotificationGlobal& value,u64 now,bool connected) noexcept {
    auto g=value;
    if(!connected)return g;
    const bool status_valid=static_cast<u8>(g.status)<=static_cast<u8>(MessageNotificationStatus::offline);
    if((g.fields&notification_status)&&status_valid) {
        if(g.status==MessageNotificationStatus::unknown)g.status=MessageNotificationStatus::online;
        if((g.fields&notification_status_expiry)&&g.status_expires_ms&&g.status_expires_ms<=now)
            g.status=MessageNotificationStatus::online;
    }
    if((g.fields&(notification_quiet|notification_focus_expiry))==(notification_quiet|notification_focus_expiry)&&
        g.focus_expires_ms&&g.focus_expires_ms<=now)g.quiet=false;
    return g;
}
MessageNotificationScope message_notification_scope_resolve(const MessageNotificationPolicyInput&i,MessageNotificationScopeMode mode) noexcept {
    using R=MessageNotificationReason;using L=MessageNotificationLevel;
    const auto fail=[](R reason) noexcept {return MessageNotificationScope{reason,L::inherit,false,false};};
    if(!i.settings||!i.channel_id||static_cast<u8>(mode)>static_cast<u8>(MessageNotificationScopeMode::message_thread))return fail(R::unknown);
    const auto*g=message_notifications_guild(*i.settings,i.guild_id);
    if((!g&&!i.settings->complete)||(g&&g->incomplete))return fail(R::unknown);
    auto gs=g?g->setting:baseline();
    const auto*c=message_notifications_override(*i.settings,i.guild_id,i.channel_id);
    const auto*p=message_notifications_override(*i.settings,i.guild_id,i.parent_id);
    const bool thread=i.channel_type==10||i.channel_type==11||i.channel_type==12||i.channel_type==19;
    if(message_notification_muted(gs,i.now_ms)||(c&&message_notification_muted(c->setting,i.now_ms))||
       (p&&message_notification_muted(p->setting,i.now_ms)))return fail(R::muted);
    L level=c?c->setting.level:L::inherit;if(level==L::inherit&&p)level=p->setting.level;
    if(level==L::inherit)level=gs.level;
    if(level==L::inherit){if(!i.guild_default_known)return fail(R::unknown);level=i.guild_default;}
    if(level==L::inherit||static_cast<u8>(level)>static_cast<u8>(L::inherit))return fail(R::unknown);
    if(level==L::none)return fail(R::nothing);
    if(i.guild_id&&(gs.flags&16384)&&!(c&&(c->setting.flags&4096))&&!(p&&(p->setting.flags&4096)))return fail(R::not_opted_in);
    bool se=gs.suppress_everyone,sr=gs.suppress_roles;
    if(thread&&mode==MessageNotificationScopeMode::message_thread){
        const auto*t=message_notifications_thread(*i.settings,i.channel_id);if(!t)return fail(R::not_joined);
        if(t->guild_id!=i.guild_id||(t->setting.fields&(notification_flags|notification_muted))!=(notification_flags|notification_muted))return fail(R::unknown);
        if(message_notification_muted(t->setting,i.now_ms))return fail(R::muted);
        const u32 f=t->setting.flags;if(f&2)level=L::all;else if(f&4)level=L::mentions;else if(f&8)level=L::none;
        else {if(!i.parent_id)return fail(R::unknown);
            const auto*cat=message_notifications_override(*i.settings,i.guild_id,i.category_id);
            if(cat&&message_notification_muted(cat->setting,i.now_ms))return fail(R::muted);
            level=p?p->setting.level:L::inherit;
            if(level==L::inherit&&cat)level=cat->setting.level;
            if(level==L::inherit)level=gs.level;if(level==L::inherit){if(!i.guild_default_known)return fail(R::unknown);level=i.guild_default;}}
        se=sr=false;
    }
    if(level==L::inherit||static_cast<u8>(level)>static_cast<u8>(L::inherit))return fail(R::unknown);
    if(level==L::none)return fail(R::nothing);
    return {R::eligible,level,se,sr};
}
MessageNotificationReason message_notification_admit(const MessageNotificationPolicyInput&i,u32 required_gates) noexcept {
    using R=MessageNotificationReason;using L=MessageNotificationLevel;
    if(!i.live)return R::not_live;if(i.duplicate)return R::duplicate;
    if(i.message_flags&4096)return R::silent;
    if(!i.ready||!i.settings||!i.identities_known||!i.self_id||!i.author_id||!i.type_known)return R::unknown;
    if(i.author_id==i.self_id&&!i.self_mentionable_system)return R::self;
    if((i.known_gates&required_gates)!=required_gates)return R::unknown;
    if(i.gates&required_gates)return R::special;
    if(i.channel_type==3&&i.message_type==2)return R::special;
    const auto global=message_notification_effective_global(i.global,i.now_ms,i.ready&&i.live);
    if((global.fields&~15u)||!(global.fields&notification_status)||!(global.fields&notification_quiet)||
        static_cast<u8>(global.status)>static_cast<u8>(MessageNotificationStatus::offline))return R::unknown;
    if(global.status==MessageNotificationStatus::unknown)return R::unknown;
    if(global.status==MessageNotificationStatus::dnd)return R::dnd;
    if(global.quiet)return R::quiet;
    const auto scope=message_notification_scope_resolve(i);
    if(scope.reason!=R::eligible)return scope.reason;
    if(i.focused&&(i.selected||i.sidebar_selected))return R::selected;
    const bool vocal=i.channel_type==2||i.channel_type==13;
    if(scope.level==L::all&&(!vocal||(i.current_voice_channel_known&&i.current_voice_channel)))return R::eligible;
    bool mentioned=i.direct_mention||(i.everyone_mention&&!scope.suppress_everyone);
    if(!mentioned&&!scope.suppress_roles&&i.mention_roles.count){if(!i.roles_known||(!i.mention_roles.data&&i.mention_roles.count)||(!i.self_roles.data&&i.self_roles.count))return R::unknown;
        for(u32 m=0;m<i.mention_roles.count;++m)for(u32 r=0;r<i.self_roles.count;++r)if(i.mention_roles[m]==i.self_roles[r])mentioned=true;}
    if(mentioned)return R::eligible;
    if(scope.level==L::all&&vocal&&!i.current_voice_channel_known)return R::unknown;
    return R::not_mentioned;
}
MessageNotificationReason message_notification_admit(const MessageNotificationPolicyInput&i) noexcept {
    return message_notification_admit(i,i.required_gates);
}
MessageNotificationDecision message_notification_decide(const MessageNotificationPolicyInput&i) noexcept {
    const auto reason=message_notification_admit(i);
    return reason==MessageNotificationReason::eligible?MessageNotificationDecision{reason,i.desktop_enabled,i.sound_enabled}:reject(reason);
}
MessageReadBadge message_read_badge(const MessageNotificationPolicyInput& i,
                                    bool mention_on_all_messages) noexcept {
    using B=MessageReadBadge;using L=MessageNotificationLevel;
    constexpr u32 relationships=notification_gate_blocked|notification_gate_ignored;
    if(!i.settings||!i.channel_id||!i.self_id||!i.author_id||
       (i.author_id==i.self_id&&i.message_type!=46)||
       (i.known_gates&relationships)!=relationships||(i.gates&relationships))return B::none;
    const auto* guild=message_notifications_guild(*i.settings,i.guild_id);
    const bool settings_known=(guild&&!guild->incomplete)||(!guild&&i.settings->complete);
    const auto* channel=message_notifications_override(*i.settings,i.guild_id,i.channel_id);
    const auto* parent=message_notifications_override(*i.settings,i.guild_id,i.parent_id);
    const auto* category=message_notifications_override(*i.settings,i.guild_id,i.category_id);
    const auto* thread=message_notifications_thread(*i.settings,i.channel_id);
    bool mentioned=i.direct_mention;
    if(settings_known){
        const bool suppress_everyone=guild&&guild->setting.suppress_everyone;
        const bool suppress_roles=guild&&guild->setting.suppress_roles;
        mentioned|=i.everyone_mention&&!suppress_everyone;
        if(!suppress_roles&&i.roles_known&&slice_valid(i.mention_roles)&&slice_valid(i.self_roles))
            for(u32 m=0;m<i.mention_roles.count;++m)
                for(u32 r=0;r<i.self_roles.count;++r)
                    mentioned|=i.mention_roles[m]==i.self_roles[r];
    }
    const bool scope_muted=(guild&&message_notification_muted(guild->setting,i.now_ms))||
        (parent&&message_notification_muted(parent->setting,i.now_ms))||
        (category&&message_notification_muted(category->setting,i.now_ms));
    const bool muted=scope_muted||(channel&&message_notification_muted(channel->setting,i.now_ms));
    const bool private_channel=i.channel_type==1||i.channel_type==3||i.channel_type==18;
    if(private_channel){
        if(i.message_type==2)return B::none;
        if((settings_known&&!muted)||mentioned)return B::mention;
    }else if(mentioned)return B::mention;
    if(!mention_on_all_messages||i.channel_type==2||i.channel_type==13)return B::none;
    const bool is_thread=i.channel_type==10||i.channel_type==11||i.channel_type==12||i.channel_type==19;
    if(is_thread){
        // Explicit thread flags resolve before ancestor mute/default settings.
        // An absent self-thread setting is not inherited notification consent.
        if(!thread||thread->guild_id!=i.guild_id||!(thread->setting.fields&notification_flags))return B::none;
        if(thread->setting.flags&2u)return B::low_importance;
        if(thread->setting.flags&(4u|8u))return B::none;
        if(!i.parent_id||scope_muted)return B::none;
    }else if(muted)return B::none;
    if(!settings_known)return B::none;
    L level=!is_thread&&channel?channel->setting.level:L::inherit;
    if(level==L::inherit&&parent)level=parent->setting.level;
    if(level==L::inherit&&category)level=category->setting.level;
    if(level==L::inherit&&guild)level=guild->setting.level;
    if(level==L::inherit&&i.guild_default_known)level=i.guild_default;
    return level==L::all?B::low_importance:B::none;
}
} // namespace maicord

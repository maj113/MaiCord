#include "maicord/protocol/notification_settings.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/byte_range.hpp"
#include "proto_wire.hpp"
#include "json_field_lists.hpp"
namespace maicord {
namespace {
using E=NotificationSettingsDecodeError;
struct Parse {
    const JsonIndexedDocument* doc;
    MessageNotificationSetting setting;
    u64 guild,id,version;
    JsonValue overrides;
    u32 seen;
    bool guild_seen,id_seen,overrides_seen,version_known;
    E error;
};
bool mark(Parse&p,u32 bit) noexcept {if(p.seen&bit){p.error=E::duplicate;return false;}p.seen|=bit;return true;}
bool mute_member(void*opaque,JsonString name,JsonValue v) noexcept {
    auto&p=*static_cast<Parse*>(opaque);
    if(!json_string_equals_ascii(name,utf8_literal("end_time")))return true;
    if(!mark(p,1u<<20))return false;p.setting.fields|=notification_mute_end;
    if(v.kind==JsonValueKind::null_value){p.setting.mute_end_ms=0;return true;}
    if(!json_value_to_timestamp(v,&p.setting.mute_end_ms)){p.error=E::field;return false;}return true;
}
bool member(void*opaque,JsonString name,JsonValue v) noexcept {
    auto&p=*static_cast<Parse*>(opaque);u32 bit=0;bool ok=true;bool*dest=nullptr;
    switch(protocol_detail::member_index<"guild_id","channel_id","id","channel_overrides","version","message_notifications","flags","mute_config","muted","suppress_everyone","suppress_roles","mobile_push","hide_muted_channels">(name)){
    case 0:bit=1u<<8;if(!mark(p,bit))return false;p.guild_seen=true;
        ok=v.kind==JsonValueKind::null_value?(p.guild=0,true):json_value_to_snowflake(v,&p.guild);break;
    case 1:case 2:bit=1u<<9;if(!mark(p,bit))return false;p.id_seen=true;ok=json_value_to_snowflake(v,&p.id)&&p.id;break;
    case 3:bit=1u<<10;if(!mark(p,bit))return false;p.overrides_seen=true;p.overrides=v;ok=v.kind==JsonValueKind::array;break;
    case 4:bit=1u<<12;if(!mark(p,bit))return false;ok=json_value_to_u64(v,&p.version);p.version_known=ok;break;
    case 5:bit=notification_level;if(!mark(p,bit))return false;
        if(v.kind==JsonValueKind::null_value)p.setting.level=MessageNotificationLevel::inherit;
        else {u64 n=0;ok=json_value_to_u64(v,&n)&&n<=3;p.setting.level=static_cast<MessageNotificationLevel>(n);}p.setting.fields|=static_cast<u16>(bit);break;
    case 6:{bit=notification_flags;if(!mark(p,bit))return false;u64 n=0;ok=json_value_to_u64(v,&n)&&n<=~u32{0};p.setting.flags=static_cast<u32>(n);p.setting.fields|=static_cast<u16>(bit);break;}
    case 7:bit=1u<<11;if(!mark(p,bit))return false;
        if(v.kind==JsonValueKind::null_value){p.setting.fields|=notification_mute_end;p.setting.mute_end_ms=0;}
        else if(v.kind==JsonValueKind::object){const auto r=json_visit_indexed_object(*p.doc,v,64,mute_member,&p);ok=r.ok();}
        else ok=false;break;
    case 8:bit=notification_muted;dest=&p.setting.muted;break;
    case 9:bit=notification_suppress_everyone;dest=&p.setting.suppress_everyone;break;
    case 10:bit=notification_suppress_roles;dest=&p.setting.suppress_roles;break;
    case 11:bit=notification_mobile_push;dest=&p.setting.mobile_push;break;
    case 12:bit=notification_hide_muted;dest=&p.setting.hide_muted_channels;break;
    default:break;}
    if(dest){if(!mark(p,bit))return false;ok=json_value_to_bool(v,dest);p.setting.fields|=static_cast<u16>(bit);}
    if(!ok&&p.error==E::none)p.error=E::field;return ok;
}
E parse(const JsonIndexedDocument&d,JsonValue v,Parse*out) noexcept {
    if(v.kind!=JsonValueKind::object)return E::field;Parse p{};p.doc=&d;
    const auto r=json_visit_indexed_object(d,v,64,member,&p);
    if(!r.ok())return p.error==E::none?E::json:p.error;*out=p;return E::none;
}
struct Batch {const JsonIndexedDocument*doc;Slice<MessageNotificationGuildSetting>g;
    Slice<MessageNotificationOverride>o;NotificationSettingsPacket packet;u64 guild;E error;};
bool override_item(void*opaque,u32,JsonValue v) noexcept {
    auto&b=*static_cast<Batch*>(opaque);Parse p{};b.error=parse(*b.doc,v,&p);if(b.error!=E::none)return false;
    if(!p.id_seen||p.overrides_seen||(p.guild_seen&&p.guild!=b.guild)){b.error=E::field;return false;}
    for(u32 i=0;i<b.packet.override_count;++i)if(b.o[i].guild_id==b.guild&&b.o[i].channel_id==p.id){b.error=E::duplicate;return false;}
    if(b.packet.override_count==b.o.count){b.error=E::capacity;return false;}
    b.o[b.packet.override_count++]={b.guild,p.id,p.setting};return true;
}
bool guild_item(Batch&b,JsonValue v) noexcept {
    Parse p{};b.error=parse(*b.doc,v,&p);if(b.error!=E::none)return false;
    if(!p.guild_seen){b.error=E::field;return false;}
    if(!b.g.count){b.error=E::capacity;return false;}
    b.g[0]={p.guild,p.setting};b.guild=p.guild;
    if(p.version_known){b.packet.version=p.version;b.packet.version_known=true;}
    if(p.overrides_seen&&!json_visit_indexed_array(*b.doc,p.overrides,64,override_item,&b).ok()){
        if(b.error==E::none)b.error=E::json;return false;}return true;
}
// Raw or lazily decoded base64 bytes. A base64 source is validated by the
// canonical codec first, so quartet lookup cannot encounter invalid symbols.
using namespace protocol_detail;
using Wire=ProtoSource;
bool string_status(const Wire&w,u32 begin,u32 end,MessageNotificationStatus&status) noexcept {
    char s[9]{};const u32 count=end-begin;if(count>9){status=MessageNotificationStatus::unknown;return true;}for(u32 p=0;p<count;++p)s[p]=static_cast<char>(w.at(begin+p));
    auto eq=[&](const char*t,u32 n) noexcept {return count==n&&bytes_equal(s,t,n);};
    status=eq("online",6)?MessageNotificationStatus::online:eq("idle",4)?MessageNotificationStatus::idle:
        eq("dnd",3)?MessageNotificationStatus::dnd:eq("invisible",9)?MessageNotificationStatus::invisible:
        eq("offline",7)?MessageNotificationStatus::offline:MessageNotificationStatus::unknown;return true;
}
bool proto(const Wire&w,u32 begin,u32 end,u32 context,MessageNotificationGlobal&g,u32 depth=0) noexcept {
    if(depth>16)return false;u32 pos=begin;
    // Server partial updates replace each present top-level group. Project
    // its protobuf defaults even when nested wire fields are omitted. Repeated
    // occurrences within one wire payload still merge into this decoded group.
    if(context==1&&!(g.fields&notification_quiet)){
        g.quiet=false;g.focus_expires_ms=0;g.fields|=notification_quiet|notification_focus_expiry;
    }
    if(context==2&&!(g.fields&notification_status)){
        g.status=MessageNotificationStatus::unknown;g.status_expires_ms=0;g.fields|=notification_status|notification_status_expiry;
    }
    // Empty present wrapper is a present protobuf default, not missing.
    if(context==3&&!(g.fields&notification_quiet)){g.quiet=false;g.fields|=notification_quiet;}
    if(context==4&&!(g.fields&notification_status)){g.status=MessageNotificationStatus::unknown;g.fields|=notification_status;}
    while(pos<end){u32 f=0,t=0,child=0;if(!proto_tag(w,pos,end,f,t))return false;
        if(context==0&&f==7)child=1;else if(context==0&&f==11)child=2;
        else if(context==1&&f==5)child=3;else if(context==2&&f==1)child=4;
        if(child){u32 stop=0;if(t!=2||!proto_length(w,pos,end,stop)||!proto(w,pos,stop,child,g,depth+1))return false;pos=stop;}
        else if((context==1&&f==6)||(context==2&&f==4)){if(t!=1||end-pos<8)return false;const u64 n=proto_fixed64(w,pos);pos+=8;
            if(context==1){g.focus_expires_ms=n;g.fields|=notification_focus_expiry;}else{g.status_expires_ms=n;g.fields|=notification_status_expiry;}}
        else if(context==3&&f==1){u64 n=0;if(t!=0||!proto_varint(w,pos,end,n))return false;g.quiet=n!=0;}
        else if(context==4&&f==1){u32 stop=0;if(t!=2||!proto_length(w,pos,end,stop)||!string_status(w,pos,stop,g.status))return false;pos=stop;}
        else if(!proto_skip(w,pos,end,t,f,depth))return false;
    }return true;
}
}
NotificationSettingsDecodeError decode_notification_settings(const JsonIndexedDocument&d,JsonValue v,
    Slice<MessageNotificationGuildSetting>g,Slice<MessageNotificationOverride>o,NotificationSettingsPacket*out) noexcept {
    if(d.input.bytes>kNotificationSettingsMaximumBytes||g.count>~u32{0}/sizeof(*g.data)||o.count>~u32{0}/sizeof(*o.data)||
       d.structurals.count>~u32{0}/sizeof(u32))return E::input;
    const ConstSlice<u8> gb{reinterpret_cast<const u8*>(g.data),g.count*static_cast<u32>(sizeof(*g.data))};
    const ConstSlice<u8> ob{reinterpret_cast<const u8*>(o.data),o.count*static_cast<u32>(sizeof(*o.data))};
    const ConstSlice<u8> source{reinterpret_cast<const u8*>(d.input.data),d.input.bytes};
    const ConstSlice<u8> index{reinterpret_cast<const u8*>(d.structurals.data),d.structurals.count*static_cast<u32>(sizeof(u32))};
    const ConstSlice<u8> header{reinterpret_cast<const u8*>(out),sizeof(*out)};
    if(!byte_range_valid(gb)||!byte_range_valid(ob)||!byte_range_valid(source)||!byte_range_valid(index)||byte_ranges_overlap(gb,ob)||
       byte_ranges_overlap(gb,source)||byte_ranges_overlap(ob,source)||byte_ranges_overlap(header,gb)||
       byte_ranges_overlap(header,ob)||byte_ranges_overlap(header,source)||byte_ranges_overlap(gb,index)||
       byte_ranges_overlap(ob,index)||byte_ranges_overlap(header,index))return E::input;
    Batch b{};b.doc=&d;b.g=g;b.o=o;
    if(!guild_item(b,v))return b.error;
    *out=b.packet;return E::none;
}
NotificationSettingsDecodeError decode_notification_thread(const JsonIndexedDocument&d,JsonValue v,u64 fallback,MessageNotificationThreadSetting*out,u64 fallback_thread) noexcept {
    Parse p{};const E e=parse(d,v,&p);if(e!=E::none)return e;
    if(!(p.id_seen?p.id:fallback_thread)||!(p.guild_seen?p.guild:fallback)||p.overrides_seen)return E::field;
    *out={p.guild_seen?p.guild:fallback,p.id_seen?p.id:fallback_thread,p.setting};return E::none;
}
NotificationSettingsDecodeError decode_notification_global_base64(Utf8View b,MessageNotificationGlobal*out) noexcept {
    if(b.bytes>kNotificationSettingsMaximumBytes)return E::input;
    const auto r=base64_decode(b,{});if(!r.ok()&&r.error!=Base64Error::output_too_small)return E::base64;
    const Wire w{{},b,r.bytes};MessageNotificationGlobal g{};if(!proto(w,0,w.size,0,g))return E::protobuf;*out=g;return E::none;
}
} // namespace maicord

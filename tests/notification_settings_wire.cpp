#include "maicord/protocol/notification_settings.hpp"
#include "maicord/protocol/base64.hpp"
namespace maicord {
namespace {
NotificationSettingsDecodeError decode(Utf8View text,Slice<MessageNotificationGuildSetting>g,
    Slice<MessageNotificationOverride>o,NotificationSettingsPacket*p) noexcept {
    u32 scratch[512]{};const auto scan=json_scan_structurals_avx1(text,{scratch,512});
    if(!scan.ok())return NotificationSettingsDecodeError::json;
    const JsonIndexedDocument doc{text,{scratch,scan.count}};
    return decode_notification_settings(doc,{text,0,JsonValueKind::object},g,o,p);
}
// The decoder reads base64 settings; encode raw protobuf fixtures first.
NotificationSettingsDecodeError decode_global(ConstSlice<u8> bytes,MessageNotificationGlobal*out) noexcept {
    char encoded[64]{};const auto base=base64_encode(bytes,{encoded,64});
    return base.ok()?decode_notification_global_base64({encoded,base.bytes},out):NotificationSettingsDecodeError::input;
}
}
void test_notification_settings_wire(void(*check)(bool,const char*)noexcept) noexcept {
    using E=NotificationSettingsDecodeError;MessageNotificationGuildSetting g[3]{};MessageNotificationOverride o[4]{};NotificationSettingsPacket packet{};
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"muted\":true,\"mute_config\":{\"end_time\":\"1970-01-01T00:00:01.000Z\"},\"channel_overrides\":[{\"channel_id\":\"20\",\"message_notifications\":1,\"mute_config\":null}]}"),{g,3},{o,4},&packet)==E::none&&packet.override_count==1&&
          g[0].setting.mute_end_ms==1000&&(o[0].setting.fields&notification_mute_end)&&o[0].setting.mute_end_ms==0,"mute timestamp and explicit null clear");
    packet.override_count=99;
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"muted\":true,\"muted\":false}"),{g,3},{o,4},&packet)==E::duplicate&&packet.override_count==99,"duplicate field rejects without publishing header");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"channel_overrides\":[{\"channel_id\":\"20\"},{\"channel_id\":\"20\"}]}"),{g,3},{o,4},&packet)==E::duplicate,"duplicate override identity rejected");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"message_notifications\":4}"),{g,3},{o,4},&packet)==E::field,"invalid setting enum rejected");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"channel_overrides\":[{\"channel_id\":\"20\"}]}"),{g,3},{nullptr,0},&packet)==E::capacity,"bounded override staging capacity");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"message_notifications\":null,\"mute_config\":{}}"),{g,3},{o,4},&packet)==E::none&&g[0].setting.level==MessageNotificationLevel::inherit&&!(g[0].setting.fields&notification_mute_end),"null setting inherits and absent mute end preserves");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"version\":17}"),{g,3},{o,4},&packet)==E::none&&packet.version_known&&packet.version==17,"full entry carries its update version");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"hide_muted_channels\":true,\"flags\":16384,\"channel_overrides\":[{\"channel_id\":\"20\",\"flags\":4096}]}"),{g,3},{o,4},&packet)==E::none&&
          (g[0].setting.fields&notification_hide_muted)&&g[0].setting.hide_muted_channels&&g[0].setting.flags==16384&&
          (o[0].setting.fields&notification_flags)&&o[0].setting.flags==4096,
          "USER_GUILD_SETTINGS_UPDATE carries hide_muted_channels, guild opt-in flags and channel opt-in flags");
    check(decode(utf8_literal("{\"guild_id\":\"10\",\"hide_muted_channels\":true,\"hide_muted_channels\":false}"),{g,3},{o,4},&packet)==E::duplicate,
          "duplicate hide_muted_channels rejected");
    // root notifications(7): quiet wrapper(5)->bool; expiry(6) fixed64.
    // root status(11): status wrapper(1)->StringValue dnd.
    const u8 bytes[]={0x3a,13,0x2a,2,8,1,0x31,0xe8,3,0,0,0,0,0,0,0x5a,7,0x0a,5,0x0a,3,'d','n','d'};
    MessageNotificationGlobal global{};check(decode_global({bytes,sizeof(bytes)},&global)==E::none&&global.quiet&&global.focus_expires_ms==1000&&global.status==MessageNotificationStatus::dnd,"minimal nested protobuf subset");
    const u8 empty_wrapper[]={0x3a,2,0x2a,0};check(decode_global({empty_wrapper,4},&global)==E::none&&(global.fields&notification_quiet)&&!global.quiet,"empty present wrapper carries protobuf default false");
    const u8 empty_notifications[]={0x3a,0};
    check(decode_global({empty_notifications,2},&global)==E::none&&global.fields==(notification_quiet|notification_focus_expiry)&&!global.quiet&&!global.focus_expires_ms,
        "present empty notifications group projects both replacement defaults");
    MessageNotificationGlobal retained{100,200,15,true,MessageNotificationStatus::dnd};
    message_notifications_apply_global(&retained,global,true);
    check(!retained.quiet&&!retained.focus_expires_ms&&retained.status==MessageNotificationStatus::dnd&&retained.status_expires_ms==200,
        "partial notifications group resets its members and preserves omitted status group");
    const u8 empty_status[]={0x5a,0};
    check(decode_global({empty_status,2},&global)==E::none&&global.fields==(notification_status|notification_status_expiry)&&global.status==MessageNotificationStatus::unknown&&!global.status_expires_ms,
        "present empty status group projects unknown status and zero expiry defaults");
    retained={100,200,15,true,MessageNotificationStatus::dnd};message_notifications_apply_global(&retained,global,true);
    check(retained.quiet&&retained.focus_expires_ms==100&&retained.status==MessageNotificationStatus::unknown&&!retained.status_expires_ms,
        "partial status group replaces its members and preserves omitted notifications group");
    const u8 status_only[]={0x5a,7,0x0a,5,0x0a,3,'d','n','d'};
    check(decode_global({status_only,sizeof(status_only)},&global)==E::none&&global.fields==(notification_status|notification_status_expiry)&&global.status==MessageNotificationStatus::dnd&&!global.status_expires_ms,
        "status group with omitted fixed64 expiry explicitly clears prior finite expiry");
    retained={100,200,15,true,MessageNotificationStatus::dnd};
    check(decode_global({},&global)==E::none&&!global.fields,"entire empty partial proto has no present groups");
    message_notifications_apply_global(&retained,global,true);
    check(retained.quiet&&retained.focus_expires_ms==100&&retained.status==MessageNotificationStatus::dnd&&retained.status_expires_ms==200,
        "entire empty partial proto preserves both existing groups");
    const u8 repeated_wrapper[]={0x3a,6,0x2a,2,8,1,0x2a,0};check(decode_global({repeated_wrapper,8},&global)==E::none&&global.quiet,"repeated protobuf message wrapper merges rather than clearing");
    const u8 unknown[]={0x80,1,7,0x3a,4,0x2a,2,8,1};check(decode_global({unknown,sizeof(unknown)},&global)==E::none&&global.quiet,"unknown varint field skipped");
    const u8 malformed[]={0x3a,10,0x2a,2,8,1};global.quiet=false;global.fields=99;
    check(decode_global({malformed,sizeof(malformed)},&global)==E::protobuf&&global.fields==99,"truncated proto leaves output unchanged");
    const u8 overflow[]={0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,2};check(decode_global({overflow,10},&global)==E::protobuf,"overflow varint rejected");
    check(decode_notification_global_base64(utf8_literal("AB=="),&global)==E::base64,"noncanonical base64 rejected before parsing");
}
} // namespace maicord

#include "maicord/app/user_store_dispatch.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
bool user_store_apply_gateway_snapshots(UserStore* store,
    const GatewayClientEvent& input_event,ConstSlice<u8> input_payload) noexcept {
    if(input_event.kind!=GatewayClientEventKind::dispatch||
        input_event.dispatch_kind!=GatewayClientDispatchKind::user_snapshot)return false;
    if(input_event.dispatch_payload_bytes!=input_payload.count||(!input_payload.data&&input_payload.count))return false;
    GatewayClientEvent normalized_event;
    ConstSlice<u8> payload=input_payload;
    if(input_event.notification_payload_bytes&&
        !gateway_client_notification_trim(input_event,input_payload,&normalized_event,&payload))return false;
    const GatewayClientEvent& event=input_event.notification_payload_bytes?normalized_event:input_event;
    return user_store_apply_gateway_snapshots(store,payload,event.state.record_count);
}
bool user_store_apply_gateway_snapshots(UserStore* store, ConstSlice<u8> payload, u32 count) noexcept {
    if ((!payload.data && payload.count) || count > payload.count / sizeof(GatewayClientUserMutation)) return false;
    u32 cursor = 0;
    for (u32 i = 0; i < count; ++i) {
        GatewayClientUserMutation user{};
        Utf8View name{}, avatar{};
        if (!gateway_client_user_record(payload, &cursor, &user, &name, &avatar) || ((user.flags || user.appearance_bytes) && !store)) return false;
    }
    if (cursor != payload.count) return false;
    cursor = 0;
    for (u32 i = 0; i < count; ++i) {
        GatewayClientUserMutation user{};
        Utf8View name{}, avatar{}, username{};
        ConstSlice<u8> appearance{};
        if (!gateway_client_user_record(payload, &cursor, &user, &name, &avatar, &username, &appearance)) return false;
        if (appearance.count) {
            UserAppearance decoded{};
            if (!user_appearance_read(appearance, user.id, 0, &decoded) || !user_appearance_upsert(&store->appearance, decoded)) return false;
            ++store->revision;
        }
        if (!user.flags) continue;
        char fallback[20];
        if (name.bytes) {
            if (!user_store_upsert(store, user.id, name, username).ok()) return false;
        } else if (!user_store_find(*store, user.id)) {
            // An avatar-only snapshot still needs an identity-bearing cache entry.
            if (!user_store_upsert(store, user.id, {fallback, decimal_write(fallback, user.id)}).ok()) return false;
        }
        if ((user.flags & 2) && !user_store_set_avatar(store, user.id, avatar)) return false;
    }
    return true;
}
}

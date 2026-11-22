#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/core/memory.hpp"

// Readers for the notification records the account module publishes; the
// client drains these from the mailbox.
namespace maicord {

bool gateway_client_notification_entry(const GatewayClientEvent& e, ConstSlice<u8> p,
                                       MessageNotificationGuildSetting* out) noexcept {
    // The worker writes the setting, then override_count overrides, and decodes level and fields within range.
    if (e.dispatch_payload_bytes != p.count || !p.data)
        return false;
    copy_bytes(out, p.data, sizeof(*out));
    return true;
}
bool gateway_client_notification_override_at(ConstSlice<u8> p, u32 at,
                                             MessageNotificationOverride* out) noexcept {
    // The caller read the entry first and asks for at < override_count. Only the fields can still be out of
    // range: an override may carry hide_muted_channels.
    MessageNotificationOverride row{};
    copy_bytes(&row, p.data + sizeof(MessageNotificationGuildSetting) + at * sizeof(row), sizeof(row));
    if (row.setting.fields & ~kMessageNotificationChannelFields)
        return false;
    *out = row;
    return true;
}
bool gateway_client_notification_sidecar(ConstSlice<u8> p, GatewayClientNotificationSidecar* out,
                                         ConstSlice<u8>* records) noexcept {
    if (!p.data || p.count < sizeof(*out))
        return false;
    GatewayClientNotificationSidecar h{};
    copy_bytes(&h, p.data, sizeof(h));
    // The worker sets only known flags, and each gate bit with its known bit.
    if (p.count != sizeof(h) + h.thread_count * sizeof(GatewayClientNotificationThreadRecord))
        return false;
    *out = h;
    *records = {p.data + sizeof(h), p.count - static_cast<u32>(sizeof(h))};
    return true;
}
bool gateway_client_notification_thread_at(ConstSlice<u8> p, u32 at,
                                           GatewayClientNotificationThreadRecord* out) noexcept {
    // p holds the rows of a sidecar read above, and at < its thread_count. The worker writes only the two
    // flags, a zero reserved byte, and each gate bit with its known bit.
    GatewayClientNotificationThreadRecord row{};
    copy_bytes(&row, p.data + at * sizeof(row), sizeof(row));
    if (!row.thread.guild_id || !row.thread.thread_id ||
        row.thread.setting.fields & ~kMessageNotificationChannelFields)
        return false;
    *out = row;
    return true;
}
bool gateway_client_notification_trim(const GatewayClientEvent& e, ConstSlice<u8> p,
                                      GatewayClientEvent* out, ConstSlice<u8>* normal) noexcept {
    if (e.dispatch_payload_bytes != p.count || (!p.data && p.count))
        return false;
    const bool applicable = e.dispatch_kind == GatewayClientDispatchKind::guild_create ||
                            e.dispatch_kind == GatewayClientDispatchKind::guild_update ||
                            e.dispatch_kind == GatewayClientDispatchKind::channel_create ||
                            e.dispatch_kind == GatewayClientDispatchKind::channel_update ||
                            e.dispatch_kind == GatewayClientDispatchKind::thread_members_update ||
                            e.dispatch_kind == GatewayClientDispatchKind::thread_self_update ||
                            e.dispatch_kind == GatewayClientDispatchKind::thread_list_sync ||
                            e.dispatch_kind == GatewayClientDispatchKind::unknown ||
                            e.dispatch_kind == GatewayClientDispatchKind::user_snapshot;
    const u32 raw_tail = applicable ? e.notification_payload_bytes : 0;
    const u32 tail = raw_tail == kGatewayNotificationMetadataIncomplete ? 0 : raw_tail;
    if (tail > p.count)
        return false;
    if (tail) {
        GatewayClientNotificationSidecar h{};
        ConstSlice<u8> rows{};
        if (!gateway_client_notification_sidecar({p.data + p.count - tail, tail}, &h, &rows))
            return false;
        for (u32 n = 0; n < h.thread_count; ++n) {
            GatewayClientNotificationThreadRecord row{};
            if (!gateway_client_notification_thread_at(rows, n, &row))
                return false;
        }
    }
    *out = e;
    *normal = {p.data, p.count - tail};
    out->dispatch_payload_bytes = normal->count;
    if (applicable)
        out->notification_payload_bytes = 0;
    return true;
}

} // namespace maicord

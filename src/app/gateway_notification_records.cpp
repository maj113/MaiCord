#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"

// Readers for the notification records the account module publishes. The
// client drains these from the mailbox; keeping them apart from the Gateway
// JSON router keeps that router (and its decoders) out of the client image.
namespace maicord {

bool gateway_client_notification_entry(const GatewayClientEvent& e, ConstSlice<u8> p,
                                       MessageNotificationGuildSetting* out) noexcept {
    if (!out || e.dispatch_kind != GatewayClientDispatchKind::notification_settings_entry ||
        e.dispatch_payload_bytes != p.count || !p.data || p.count < sizeof(*out) ||
        e.notification.override_count > (p.count - sizeof(*out)) / sizeof(MessageNotificationOverride) ||
        p.count != sizeof(*out) + e.notification.override_count * sizeof(MessageNotificationOverride))
        return false;
    MessageNotificationGuildSetting row{};
    copy_bytes(&row, p.data, sizeof(row));
    if (row.guild_id != e.notification.guild_id || row.setting.fields & ~kMessageNotificationGuildFields ||
        static_cast<u8>(row.setting.level) > 3 || row.incomplete)
        return false;
    *out = row;
    return true;
}
bool gateway_client_notification_override_at(const GatewayClientEvent& e, ConstSlice<u8> p, u32 at,
                                             MessageNotificationOverride* out) noexcept {
    MessageNotificationGuildSetting guild{};
    if (!out || at >= e.notification.override_count || !gateway_client_notification_entry(e, p, &guild))
        return false;
    MessageNotificationOverride row{};
    copy_bytes(&row, p.data + sizeof(guild) + at * sizeof(row), sizeof(row));
    if (!row.channel_id || row.guild_id != guild.guild_id || row.setting.fields & ~kMessageNotificationChannelFields ||
        static_cast<u8>(row.setting.level) > 3)
        return false;
    *out = row;
    return true;
}
bool gateway_client_notification_sidecar(ConstSlice<u8> p, GatewayClientNotificationSidecar* out,
                                         ConstSlice<u8>* records) noexcept {
    if (!out || !records || !p.data || p.count < sizeof(*out))
        return false;
    GatewayClientNotificationSidecar h{};
    copy_bytes(&h, p.data, sizeof(h));
    if (h.thread_count > (p.count - sizeof(h)) / sizeof(GatewayClientNotificationThreadRecord) ||
        p.count != sizeof(h) + h.thread_count * sizeof(GatewayClientNotificationThreadRecord) ||
        h.flags & ~(127u | gateway_notification_channel_age_baseline_complete) || h.gates & ~h.known_gates)
        return false;
    *out = h;
    *records = {p.data + sizeof(h), p.count - static_cast<u32>(sizeof(h))};
    return true;
}
bool gateway_client_notification_thread_at(ConstSlice<u8> p, u32 at,
                                           GatewayClientNotificationThreadRecord* out) noexcept {
    if (!out || !p.data || p.count % sizeof(*out) || at >= p.count / sizeof(*out))
        return false;
    GatewayClientNotificationThreadRecord row{};
    copy_bytes(&row, p.data + at * sizeof(row), sizeof(row));
    if (!row.thread.guild_id || !row.thread.thread_id ||
        row.flags & ~(gateway_notification_thread_remove | gateway_notification_channel_metadata) ||
        row.reserved || row.thread.setting.fields & ~kMessageNotificationChannelFields || row.gates & ~row.known_gates)
        return false;
    *out = row;
    return true;
}
bool gateway_client_notification_trim(const GatewayClientEvent& e, ConstSlice<u8> p,
                                      GatewayClientEvent* out, ConstSlice<u8>* normal) noexcept {
    if (!out || !normal || e.dispatch_payload_bytes != p.count || (!p.data && p.count))
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

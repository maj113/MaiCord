#include "dispatch_internal.hpp"
#include "maicord/protocol/user/notification_settings.hpp"

namespace maicord {
namespace dispatch_detail {

bool append_message_metadata(Slice<char> output, const GatewayMessageEvent& message,
                             ConstSlice<u64> role_ids, GatewayClientMessageMutation* mutation,
                             ConstSlice<u8>* payload) noexcept {
    const u64 role_bytes = static_cast<u64>(role_ids.count) * sizeof(u64);
    const u64 member_role_bytes = static_cast<u64>(message.member_roles.count) * sizeof(u64);
    const u64 user_bytes =
        static_cast<u64>(message.mention_users.count) * sizeof(GatewayClientNamedUserMutation);
    if (message.content.bytes > message.transient_bytes ||
        message.transient_bytes > output.count || role_bytes > output.count - message.transient_bytes ||
        member_role_bytes > output.count - message.transient_bytes - role_bytes ||
        user_bytes > output.count - message.transient_bytes - role_bytes - member_role_bytes) {
        return false;
    }
    u32 cursor = message.transient_bytes;
    // The mailbox stores role ids as little-endian u64s, which is what this target's arrays hold.
    copy_bytes(output.data + cursor, role_ids.data, static_cast<u32>(role_bytes));
    cursor += static_cast<u32>(role_bytes);
    copy_bytes(output.data + cursor, message.member_roles.data, static_cast<u32>(member_role_bytes));
    cursor += static_cast<u32>(member_role_bytes);
    for (u32 index = 0; index < message.mention_users.count; ++index) {
        const GatewayMessageNamedUser& user = message.mention_users[index];
        if (user.id == 0 || !user.name.data || user.name.bytes > kGatewayMessageUserNameMaximumBytes ||
            user.name.data < output.data || user.name.data > output.data + message.transient_bytes ||
            user.name.bytes > static_cast<u32>(output.data + message.transient_bytes - user.name.data)) {
            return false;
        }
        GatewayClientNamedUserMutation packed{};
        packed.id = user.id;
        packed.name_offset = static_cast<u32>(user.name.data - output.data);
        packed.name_bytes = static_cast<u16>(user.name.bytes);
        copy_bytes(output.data + cursor, &packed, static_cast<u32>(sizeof(packed)));
        cursor += sizeof(packed);
    }
    if (!append_message_rich_metadata(output, message, &cursor, &mutation->rich_payload_bytes)) {
        return false;
    }
    *payload = ConstSlice<u8>{reinterpret_cast<const u8*>(output.data), cursor};
    return true;
}

bool notification_fields(void* opaque, JsonString name, JsonValue v) noexcept {
    auto& f = *static_cast<NotificationFields*>(opaque);
    const u32 bit = protocol_detail::member_bit<"id", "guild_id", "user_id", "entries", "partial", "version", "proto",
        "settings", "threads", "member", "added_members", "removed_member_ids", "flags", "default_message_notifications",
        "lurking", "joined_at", "application_id", "is_message_request", "unavailable", "members", "type", "is_spam",
        "restricted_schedule", "rules", "channels", "nsfw", "nsfw_level", "owner_configured_content_level">(name);
    if (!bit)
        return true;
    if (f.seen & bit) {
        f.bad = true;
        return false;
    }
    f.seen |= bit;
    bool ok = true;
    u64 number = 0;
    switch (bit) {
    case 1:
        ok = json_value_to_snowflake(v, &f.id) && f.id;
        break;
    case 2:
        ok = v.kind == JsonValueKind::null_value ? (f.guild = 0, true)
                                                 : json_value_to_snowflake(v, &f.guild);
        break;
    case 4:
        ok = json_value_to_snowflake(v, &f.user);
        break;
    case 8:
        f.entries = v;
        break;
    case 16:
        ok = json_value_to_bool(v, &f.partial);
        break;
    case 32:
        ok = json_value_to_u64(v, &f.version);
        f.version_known = ok;
        break;
    case 64:
        f.proto = v;
        break;
    case 128:
        f.settings = v;
        break;
    case 256:
        f.threads = v;
        break;
    case 512:
        f.member = v;
        break;
    case 1024:
        f.added = v;
        break;
    case 2048:
        f.removed = v;
        break;
    case 4096:
        ok = json_value_to_u64(v, &number);
        f.flags = static_cast<u32>(number);
        break;
    case 8192:
        ok = json_value_to_u64(v, &number) && number <= 1;
        f.default_mentions = number == 1;
        break;
    case 16384: {
        bool yes = false;
        ok = json_value_to_bool(v, &yes);
        f.known_gates |= notification_gate_lurking;
        if (yes)
            f.gates |= notification_gate_lurking;
        break;
    }
    case 32768:
        if (v.kind == JsonValueKind::string)
            f.known_gates |= notification_gate_lurking;
        break;
    case 65536:
        f.known_gates |= notification_gate_managed;
        if (v.kind != JsonValueKind::null_value)
            f.gates |= notification_gate_managed;
        break;
    case 131072:
        ok = json_value_to_bool(v, &f.request);
        break;
    case 262144:
        ok = json_value_to_bool(v, &f.unavailable);
        break;
    default:
        break;
    }
    if (bit == 524288)
        f.members = v;
    if (bit == 1048576) {
        ok = json_value_to_u64(v, &number) && number <= ~u32{0};
        f.type = static_cast<u32>(number);
    }
    if (bit == 2097152)
        ok = json_value_to_bool(v, &f.spam);
    if (bit == 4194304)
        f.schedule = v;
    if (bit == 8388608)
        f.rules = v;
    if (bit == 16777216)
        f.channels = v;
    if (bit == 33554432)
        ok = v.kind == JsonValueKind::null_value || (json_value_to_bool(v, &f.nsfw));
    if (bit == 67108864) {
        ok = v.kind == JsonValueKind::null_value || (json_value_to_u64(v, &number) && number <= 3);
        f.nsfw_level = static_cast<u8>(number);
    }
    if (bit == 134217728) {
        ok = v.kind == JsonValueKind::null_value || (json_value_to_u64(v, &number) && number <= 3);
        f.owner_content_level = static_cast<u8>(number);
    }
    f.bad |= !ok;
    return ok;
}
bool notification_document(JsonValue source, Slice<u32> scratch, JsonIndexedDocument* doc,
                           NotificationFields* fields) noexcept {
    if (source.kind != JsonValueKind::object)
        return false;
    const auto scan = json_scan_structurals_avx1(source.source, scratch);
    if (!scan.ok())
        return false;
    *doc = {source.source, {scratch.data, scan.count}};
    return json_visit_value_object(source, doc->structurals, 64, notification_fields, fields).ok() &&
           !fields->bad;
}

} // namespace dispatch_detail
namespace {

struct RestrictionRules {
    const JsonIndexedDocument* doc;
    bool inactive = true;
};
struct RestrictionRule {
    bool seen = false, enabled = false;
};
bool restriction_enabled(void* opaque, JsonString name, JsonValue v) noexcept {
    if (!json_name_is(name, utf8_literal("enabled")))
        return true;
    auto& r = *static_cast<RestrictionRule*>(opaque);
    if (r.seen)
        return false;
    r.seen = true;
    return json_value_to_bool(v, &r.enabled);
}
bool restriction_rule(void* opaque, u32, JsonValue v) noexcept {
    auto& r = *static_cast<RestrictionRules*>(opaque);
    RestrictionRule rule{};
    if (v.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*r.doc, v, 64, restriction_enabled, &rule).ok())
        return false;
    if (!rule.seen || rule.enabled)
        r.inactive = false;
    return true;
}

} // namespace
namespace dispatch_detail {

bool restriction_inactive(const JsonIndexedDocument& doc, const NotificationFields& f, bool full) noexcept {
    if (!(f.seen & 4194304))
        return full;
    if (f.schedule.kind == JsonValueKind::null_value)
        return true;
    if (f.schedule.kind != JsonValueKind::object)
        return false;
    NotificationFields schedule{};
    if (!json_visit_indexed_object(doc, f.schedule, 64, notification_fields, &schedule).ok() ||
        schedule.rules.kind != JsonValueKind::array)
        return false;
    RestrictionRules rules{&doc, true};
    return json_visit_indexed_array(doc, schedule.rules, 64, restriction_rule, &rules).ok() &&
           rules.inactive;
}
void notification_incomplete(GatewayClientEvent* e, ConstSlice<u8>* payload, u64 guild) noexcept {
    e->dispatch_kind = GatewayClientDispatchKind::notification_incomplete;
    e->notification = {guild, 0, 0, 0, gateway_notification_incomplete, 0, 0};
    e->dispatch_payload_bytes = 0;
    reset_value(payload);
}

} // namespace dispatch_detail
namespace {

void notification_global_incomplete(GatewayClientEvent* e, ConstSlice<u8>* payload) noexcept {
    e->dispatch_kind = GatewayClientDispatchKind::notification_global;
    e->notification = {0, 0, 0, 0, gateway_notification_incomplete, 0, 0};
    e->dispatch_payload_bytes = 0;
    reset_value(payload);
}

} // namespace
namespace dispatch_detail {

bool notification_entry(JsonValue source, Slice<u32> scratch, Slice<char> storage, GatewayClientEvent* e,
                        ConstSlice<u8>* payload) noexcept {
    JsonIndexedDocument doc{};
    NotificationFields f{};
    if (!notification_document(source, scratch, &doc, &f)) {
        notification_incomplete(e, payload, f.guild);
        return true;
    }
    const usize address = reinterpret_cast<usize>(storage.data);
    const u32 padding = static_cast<u32>((8 - (address & 7)) & 7);
    if (padding > storage.count || sizeof(MessageNotificationGuildSetting) > storage.count - padding) {
        notification_incomplete(e, payload, f.guild);
        return true;
    }
    char* start = storage.data + padding;
    const u32 available = storage.count - padding;
    const u32 capacity = (available - static_cast<u32>(sizeof(MessageNotificationGuildSetting))) /
                         static_cast<u32>(sizeof(MessageNotificationOverride));
    auto* overrides =
        reinterpret_cast<MessageNotificationOverride*>(start + sizeof(MessageNotificationGuildSetting));
    MessageNotificationGuildSetting guild{};
    NotificationSettingsPacket packet{};
    const auto err = decode_notification_settings(doc, {doc.input, 0, JsonValueKind::object},
                                                  {&guild, 1}, {overrides, capacity}, &packet);
    if (err != NotificationSettingsDecodeError::none) {
        notification_incomplete(e, payload, f.guild);
        return true;
    }
    copy_bytes(start, &guild, sizeof(guild));
    const u32 bytes =
        static_cast<u32>(sizeof(guild)) + packet.override_count * static_cast<u32>(sizeof(*overrides));
    e->dispatch_kind = GatewayClientDispatchKind::notification_settings_entry;
    e->notification = {guild.guild_id,
                       0,
                       packet.version,
                       packet.override_count,
                       packet.version_known ? gateway_notification_version_known : 0u,
                       0,
                       0};
    e->dispatch_payload_bytes = bytes;
    *payload = {reinterpret_cast<const u8*>(start), bytes};
    return true;
}
bool notification_proto(JsonValue value, Slice<char> storage, MessageNotificationGlobal* g) noexcept {
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return false;
    Utf8View encoded = text.encoded;
    if (text.has_escapes) {
        const auto decoded = json_string_decode_utf8(text, storage);
        if (!decoded.ok())
            return false;
        encoded = {storage.data, decoded.bytes};
    }
    return decode_notification_global_base64(encoded, g) == NotificationSettingsDecodeError::none;
}
bool notification_route(const GatewayEnvelope& env, Slice<u32> scratch, Slice<char> storage,
                        GatewayClientEvent* e, ConstSlice<u8>* payload, u32 kind) noexcept {
    if (kind == 0)
        return notification_entry(env.data, scratch, storage, e, payload);
    const bool proto_update = kind == 1;
    JsonIndexedDocument doc{};
    NotificationFields f{};
    if (!notification_document(env.data, scratch, &doc, &f)) {
        notification_global_incomplete(e, payload);
        return true;
    }
    MessageNotificationGlobal global{};
    u32 flags = proto_update && f.partial ? gateway_notification_partial : 0u;
    u32 account_flags = 0;
    if (proto_update) {
        NotificationFields nested{};
        if (f.settings.kind != JsonValueKind::object ||
            !json_visit_indexed_object(doc, f.settings, 64, notification_fields, &nested).ok()) {
            notification_global_incomplete(e, payload);
            return true;
        }
        if ((nested.seen & 1048576) && nested.type != 1)
            return true;
        if (!(nested.seen & 1048576) || !notification_proto(nested.proto, storage, &global)) {
            notification_global_incomplete(e, payload);
            return true;
        }
    } else {
        if (!(f.seen & 4096)) {
            notification_global_incomplete(e, payload);
            return true;
        }
        flags = gateway_notification_partial | gateway_notification_account_flags_known;
        account_flags = f.flags;
    }
    if (storage.count < sizeof(global)) {
        notification_global_incomplete(e, payload);
        return true;
    }
    copy_bytes(storage.data, &global, sizeof(global));
    e->dispatch_kind = GatewayClientDispatchKind::notification_global;
    e->notification = {0, 0, 0, 0, flags, account_flags, 0};
    e->dispatch_payload_bytes = sizeof(global);
    *payload = {reinterpret_cast<const u8*>(storage.data), sizeof(global)};
    return true;
}

} // namespace dispatch_detail
namespace {

struct NotificationSide {
    const JsonIndexedDocument* doc;
    Slice<char> storage;
    GatewayClientNotificationSidecar header;
    u64 self;
    u32 cursor;
    bool bad;
};
bool notification_channel_meta(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if (f.nsfw && !(f.seen & 1)) {
        s.bad = true;
        return false;
    }
    if (!(f.seen & 1048576) && f.nsfw) {
        s.bad = true;
        return false;
    }
    if (!f.nsfw && (!(f.seen & 1048576) || f.type != 18))
        return true;
    GatewayClientNotificationThreadRecord row{};
    row.thread.guild_id = s.header.guild_id;
    row.thread.thread_id = f.id;
    row.flags = gateway_notification_channel_metadata;
    row.known_gates = notification_gate_age_access;
    if (f.nsfw)
        row.gates |= notification_gate_age_access;
    if (f.type == 18) {
        row.known_gates |= notification_gate_managed | notification_gate_request;
        row.gates |= f.gates & notification_gate_managed;
    }
    if (!f.id || sizeof(row) > s.storage.count - s.cursor) {
        s.bad = true;
        return false;
    }
    copy_bytes(s.storage.data + s.cursor, &row, sizeof(row));
    s.cursor += sizeof(row);
    ++s.header.thread_count;
    return true;
}
bool append_notification_thread(NotificationSide& s, JsonValue value, u64 fallback_thread,
                                bool remove = false) noexcept {
    GatewayClientNotificationThreadRecord row{};
    row.flags = remove ? gateway_notification_thread_remove : 0u;
    if (remove) {
        row.thread = {s.header.guild_id, fallback_thread, {}};
    } else if (decode_notification_thread(*s.doc, value, s.header.guild_id, &row.thread, fallback_thread) !=
               NotificationSettingsDecodeError::none) {
        s.bad = true;
        return false;
    }
    if (!remove) {
        NotificationFields member{};
        if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &member).ok() ||
            row.thread.guild_id != s.header.guild_id || !(row.thread.setting.fields & notification_flags)) {
            s.bad = true;
            return false;
        }
        if ((member.seen & 4) && member.user != s.self)
            return true;
        if (!(row.thread.setting.fields & notification_muted)) {
            row.thread.setting.fields |= notification_muted;
            row.thread.setting.muted = false;
        }
        if (!(row.thread.setting.fields & notification_mute_end)) {
            row.thread.setting.fields |= notification_mute_end;
            row.thread.setting.mute_end_ms = 0;
        }
    }
    if (sizeof(row) > s.storage.count - s.cursor) {
        s.bad = true;
        return false;
    }
    copy_bytes(s.storage.data + s.cursor, &row, sizeof(row));
    s.cursor += sizeof(row);
    ++s.header.thread_count;
    return true;
}
bool notification_joined(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if (!notification_channel_meta(opaque, 0, value))
        return false;
    if (!f.member.source.data || f.member.kind == JsonValueKind::null_value)
        return true;
    return append_notification_thread(s, f.member, f.id);
}
bool notification_added(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if (!(f.seen & 4)) {
        s.bad = true;
        return false;
    }
    if (f.user != s.self)
        return true;
    return append_notification_thread(s, value, s.header.channel_id);
}
bool notification_removed(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    u64 user = 0;
    if (!json_value_to_snowflake(value, &user)) {
        s.bad = true;
        return false;
    }
    return user != s.self || append_notification_thread(s, {}, s.header.channel_id, true);
}
bool notification_sync(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<NotificationSide*>(opaque);
    NotificationFields f{};
    if (!json_visit_indexed_object(*s.doc, value, 64, notification_fields, &f).ok()) {
        s.bad = true;
        return false;
    }
    if ((f.seen & 4) && f.user != s.self)
        return true;
    return append_notification_thread(s, value, f.id);
}

} // namespace

void gateway_client_append_notification_sidecar(const GatewayEnvelope& env, Slice<u32> scratch,
                                                Slice<char> storage, u64 self, GatewayClientEvent* e,
                                                ConstSlice<u8>* payload) noexcept {
    const u32 named = protocol_detail::skim_member_index<
        "GUILD_CREATE", "READY_GUILD", "GUILD_UPDATE", "THREAD_MEMBER_UPDATE", "THREAD_CREATE", "THREAD_UPDATE",
        "THREAD_MEMBERS_UPDATE", "THREAD_LIST_SYNC", "CHANNEL_CREATE", "CHANNEL_UPDATE", "USER_UPDATE">(env.event_name);
    if (named >= 11)
        return;
    const bool guild = named <= 2;
    const bool self_thread = named == 3;
    const bool thread = named >= 3 && named <= 5;
    const bool delta = named == 6;
    const bool sync = named == 7;
    const bool channel = thread || named == 8 || named == 9;
    const bool user = named == 10;
    JsonIndexedDocument doc{};
    NotificationFields f{};
    bool valid = notification_document(env.data, scratch, &doc, &f);
    if (user && (f.id != self || !(f.seen & 4194304)))
        return;
    const u32 base = payload->count;
    const auto* original = payload->data;
    // Existing payload bytes are copied unchanged. The appended descriptors are
    // read with memcpy accessors, so no alignment padding leaks into its body.
    if (base > storage.count || sizeof(GatewayClientNotificationSidecar) > storage.count - base) {
        e->notification_payload_bytes = kGatewayNotificationMetadataIncomplete;
        return;
    }
    if (base && original != reinterpret_cast<const u8*>(storage.data))
        copy_bytes(storage.data, original, base);
    NotificationSide side{};
    side.doc = &doc;
    side.storage = storage;
    side.self = self;
    side.cursor = base + sizeof(side.header);
    side.header.guild_id = guild ? f.id : f.guild;
    side.header.channel_id = guild || user ? 0 : f.id;
    side.header.known_gates = f.known_gates;
    side.header.gates = f.gates;
    if (user && f.id == self && (f.seen & 4194304)) {
        if (restriction_inactive(doc, f, false))
            side.header.known_gates |= notification_gate_restricted_hours;
        else
            side.header.flags |= gateway_notification_incomplete;
    }
    const bool full_channel = channel && (named == 4 || named == 8);
    const bool full_guild = guild && named <= 1;
    const bool sensitive_guild =
        f.nsfw_level == 1 || f.nsfw_level == 3 || f.owner_content_level == 1 || f.owner_content_level == 3;
    if (guild && sensitive_guild) {
        side.header.known_gates |= notification_gate_age_access;
        side.header.gates |= notification_gate_age_access;
    }
    if (channel && (full_channel || (f.seen & 33554432))) {
        side.header.known_gates |= notification_gate_age_access;
        if (f.nsfw)
            side.header.gates |= notification_gate_age_access;
        else
            side.header.gates &= ~notification_gate_age_access;
    }
    if (channel && (f.seen & 1048576)) {
        if ((f.type != 3 && f.type != 18) || (full_channel && !(f.seen & 65536))) {
            side.header.known_gates |= notification_gate_managed;
            side.header.gates &= ~notification_gate_managed;
        }
        if (f.type != 1 && f.type != 3) {
            side.header.known_gates |= notification_gate_request;
            side.header.gates &= ~notification_gate_request;
        }
    }
    if (channel && (full_channel || ((f.seen & 131072) && !f.request) || ((f.seen & 2097152) && f.spam) ||
                    ((f.seen & (131072 | 2097152)) == (131072 | 2097152)))) {
        side.header.known_gates |= notification_gate_request;
        if (f.request && !f.spam)
            side.header.gates |= notification_gate_request;
        else
            side.header.gates &= ~notification_gate_request;
    }
    if (guild && (f.seen & 8192)) {
        side.header.flags |= gateway_notification_guild_default_known;
        if (f.default_mentions)
            side.header.flags |= gateway_notification_guild_default_mentions;
    }
    if (guild && f.threads.kind == JsonValueKind::array)
        valid = valid && json_visit_indexed_array(doc, f.threads, 64, notification_joined, &side).ok();
    if (guild && (f.seen & 16777216)) {
        valid = valid && f.channels.kind == JsonValueKind::array &&
                json_visit_indexed_array(doc, f.channels, 64, notification_channel_meta, &side).ok();
        if (valid && !side.bad) {
            side.header.flags |= gateway_notification_channel_age_baseline_complete;
            if (!sensitive_guild &&
                (full_guild || ((f.seen & (67108864 | 134217728)) == (67108864 | 134217728))))
                side.header.known_gates |= notification_gate_age_access;
        }
    }
    if (thread && (f.seen & 512) && f.member.kind == JsonValueKind::object)
        valid = valid && append_notification_thread(side, f.member, f.id);
    if (self_thread)
        valid = valid && append_notification_thread(side, env.data, f.id);
    if (delta) {
        if (f.added.kind == JsonValueKind::array)
            valid = valid && json_visit_indexed_array(doc, f.added, 64, notification_added, &side).ok();
        if (f.removed.kind == JsonValueKind::array)
            valid = valid && json_visit_indexed_array(doc, f.removed, 64, notification_removed, &side).ok();
    }
    if (sync && f.members.kind == JsonValueKind::array)
        valid = valid && json_visit_indexed_array(doc, f.members, 64, notification_sync, &side).ok();
    if (!valid || side.bad) {
        side.header.flags = (side.header.flags & ~gateway_notification_channel_age_baseline_complete) |
                            gateway_notification_incomplete;
        side.header.known_gates = side.header.gates = 0;
        side.header.thread_count = 0;
        side.cursor = base + sizeof(side.header);
    }
    copy_bytes(storage.data + base, &side.header, sizeof(side.header));
    e->notification_payload_bytes = side.cursor - base;
    e->dispatch_payload_bytes = side.cursor;
    *payload = {reinterpret_cast<const u8*>(storage.data), side.cursor};
}

} // namespace maicord

#include "maicord/app/gateway_thread_members.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "gateway_dispatch_common.hpp"
#include "gateway_payload_writer.hpp"
namespace maicord {
namespace {
using PayloadWriter = GatewayPayloadWriter;
bool reject(GatewayEventDecodeError* error, GatewayEventDecodeError value = GatewayEventDecodeError::invalid_data) noexcept {
    if (*error == GatewayEventDecodeError::none) *error = value;
    return false;
}
bool visit_object(JsonValue value, const JsonIndexedDocument& document, u32 depth,
    JsonMemberVisitor visitor, void* context, GatewayEventDecodeError* error) noexcept {
    if (value.kind != JsonValueKind::object) return reject(error);
    const auto read = json_visit_indexed_object(document, value, depth, visitor, context);
    return read.ok() || reject(error, GatewayEventDecodeError::invalid_json);
}
struct MemberObject {
    JsonValue user;
    JsonValue roles;
    JsonValue presence;
    JsonString nickname;
    GatewayEventDecodeError error;
    bool user_seen;
    bool roles_seen;
    bool presence_seen;
    bool nickname_seen;
    bool nickname_null;
};

using UserObject = GatewayMemberUser;

struct PresenceObject {
    JsonValue user;
    JsonString status;
    GatewayEventDecodeError error;
    bool status_seen;
    bool user_seen;
    bool client_status_seen;
    u8 client_status;
};

bool visit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<MemberObject*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("user"))) {
        return gateway_keep_member(&context->error, &context->user_seen, &context->user, value,
                                   value.kind == JsonValueKind::object);
    }
    if (json_string_equals_ascii(name, utf8_literal("roles"))) {
        return gateway_keep_member(&context->error, &context->roles_seen, &context->roles, value,
                                   value.kind == JsonValueKind::array);
    }
    if (json_string_equals_ascii(name, utf8_literal("presence"))) {
        return gateway_keep_member(&context->error, &context->presence_seen, &context->presence, value,
                                   value.kind == JsonValueKind::object);
    }
    if (json_string_equals_ascii(name, utf8_literal("nick"))) {
        if (context->nickname_seen) {
            return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        }
        context->nickname_seen = true;
        context->nickname_null = value.kind == JsonValueKind::null_value;
        if (!context->nickname_null && !json_value_as_string(value, &context->nickname)) {
            return reject(&context->error);
        }
    }
    return true;
}

bool visit_presence(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<PresenceObject*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("user"))) {
        if (context->user_seen || value.kind != JsonValueKind::object) return reject(&context->error);
        context->user = value; context->user_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("status"))) {
        if (context->status_seen || !json_value_as_string(value, &context->status))
            return reject(&context->error, gateway_field_error(context->status_seen));
        context->status_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("client_status"))) {
        if (context->client_status_seen) return reject(&context->error, GatewayEventDecodeError::duplicate_field);
        context->client_status_seen = true;
        if (!client_status_decode(value, &context->client_status)) context->client_status = 0;
    }
    return true;
}

struct Outer {
    u64 user_id, thread_id, join_ms;
    JsonValue member, presence;
    bool id_seen, thread_seen, member_seen, presence_seen;
    GatewayEventDecodeError error;
};
bool visit_outer(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& o = *static_cast<Outer*>(opaque);
    if (json_string_equals_ascii(key, utf8_literal("user_id"))) {
        if (o.id_seen || !json_value_to_snowflake(value, &o.user_id)) return reject(&o.error);
        o.id_seen = true;
    } else if (json_string_equals_ascii(key, utf8_literal("id"))) {
        if (o.thread_seen || !json_value_to_snowflake(value, &o.thread_id)) return reject(&o.error);
        o.thread_seen = true;
    } else if (json_string_equals_ascii(key, utf8_literal("join_timestamp"))) {
        // Advisory: an unreadable time only leaves the join order unknown.
        JsonString text{};
        if (!json_value_as_string(value, &text) || !parse_iso_timestamp(text, &o.join_ms)) o.join_ms = 0;
    } else if (json_string_equals_ascii(key, utf8_literal("member"))) {
        if (o.member_seen || value.kind != JsonValueKind::object) return reject(&o.error);
        o.member_seen = true; o.member = value;
    } else if (json_string_equals_ascii(key, utf8_literal("presence"))) {
        if (o.presence_seen || (value.kind != JsonValueKind::object && value.kind != JsonValueKind::null_value)) return reject(&o.error);
        o.presence_seen = true; o.presence = value;
    }
    return true;
}
struct Roles {
    u64 ids[256]; u32 count; GatewayEventDecodeError error;
};
bool visit_role(void* opaque, u32 index, JsonValue value) noexcept {
    auto& r = *static_cast<Roles*>(opaque);
    if (index >= 256 || !json_value_to_snowflake(value, &r.ids[index])) return reject(&r.error);
    for (u32 i = 0; i < index; ++i) if (r.ids[i] == r.ids[index]) return reject(&r.error);
    r.count = index + 1; return true;
}
struct Root {
    u64 guild, thread;
    JsonValue members, removed, presences;
    bool snapshot, guild_seen, thread_seen, members_seen, removed_seen, presences_seen;
    GatewayEventDecodeError error;
};
bool visit_root(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& r = *static_cast<Root*>(opaque);
    if (json_string_equals_ascii(key, utf8_literal("guild_id"))) {
        if (r.guild_seen || !json_value_to_snowflake(value, &r.guild)) return reject(&r.error);
        r.guild_seen = true;
    } else if (json_string_equals_ascii(key, r.snapshot ? utf8_literal("thread_id") : utf8_literal("id"))) {
        if (r.thread_seen || !json_value_to_snowflake(value, &r.thread)) return reject(&r.error);
        r.thread_seen = true;
    } else if (json_string_equals_ascii(key, r.snapshot ? utf8_literal("members") : utf8_literal("added_members"))) {
        if (r.members_seen || value.kind != JsonValueKind::array) return reject(&r.error);
        r.members_seen = true; r.members = value;
    } else if (!r.snapshot && json_string_equals_ascii(key, utf8_literal("removed_member_ids"))) {
        if (r.removed_seen || value.kind != JsonValueKind::array) return reject(&r.error);
        r.removed_seen = true; r.removed = value;
    } else if (json_string_equals_ascii(key, utf8_literal("presences"))) {
        if (r.presences_seen || value.kind != JsonValueKind::array) return reject(&r.error);
        r.presences_seen = true; r.presences = value;
    }
    return true;
}
struct Array {
    GatewayPayloadWriter writer;
    const JsonIndexedDocument* document;
    u32 depth, records;
    u64 guild, thread;
    GatewayClientMemberListOperation operation;
    GatewayEventDecodeError error;
    u64 self, self_join_ms;
};
void merge_appearance(UserAppearance* target, const UserAppearance& other) noexcept {
    if (!target->user_id) { target->user_id = other.user_id; target->guild_id = other.guild_id; }
    if (other.fields & appearance_bot) target->bot = other.bot;
    if (other.fields & appearance_flags) target->public_flags = other.public_flags;
    if (other.fields & appearance_name) target->name = other.name;
    if (other.fields & appearance_decoration) target->decoration = other.decoration;
    if (other.fields & appearance_nameplate) { target->nameplate = other.nameplate; target->nameplate_sku = other.nameplate_sku; }
    if (other.fields & appearance_avatar) target->avatar = other.avatar;
    if (other.fields & appearance_guild_tag) {
        target->primary_guild_id = other.primary_guild_id; target->primary_badge = other.primary_badge;
        target->primary_tag = other.primary_tag; target->primary_enabled = other.primary_enabled;
    }
    target->fields |= other.fields;
}
bool append_record(Array* a, u64 id, JsonValue member_value, bool has_member,
    JsonValue presence_value, bool has_presence) noexcept {
    MemberObject member{}; UserObject user{}, presence_user{}; PresenceObject presence{};
    if (has_member && !visit_object(member_value, *a->document, a->depth, visit_member, &member, &member.error)) return reject(&a->error, member.error);
    if (member.user_seen && (!visit_object(member.user, *a->document, a->depth, gateway_visit_member_user, &user, &user.error) || !user.id_seen || user.id != id)) return reject(&a->error);
    if (has_presence && presence_value.kind == JsonValueKind::object) {
        if (!visit_object(presence_value, *a->document, a->depth, visit_presence, &presence, &presence.error)) return reject(&a->error);
        if (presence.user_seen && (!visit_object(presence.user, *a->document, a->depth, gateway_visit_member_user, &presence_user, &presence_user.error) || !presence_user.id_seen || presence_user.id != id)) return reject(&a->error);
        if (presence.status_seen && gateway_member_status(presence.status_seen, presence.status) == GatewayClientMemberListStatus::unknown) return reject(&a->error);
        if (!member.user_seen) user = presence_user;
        else {
            if (!user.username_seen && presence_user.username_seen) { user.username = presence_user.username; user.username_seen = true; }
            if (!user.global_name_seen && presence_user.global_name_seen) { user.global_name = presence_user.global_name; user.global_name_seen = true; user.global_name_null = presence_user.global_name_null; }
            if (!user.display_name_seen && presence_user.display_name_seen) { user.display_name = presence_user.display_name; user.display_name_seen = true; user.display_name_null = presence_user.display_name_null; }
            if (!user.avatar_seen && presence_user.avatar_seen) { user.avatar_hash = presence_user.avatar_hash; user.avatar_seen = true; user.avatar_null = presence_user.avatar_null; }
        }
    }
    Roles roles{};
    if (member.roles_seen && !json_visit_indexed_array(*a->document, member.roles, a->depth, visit_role, &roles).ok()) return reject(&a->error);
    GatewayClientMemberListMutation m{};
    m.user_id = id; m.operation = a->operation; m.role_count = static_cast<u16>(roles.count);
    m.status = gateway_member_status(presence.status_seen, presence.status);
    m.client_status = presence.client_status;
    if (has_member) m.hydration_flags |= gateway_thread_member_known;
    if (member.roles_seen) m.hydration_flags |= gateway_thread_roles_known;
    if (member.nickname_seen) m.hydration_flags |= gateway_thread_nickname_known;
    if (user.username_seen) m.hydration_flags |= gateway_thread_username_known;
    if (user.avatar_seen) m.hydration_flags |= gateway_thread_avatar_known;
    if (presence.status_seen) m.hydration_flags |= gateway_thread_presence_known;
    JsonString display = user.username;
    if (user.global_name_seen && !user.global_name_null) display = user.global_name;
    else if (user.display_name_seen && !user.display_name_null) display = user.display_name;
    if ((member.nickname_seen && !member.nickname_null && !gateway_member_string_size(member.nickname, &m.nickname_bytes)) ||
        (user.username_seen && (!gateway_member_string_size(user.username, &m.username_bytes) || !gateway_member_string_size(display, &m.display_name_bytes))) ||
        (user.avatar_seen && !user.avatar_null && !gateway_member_string_size(user.avatar_hash, &m.avatar_hash_bytes)) || m.avatar_hash_bytes > kUserAvatarHashMaximumBytes) return reject(&a->error);
    if (user.username_seen && !m.display_name_bytes) { display = user.username; m.display_name_bytes = m.username_bytes; }
    UserAppearance global{}, guild{}, extra_global{};
    if (member.user_seen && !user_appearance_decode(*a->document, member.user, id, 0, &global)) return reject(&a->error);
    if (presence.user_seen) {
        if (!user_appearance_decode(*a->document, presence.user, id, 0, &extra_global)) return reject(&a->error);
        merge_appearance(&global, extra_global);
    }
    if (has_member && !user_appearance_decode(*a->document, member_value, id, a->guild, &guild)) return reject(&a->error);
    if (has_presence && presence_value.kind == JsonValueKind::object && !activity_decode(*a->document, presence_value, &guild)) return reject(&a->error);
    if (!guild.user_id) { guild.user_id = id; guild.guild_id = a->guild; }
    u8 global_bytes[1024]{}, guild_bytes[1024]{};
    m.user_appearance_bytes = static_cast<u16>(user_appearance_write(global, {global_bytes, sizeof(global_bytes)}));
    m.member_appearance_bytes = static_cast<u16>(user_appearance_write(guild, {guild_bytes, sizeof(guild_bytes)}));
    if ((global.fields && !m.user_appearance_bytes) || (guild.fields && !m.member_appearance_bytes)) return reject(&a->error);
    if (!gateway_payload_append(&a->writer, &m, sizeof(m))) return reject(&a->error, GatewayEventDecodeError::message_content_capacity);
    for (u32 i = 0; i < roles.count; ++i) if (!gateway_payload_append_u64(&a->writer, roles.ids[i])) return reject(&a->error, GatewayEventDecodeError::message_content_capacity);
    if (!gateway_append_json_string(&a->writer, member.nickname, m.nickname_bytes) || !gateway_append_json_string(&a->writer, display, m.display_name_bytes) ||
        !gateway_append_json_string(&a->writer, user.avatar_hash, m.avatar_hash_bytes) || !gateway_append_json_string(&a->writer, user.username, m.username_bytes) ||
        !gateway_payload_append(&a->writer, global_bytes, m.user_appearance_bytes) || !gateway_payload_append(&a->writer, guild_bytes, m.member_appearance_bytes)) return reject(&a->error, GatewayEventDecodeError::message_content_capacity);
    ++a->records; return true;
}
bool visit_entry(void* opaque, u32, JsonValue value) noexcept {
    auto* a = static_cast<Array*>(opaque);
    if (a->operation == GatewayClientMemberListOperation::remove) {
        u64 id = 0;
        return json_value_to_snowflake(value, &id) ? append_record(a, id, {}, false, {}, false) : reject(&a->error);
    }
    if (a->operation == GatewayClientMemberListOperation::update) {
        PresenceObject p{}; UserObject user{};
        if (!visit_object(value, *a->document, a->depth, visit_presence, &p, &p.error) || !p.user_seen ||
            !visit_object(p.user, *a->document, a->depth, gateway_visit_member_user, &user, &user.error) || !user.id_seen) return reject(&a->error);
        return append_record(a, user.id, {}, false, value, true);
    }
    Outer o{};
    if (!visit_object(value, *a->document, a->depth, visit_outer, &o, &o.error) || !o.id_seen) return reject(&a->error);
    if (o.thread_seen && o.thread_id != a->thread) return reject(&a->error);
    if (a->self && o.user_id == a->self && o.join_ms) a->self_join_ms = o.join_ms;
    return append_record(a, o.user_id, o.member, o.member_seen, o.presence, o.presence_seen);
}
} // namespace

GatewayClientDispatchRouteResult gateway_client_route_thread_member_dispatch(const GatewayEnvelope& envelope,
    Slice<u32> scratch, u32 depth, Slice<char> storage, u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload, bool* handled) noexcept {
    if (!event || !payload || !handled || !scratch.data || !scratch.count || !storage.data || !depth) return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    Root root{};
    root.snapshot = json_string_equals_ascii(envelope.event_name, utf8_literal("THREAD_MEMBER_LIST_UPDATE"));
    *handled = root.snapshot || json_string_equals_ascii(envelope.event_name, utf8_literal("THREAD_MEMBERS_UPDATE"));
    if (!*handled) return {};
    const auto scan = json_scan_structurals_avx1(envelope.data.source, scratch);
    if (!scan.ok()) return {GatewayEventDecodeError::scan_failed, GatewayClientDispatchRouteError::decode, scan.error};
    const JsonIndexedDocument document{envelope.data.source, {scratch.data, scan.count}};
    const auto read = json_visit_root_object(document.input, document.structurals, depth, visit_root, &root);
    if (!read.ok() || !root.guild_seen || !root.thread_seen || (root.snapshot && !root.members_seen)) return {root.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::missing_required_field : root.error, GatewayClientDispatchRouteError::decode};
    Array array{{storage, 0}, &document, depth, 0, root.guild, root.thread, root.snapshot ? GatewayClientMemberListOperation::sync : GatewayClientMemberListOperation::insert, {}};
    array.self = self_user_id;
    bool valid = !root.members_seen || json_visit_indexed_array(document, root.members, depth, visit_entry, &array).ok();
    array.operation = GatewayClientMemberListOperation::remove;
    if (valid && root.removed_seen) valid = json_visit_indexed_array(document, root.removed, depth, visit_entry, &array).ok();
    array.operation = GatewayClientMemberListOperation::update;
    if (valid && root.presences_seen) valid = json_visit_indexed_array(document, root.presences, depth, visit_entry, &array).ok();
    if (!valid) return {array.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : array.error,
        array.error == GatewayEventDecodeError::message_content_capacity ? GatewayClientDispatchRouteError::payload_capacity : GatewayClientDispatchRouteError::decode};
    event->dispatch_kind = root.snapshot ? GatewayClientDispatchKind::thread_member_list_update : GatewayClientDispatchKind::thread_members_update;
    event->thread_members = {root.guild, root.thread, array.records, root.snapshot, false};
    *payload = {reinterpret_cast<const u8*>(storage.data), array.writer.used};
    event->dispatch_payload_bytes = payload->count;
    auto preflight_event = *event; preflight_event.kind = GatewayClientEventKind::dispatch;
    u32 cursor = 0, index = 0;
    GatewayClientMemberListMutation mutation{}; ConstSlice<u8> roles{}, global{}, guild{};
    Utf8View nickname{}, name{}, avatar{}, username{};
    u8 self_proof = 0;
    while (index < array.records) {
        if (!gateway_client_event_thread_member_record(preflight_event, *payload,
            &cursor, &index, &mutation, &roles, &nickname, &name, &avatar, &username, &global, &guild))
            return {GatewayEventDecodeError::invalid_data, GatewayClientDispatchRouteError::decode};
        if (self_user_id && mutation.user_id == self_user_id && mutation.operation != GatewayClientMemberListOperation::update) {
            if (self_proof) return {GatewayEventDecodeError::invalid_data, GatewayClientDispatchRouteError::decode};
            self_proof = gateway_thread_self_known;
            if (mutation.operation != GatewayClientMemberListOperation::remove) self_proof |= gateway_thread_self_joined;
        }
    }
    event->thread_members.authority_flags = self_proof;
    event->thread_members.authority_self_id = self_proof ? self_user_id : 0;
    if (self_proof & gateway_thread_self_joined)
        event->thread_members.authority_join_seconds = thread_join_seconds_from_ms(array.self_join_ms);
    return {};
}

GatewayClientDispatchRouteResult gateway_client_route_thread_member_dispatch(const GatewayEnvelope& envelope,
    Slice<u32> scratch, u32 depth, Slice<char> storage, GatewayClientEvent* event, ConstSlice<u8>* payload, bool* handled) noexcept {
    return gateway_client_route_thread_member_dispatch(envelope, scratch, depth, storage, 0, event, payload, handled);
}

bool gateway_client_event_thread_member_record(const GatewayClientEvent& event,
    ConstSlice<u8> payload, u32* cursor, u32* index, GatewayClientMemberListMutation* m,
    ConstSlice<u8>* roles, Utf8View* nickname, Utf8View* name, Utf8View* avatar,
    Utf8View* username, ConstSlice<u8>* global_extra, ConstSlice<u8>* guild_extra) noexcept {
    if (!cursor || !index || !m || !roles || !nickname || !name || !avatar || !username || !global_extra || !guild_extra ||
        event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::thread_member_list_update && event.dispatch_kind != GatewayClientDispatchKind::thread_members_update) ||
        !event.thread_members.guild_id || !event.thread_members.thread_id ||
        event.thread_members.snapshot != (event.dispatch_kind == GatewayClientDispatchKind::thread_member_list_update) ||
        event.dispatch_payload_bytes != payload.count || !payload.data || *index >= event.thread_members.record_count ||
        *cursor > payload.count || sizeof(*m) > payload.count - *cursor) return false;
    u32 at = *cursor;
    copy_bytes(m, payload.data + at, sizeof(*m)); at += sizeof(*m);
    const bool remove = m->operation == GatewayClientMemberListOperation::remove;
    const bool presence = m->operation == GatewayClientMemberListOperation::update;
    if ((!presence && m->operation != (event.thread_members.snapshot ? GatewayClientMemberListOperation::sync : GatewayClientMemberListOperation::insert) &&
        !(remove && !event.thread_members.snapshot)) || !m->user_id || m->index || m->range_end || m->kind != GatewayClientMemberListItemKind::member ||
        m->status > GatewayClientMemberListStatus::offline || m->hydration_flags & ~63u || m->role_count > 256 ||
        m->nickname_bytes > 128 || m->display_name_bytes > 128 || m->username_bytes > 128 || m->avatar_hash_bytes > 34 ||
        ((m->role_count || m->hydration_flags & gateway_thread_roles_known) && !(m->hydration_flags & gateway_thread_member_known)) ||
        ((m->hydration_flags & gateway_thread_nickname_known) && !(m->hydration_flags & gateway_thread_member_known)) ||
        (m->role_count && !(m->hydration_flags & gateway_thread_roles_known)) ||
        (m->nickname_bytes && !(m->hydration_flags & gateway_thread_nickname_known)) ||
        ((m->display_name_bytes || m->username_bytes) && !(m->hydration_flags & gateway_thread_username_known)) ||
        (m->avatar_hash_bytes && !(m->hydration_flags & gateway_thread_avatar_known)) ||
        ((m->status != GatewayClientMemberListStatus::unknown) != ((m->hydration_flags & gateway_thread_presence_known) != 0))) return false;
    const u32 role_bytes = m->role_count * 8u;
    const u32 text_bytes = m->nickname_bytes + m->display_name_bytes + m->avatar_hash_bytes + m->username_bytes + m->user_appearance_bytes + m->member_appearance_bytes;
    if ((remove && (text_bytes || role_bytes || m->hydration_flags)) || (presence && (m->hydration_flags & (gateway_thread_member_known | gateway_thread_roles_known | gateway_thread_nickname_known))) ||
        role_bytes > payload.count - at || text_bytes > payload.count - at - role_bytes) return false;
    *roles = {payload.data + at, role_bytes}; at += role_bytes;
    *nickname = {reinterpret_cast<const char*>(payload.data + at), m->nickname_bytes}; at += m->nickname_bytes;
    *name = {reinterpret_cast<const char*>(payload.data + at), m->display_name_bytes}; at += m->display_name_bytes;
    *avatar = {reinterpret_cast<const char*>(payload.data + at), m->avatar_hash_bytes}; at += m->avatar_hash_bytes;
    *username = {reinterpret_cast<const char*>(payload.data + at), m->username_bytes}; at += m->username_bytes;
    *global_extra = {payload.data + at, m->user_appearance_bytes}; at += m->user_appearance_bytes;
    *guild_extra = {payload.data + at, m->member_appearance_bytes}; at += m->member_appearance_bytes;
    if (!utf8_validate(*nickname).valid || !utf8_validate(*name).valid || !utf8_validate(*avatar).valid || !utf8_validate(*username).valid) return false;
    if (!gateway_avatar_hash_valid(*avatar)) return false;
    UserAppearance decoded{};
    if (!user_appearance_read(*global_extra, m->user_id, 0, &decoded) || !user_appearance_read(*guild_extra, m->user_id, event.thread_members.guild_id, &decoded)) return false;
    for (u32 i = 0; i < m->role_count; ++i) {
        const u64 id = byte_load_le64(roles->data + i * 8);
        if (!id) return false;
        for (u32 j = 0; j < i; ++j) if (byte_load_le64(roles->data + j * 8) == id) return false;
    }
    if (*index + 1 == event.thread_members.record_count && at != payload.count) return false;
    *cursor = at; ++*index; return true;
}

bool gateway_client_apply_thread_members(const GatewayClientEvent& input_event, ConstSlice<u8> input_payload,
    ThreadMemberStore* threads, GuildMemberStore* members, UserStore* users,
    const ChatStateStore& state) noexcept {
    if(input_event.dispatch_payload_bytes!=input_payload.count||(!input_payload.data&&input_payload.count))return false;
    GatewayClientEvent normalized_event;
    ConstSlice<u8> payload=input_payload;
    if(input_event.notification_payload_bytes&&
        !gateway_client_notification_trim(input_event,input_payload,&normalized_event,&payload))return false;
    const GatewayClientEvent& event=input_event.notification_payload_bytes?normalized_event:input_event;
    if (!threads || !members || !users || event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::thread_member_list_update && event.dispatch_kind != GatewayClientDispatchKind::thread_members_update) ||
        event.thread_members.snapshot != (event.dispatch_kind == GatewayClientDispatchKind::thread_member_list_update) ||
        !event.thread_members.guild_id || !event.thread_members.thread_id || payload.count != event.dispatch_payload_bytes ||
        (!payload.data && payload.count) || (!event.thread_members.record_count && payload.count)) return false;
    u32 cursor = 0, index = 0;
    GatewayClientMemberListMutation m{}; ConstSlice<u8> roles{}, global{}, guild{}; Utf8View nick{}, name{}, avatar{}, username{};
    u8 phase = 0;
    while (index < event.thread_members.record_count) {
        if (!gateway_client_event_thread_member_record(event, payload, &cursor, &index, &m, &roles, &nick, &name, &avatar, &username, &global, &guild)) return false;
        const u8 next = m.operation == GatewayClientMemberListOperation::update ? 2 : m.operation == GatewayClientMemberListOperation::remove ? 1 : 0;
        if (next < phase) return false;
        phase = next;
    }
    if (!thread_member_store_owns(*threads, event.thread_members.guild_id, event.thread_members.thread_id)) return true;
    if (event.thread_members.snapshot && !thread_member_store_snapshot_begin(threads, event.thread_members.guild_id, event.thread_members.thread_id)) return false;
    cursor = index = 0;
    // Syntax is validated above. Profile admission is a bounded cache operation;
    // a full profile cache must never discard a retained membership identity.
    while (index < event.thread_members.record_count) {
        if (!gateway_client_event_thread_member_record(event, payload, &cursor, &index, &m, &roles, &nick, &name, &avatar, &username, &global, &guild)) { thread_member_store_snapshot_abort(threads); return false; }
        if (m.operation == GatewayClientMemberListOperation::sync && !thread_member_store_snapshot_append(threads, m.user_id)) return false;
        else if (m.operation == GatewayClientMemberListOperation::insert) thread_member_store_add(threads, m.user_id);
        else if (m.operation == GatewayClientMemberListOperation::remove) thread_member_store_remove(threads, m.user_id);
        if (m.operation == GatewayClientMemberListOperation::remove) continue;
        if ((m.hydration_flags & gateway_thread_username_known) && username.bytes && name.bytes)
            user_store_upsert(users, m.user_id, name, username);
        if ((m.hydration_flags & gateway_thread_avatar_known) && user_store_find(*users, m.user_id)) user_store_set_avatar(users, m.user_id, avatar);
        UserAppearance appearance{};
        if (global.count && user_appearance_read(global, m.user_id, 0, &appearance)) { user_appearance_upsert(&users->appearance, appearance); saturating_increment(users->revision); }
        if (guild.count && user_appearance_read(guild, m.user_id, event.thread_members.guild_id, &appearance)) { user_appearance_upsert(&users->appearance, appearance); saturating_increment(users->revision); }
        if (m.hydration_flags & gateway_thread_member_known) {
            const auto* existing = guild_member_store_find(*members, event.thread_members.guild_id, m.user_id);
            const Utf8View effective_nick = m.hydration_flags & gateway_thread_nickname_known ? nick : existing ? guild_member_store_nickname(*existing) : Utf8View{};
            u64 ids[256]; for (u32 i = 0; i < m.role_count; ++i) ids[i] = byte_load_le64(roles.data + i * 8);
            guild_member_store_upsert(members, state, event.thread_members.guild_id, m.user_id, effective_nick, {ids, m.role_count},
                (m.hydration_flags & gateway_thread_roles_known) != 0, static_cast<GuildMemberStatus>(m.status));
        } else if (m.hydration_flags & gateway_thread_presence_known) {
            if (guild_member_store_find(*members, event.thread_members.guild_id, m.user_id))
                guild_member_store_set_status(members, event.thread_members.guild_id, m.user_id, static_cast<GuildMemberStatus>(m.status));
            else
                guild_member_store_upsert(members, state, event.thread_members.guild_id, m.user_id, {}, {}, false, static_cast<GuildMemberStatus>(m.status));
        }
    }
    if (event.thread_members.snapshot) return thread_member_store_snapshot_commit(threads, event.thread_members.incomplete);
    if (event.thread_members.incomplete && !threads->incomplete && threads->known) { threads->incomplete = true; saturating_increment(threads->revision); }
    return true;
}
static_assert(sizeof(GatewayClientMemberListMutation) == 40);
} // namespace maicord



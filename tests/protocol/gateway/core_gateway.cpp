#include "../../support/test_main_shared.hpp"

#include "../../support/model_store_fixtures.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "../../support/gateway_message_scratch.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/guild/member_list_channel_binding.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/model/chat/read_state.hpp"
#include "maicord/app/chat/messages/history_sync.hpp"
#include "maicord/protocol/gateway/qos_token.hpp"
#include "maicord/protocol/gateway/endpoint.hpp"
#include "maicord/protocol/gateway/reaction_events.hpp"
#include "maicord/protocol/gateway/runtime.hpp"
#include "maicord/protocol/gateway/session.hpp"
#include "maicord/protocol/auth/remote_auth.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"
#include "maicord/ui/chat/messages/reaction_picker.hpp"
#include "maicord/ui/shell/presentation.hpp"
#include "maicord/ui/shell/text_presentation.hpp"
#include "maicord/ui/theme/background_color_math.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/protocol/user/private_conversation.hpp"
#include "maicord/core/win32_exception.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
namespace {

u32 write_server_frame(WebSocketOpcode opcode, bool final, ConstSlice<u8> payload,
                       Slice<u8> output) noexcept {
    u32 header_bytes = 2;
    if (payload.count >= 126) {
        header_bytes += payload.count <= 0xFFFF ? 2 : 8;
    }
    if (!output.data || header_bytes + payload.count > output.count) {
        return 0;
    }
    u32 offset = 0;
    output[offset++] = static_cast<u8>((final ? 0x80u : 0) | static_cast<u8>(opcode));
    if (payload.count < 126) {
        output[offset++] = static_cast<u8>(payload.count);
    } else if (payload.count <= 0xFFFF) {
        output[offset++] = 126;
        output[offset++] = static_cast<u8>(payload.count >> 8);
        output[offset++] = static_cast<u8>(payload.count);
    } else {
        output[offset++] = 127;
        for (u32 index = 0; index < 8; ++index) {
            output[offset++] = static_cast<u8>(static_cast<u64>(payload.count) >> (56 - index * 8));
        }
    }
    for (u32 index = 0; index < payload.count; ++index) {
        output[offset++] = payload[index];
    }
    return offset;
}

GatewayClientEvent activity_message_event(u64 id, u64 channel_id, u64 author_id, u8 mention_flags = 0,
                                          u16 mention_role_count = 0) noexcept {
    constexpr u32 required = gateway_message_field_id | gateway_message_field_channel_id |
                             gateway_message_field_author_id | gateway_message_field_content |
                             gateway_message_field_timestamp;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_create;
    event.dispatch_payload_bytes = mention_role_count * static_cast<u32>(sizeof(u64));
    event.message.id = id;
    event.message.channel_id = channel_id;
    event.message.author_id = author_id;
    event.message.timestamp_ms = id;
    event.message.fields = required | (mention_role_count != 0 ? gateway_message_field_mention_roles : 0u);
    event.message.mention_role_count = mention_role_count;
    event.message.mention_flags = mention_flags;
    return event;
}

} // namespace

namespace {

GatewayDecodeResult decode_gateway(Utf8View input, GatewayEnvelope* envelope) noexcept {
    u32 structurals[1'024]{};
    return gateway_decode_envelope(input, Slice<u32>{structurals, 1'024}, 16, envelope);
}

void expect_gateway_error(Utf8View input, GatewayDecodeError error, const char* name) noexcept {
    GatewayEnvelope envelope{};
    const GatewayDecodeResult result = decode_gateway(input, &envelope);
    expect(result.error == error, name);
}

} // namespace

void test_gateway_member_list_update() noexcept {
    // Exercise an initial lazy-list SYNC, not just presence-sized deltas.
    static char sync_json[128 * 1024];
    u32 sync_bytes = 0;
    const auto append_sync = [&](Utf8View text) noexcept {
        if (text.bytes > sizeof(sync_json) - sync_bytes) return false;
        copy_bytes(sync_json + sync_bytes, text.data, text.bytes);
        sync_bytes += text.bytes;
        return true;
    };
    bool sync_written = append_sync(utf8_literal(
        R"({"op":0,"s":1,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"900","id":"everyone","member_count":5000,"online_count":1200,"ops":[{"op":"SYNC","range":[0,99],"items":[)"));
    for (u32 i = 0; sync_written && i < 100; ++i) {
        if (i) sync_written = append_sync(utf8_literal(","));
        char id[]{'1', static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10)};
        sync_written = sync_written && append_sync(utf8_literal(R"({"member":{"user":{"id":")")) &&
            append_sync(Utf8View{id, 3}) && append_sync(utf8_literal(
                R"(","username":"member","global_name":"Member"},"roles":[],"presence":{"status":"online","activities":[)"));
        const u32 activity_count = i == 0 ? 350 : 15;
        for (u32 activity = 0; sync_written && activity < activity_count; ++activity) {
            if (activity) sync_written = append_sync(utf8_literal(","));
            sync_written = sync_written && append_sync(utf8_literal(R"({"name":"activity","state":"rich"})"));
        }
        sync_written = sync_written && append_sync(utf8_literal("]}}}"));
    }
    sync_written = sync_written && append_sync(utf8_literal("]}]}}"));
    static u32 sync_scratch[32'768];
    static char sync_payload[kMessageEventPumpPayloadCapacity];
    GatewayEnvelope sync_envelope{};
    GatewayClientEvent sync_event{};
    ConstSlice<u8> sync_output{};
    u64 sync_ids[kGatewayBulkDeleteMaximumIds]{};
    const auto heavy_sync_scan = json_scan_structurals_avx1(
        Utf8View{sync_json, sync_bytes}, {sync_scratch, 32'768});
    expect(heavy_sync_scan.ok() && heavy_sync_scan.count > 16'384,
        "member_list_regression_exceeds_old_nested_scratch_without_exceeding_document_limit");
    const bool sync_routed = sync_written &&
        gateway_decode_envelope(Utf8View{sync_json, sync_bytes}, Slice<u32>{sync_scratch, 32'768}, 64,
                                &sync_envelope).ok() &&
        gateway_client_route_dispatch(sync_envelope, Slice<u32>{sync_scratch, 32'768}, 64,
            Slice<char>{sync_payload, kMessageEventPumpPayloadCapacity},
            Slice<u64>{sync_ids, kGatewayBulkDeleteMaximumIds}, 0, &sync_event, &sync_output, gateway_test_message_scratch()).ok();
    expect(sync_routed && sync_event.member_list.list_id == kMemberListEveryone &&
        sync_event.member_list.member_count == 5000 && sync_event.member_list.member_record_count == 100 &&
        sync_event.member_list.record_count == 101,
        "gateway_member_list_everyone_initial_100_member_sync_is_not_dropped");

    ChatStateStore sync_state{};
    GuildRecord sync_guild{};
    ChannelRecord sync_channels[2]{};
    u16 sync_channel_order[]{0, 1};
    sync_state.guilds = &sync_guild;
    sync_state.channels = sync_channels;
    sync_state.channel_order = sync_channel_order;
    sync_state.channel_count = sync_state.channel_capacity = 2;
    for (u32 i = 0; i < 2; ++i) {
        sync_channels[i].id = 901 + i;
        sync_channels[i].guild_id = 900;
        sync_channels[i].occupied = true;
        sync_channels[i].member_list_id = kMemberListEveryone;
    }
    GuildMemberRecord sync_members[100]{};
    u32 sync_member_order[100]{};
    GuildMemberStore sync_member_store{};
    UserRecord sync_users[100]{};
    u32 sync_user_order[100]{};
    u8 sync_names[4096]{};
    UserStore sync_user_store{};
    MessageTimeline sync_timeline{};
    MessageEventPump sync_pump{};
    sync_pump.timeline = &sync_timeline;
    sync_pump.members = &sync_member_store;
    sync_pump.users = &sync_user_store;
    sync_pump.state = &sync_state;
    const bool sync_applied = sync_routed &&
        guild_member_store_initialize(&sync_member_store, {sync_members, 100}, {sync_member_order, 100}) &&
        user_store_initialize(&sync_user_store, {sync_users, 100}, {sync_user_order, 100}, {sync_names, 4096}) &&
        message_event_pump_consume(&sync_pump, sync_event, sync_output).ok();
    MemberListRow sync_rows[101]{};
    u32 sync_projection_scratch[100]{};
    MemberListView sync_projection{};
    bool sync_visible = sync_applied && member_list_view_initialize(
        &sync_projection, {sync_rows, 101}, {sync_projection_scratch, 100});
    const u64 sync_switches[]{901, 902, 901};
    for (u64 channel_id : sync_switches) {
        sync_visible = sync_visible && member_list_view_rebuild(&sync_projection,
            sync_member_store, sync_user_store, sync_state, 900,
            member_list_channel_id(sync_state, channel_id)).ok() && sync_projection.member_count == 100;
    }
    expect(sync_visible, "member_list_initial_sync_survives_shared_channel_switch_without_presence_updates");

    const Utf8View packet = utf8_literal(
        R"({"op":0,"s":77,"t":"GUILD_MEMBER_LIST_UPDATE","d":{"guild_id":"900","id":"901","member_count":230,"online_count":18,"groups":[{"id":"online","count":18},{"id":"offline","count":212}],"ops":[{"op":"SYNC","range":[0,2],"items":[{"group":{"id":"online","count":18}},{"member":{"user":{"id":"101","username":"alice","global_name":"Alice","avatar":"0123456789abcdef0123456789abcdef"},"nick":"Ali","roles":["10","11"],"presence":{"status":"idle"}}},{"member":{"user":{"id":"102","username":"bob","global_name":null,"avatar":null},"nick":null,"roles":[],"presence":{"status":"offline"}}}]},{"op":"UPDATE","index":2,"item":{"member":{"user":{"id":"102","username":"bob","display_name":"Bobby"},"roles":["12"],"presence":{"status":"dnd"}}}},{"op":"INSERT","index":3,"item":{"member":{"user":{"id":"103","username":"charlie"},"roles":[],"presence":{"status":"online"}}}},{"op":"DELETE","index":1},{"op":"INVALIDATE","range":[100,199]}]}})");
    u32 envelope_scratch[1024]{};
    GatewayEnvelope envelope{};
    const GatewayDecodeResult decoded =
        gateway_decode_envelope(packet, Slice<u32>{envelope_scratch, 1024}, 32, &envelope);
    u32 route_scratch[4096]{};
    char payload_storage[kMessageEventPumpPayloadCapacity]{};
    u64 ids[kGatewayBulkDeleteMaximumIds]{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const GatewayClientDispatchRouteResult routed =
        decoded.ok()
            ? gateway_client_route_dispatch(envelope, Slice<u32>{route_scratch, 4096}, 32,
                                            Slice<char>{payload_storage, kMessageEventPumpPayloadCapacity},
                                            Slice<u64>{ids, kGatewayBulkDeleteMaximumIds}, 0, &event,
                                            &payload, gateway_test_message_scratch())
            : GatewayClientDispatchRouteResult{};
    expect(decoded.ok() && routed.ok() &&
               event.dispatch_kind == GatewayClientDispatchKind::guild_member_list_update &&
               event.member_list.guild_id == 900 && event.member_list.list_id == 901 &&
               event.member_list.member_count == 230 && event.member_list.online_count == 18 &&
               event.member_list.record_count == 10 && event.member_list.member_record_count == 4 &&
               event.member_list.groups_known && event.member_list.group_count == 2 &&
               event.member_list.logical_count == 232 &&
               event.dispatch_payload_bytes == payload.count,
           "gateway_member_list_route");
    u32 cursor = 0;
    u32 index = 0;
    bool records_valid = routed.ok();
    GatewayClientMemberListOperation expected_operations[] = {
        GatewayClientMemberListOperation::invalidate, GatewayClientMemberListOperation::sync,
        GatewayClientMemberListOperation::sync,       GatewayClientMemberListOperation::sync,
        GatewayClientMemberListOperation::update,
        GatewayClientMemberListOperation::insert,     GatewayClientMemberListOperation::remove,
        GatewayClientMemberListOperation::invalidate,
        GatewayClientMemberListOperation::group_summary, GatewayClientMemberListOperation::group_summary,
    };
    u32 expected_indices[] = {0, 0, 1, 2, 2, 3, 1, 100, 0, 19};
    for (u32 expected = 0; expected < 10 && records_valid; ++expected) {
        GatewayClientMemberListMutation mutation{};
        ConstSlice<u8> roles{};
        Utf8View nickname{};
        Utf8View name{};
        Utf8View avatar_hash{};
        Utf8View username{};
        records_valid = gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation,
                                                                &roles, &nickname, &name, &avatar_hash, &username);
        records_valid = records_valid && mutation.operation == expected_operations[expected] &&
                        mutation.index == expected_indices[expected];
        if (expected == 1) {
            records_valid = records_valid && mutation.kind == GatewayClientMemberListItemKind::group &&
                            mutation.user_id == kMemberListOnlineGroup && mutation.range_end == 18;
        } else if (expected == 2) {
            records_valid = records_valid && mutation.user_id == 101 && mutation.role_count == 2 &&
                            utf8_equal(nickname, utf8_literal("Ali")) &&
                            utf8_equal(name, utf8_literal("Alice")) &&
                            utf8_equal(username, utf8_literal("alice")) &&
                            utf8_equal(avatar_hash, utf8_literal("0123456789abcdef0123456789abcdef"));
        } else if (expected == 3) {
            records_valid = records_valid && mutation.user_id == 102 &&
                            mutation.status == GatewayClientMemberListStatus::offline &&
                            utf8_equal(name, utf8_literal("bob"));
        } else if (expected == 4) {
            records_valid = records_valid && mutation.user_id == 102 && mutation.role_count == 1 &&
                            mutation.status == GatewayClientMemberListStatus::dnd &&
                            utf8_equal(name, utf8_literal("Bobby"));
        } else if (expected == 5) {
            records_valid =
                records_valid && mutation.user_id == 103 && utf8_equal(name, utf8_literal("charlie"));
        } else if (expected == 7) {
            records_valid = records_valid && mutation.range_end == 199;
        } else if (expected >= 8) {
            records_valid = records_valid && mutation.kind == GatewayClientMemberListItemKind::summary &&
                mutation.range_end == (expected == 8 ? 18u : 212u);
        }
    }
    expect(records_valid && cursor == payload.count && index == 10, "gateway_member_list_packed_records");
}

void test_json_reader_and_gateway() noexcept {
    const Utf8View indexed_source = utf8_literal(
        R"({"before":0,"nested":{"items":[{"id":"1"},{"id":"2"}]},"after":true})");
    u32 indexed_offsets[128]{};
    const auto indexed_scan = json_scan_structurals_avx1(indexed_source, {indexed_offsets, 128});
    const JsonIndexedDocument indexed_document{indexed_source, {indexed_offsets, indexed_scan.count}};
    const auto capture_nested = [](void* context, JsonString name, JsonValue value) noexcept {
        if (json_string_equals_ascii(name, utf8_literal("nested")) ||
            json_string_equals_ascii(name, utf8_literal("items")))
            *static_cast<JsonValue*>(context) = value;
        return true;
    };
    JsonValue indexed_value{};
    const auto indexed_root = json_visit_root_object(indexed_source, indexed_document.structurals,
        16, capture_nested, &indexed_value);
    const auto indexed_object = json_visit_indexed_object(indexed_document, indexed_value,
        16, capture_nested, &indexed_value);
    const auto indexed_array = json_visit_indexed_array(indexed_document, indexed_value, 16, nullptr, nullptr);
    expect(indexed_scan.ok() && indexed_root.ok() && indexed_root.member_count == 3 &&
        indexed_object.ok() && indexed_object.member_count == 1 && indexed_array.ok() &&
        indexed_array.member_count == 2 &&
        json_visit_root_object(indexed_source, indexed_document.structurals, 16, nullptr, nullptr).ok(),
        "json_indexed_nested_views_keep_global_offsets_and_parent_index_intact");
    JsonValue outside_indexed{utf8_literal("[]"), 0, JsonValueKind::array};
    expect(json_visit_indexed_array(indexed_document, outside_indexed, 16, nullptr, nullptr).error ==
        JsonReadError::invalid_input, "json_indexed_view_rejects_foreign_document");
    JsonValue oversized_indexed = indexed_value;
    oversized_indexed.source.bytes = indexed_source.bytes;
    expect(json_visit_indexed_array(indexed_document, oversized_indexed, 16, nullptr, nullptr).error ==
        JsonReadError::invalid_input, "json_indexed_view_rejects_out_of_bounds_length");
    expect(json_visit_indexed_array(indexed_document, indexed_value, 0, nullptr, nullptr).error ==
        JsonReadError::depth_limit, "json_indexed_view_preserves_depth_limit");

    const char bom_object[] = {static_cast<char>(0xEF),
                               static_cast<char>(0xBB),
                               static_cast<char>(0xBF),
                               '{',
                               '"',
                               'x',
                               '"',
                               ':',
                               '1',
                               '}'};
    u32 bom_structurals[16]{};
    const Utf8View bom_view{bom_object, static_cast<u32>(sizeof(bom_object))};
    const JsonScanResult bom_scan = json_scan_structurals_avx1(bom_view, Slice<u32>{bom_structurals, 16});
    const JsonReadResult bom_read =
        bom_scan.ok() ? json_visit_root_object(bom_view, ConstSlice<u32>{bom_structurals, bom_scan.count},
                                               4, nullptr, nullptr)
                      : JsonReadResult{};
    expect(bom_scan.ok() && bom_read.ok() && bom_read.member_count == 1, "json_reader_optional_utf8_bom");

    GatewayEnvelope envelope{};
    const GatewayDecodeResult dispatch = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"x":[1,true,null,"\uD83D\uDE00"],"n":-0.1e+2},"s":42,"t":"MESSAGE_CREATE","unknown":{"a":1}})"),
        &envelope);
    expect(dispatch.ok(), "gateway_dispatch_decode");
    expect(envelope.opcode == 0 && envelope.has_sequence && envelope.sequence == 42,
           "gateway_dispatch_numeric_fields");
    expect(envelope.has_event_name &&
               json_string_equals_ascii(envelope.event_name, utf8_literal("MESSAGE_CREATE")),
           "gateway_dispatch_event_name");
    expect(envelope.data.kind == JsonValueKind::object && envelope.data.source.bytes != 0 &&
               envelope.data.source.data[0] == '{' &&
               envelope.data.source.data[envelope.data.source.bytes - 1] == '}',
           "gateway_dispatch_data_view");

    const GatewayDecodeResult escaped_fields = decode_gateway(
        utf8_literal(R"({"\u006f\u0070":10,"\u0064":{"heartbeat_interv\u0061l":45000},"s":null,"t":null})"),
        &envelope);
    expect(escaped_fields.ok() && envelope.opcode == 10, "gateway_escaped_field_names");
    expect(!envelope.has_sequence && !envelope.has_event_name, "gateway_nullable_metadata");
    u32 hello_scratch[64]{};
    GatewayHello hello{};
    const GatewayEventDecodeResult hello_result =
        gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello);
    expect(hello_result.ok() && hello.heartbeat_interval_ms == 45000, "gateway_hello_typed_decode");

    expect(decode_gateway(utf8_literal(R"({"op":10,"d":{"heartbeat_interval":1,"heartbeat_interval":2}})"),
                          &envelope)
                   .ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello).error ==
                   GatewayEventDecodeError::duplicate_field,
           "gateway_hello_duplicate_interval");
    expect(decode_gateway(utf8_literal(R"({"op":10,"d":{"heartbeat_interval":0}})"), &envelope).ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello).error ==
                   GatewayEventDecodeError::invalid_heartbeat_interval,
           "gateway_hello_zero_interval");
    expect(decode_gateway(utf8_literal(R"({"op":10,"d":{"heartbeat_interval":1.5}})"), &envelope).ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello).error ==
                   GatewayEventDecodeError::invalid_heartbeat_interval,
           "gateway_hello_fractional_interval");
    expect(decode_gateway(utf8_literal(R"({"op":10,"d":{"heartbeat_interval":4294967296}})"), &envelope)
                   .ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello).error ==
                   GatewayEventDecodeError::invalid_heartbeat_interval,
           "gateway_hello_interval_overflow");
    expect(decode_gateway(utf8_literal(R"({"op":10,"d":{"trace":[]}})"), &envelope).ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello).error ==
                   GatewayEventDecodeError::missing_heartbeat_interval,
           "gateway_hello_missing_interval");
    expect(decode_gateway(utf8_literal(R"({"op":10,"d":null})"), &envelope).ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{hello_scratch, 64}, 8, &hello).error ==
                   GatewayEventDecodeError::invalid_data,
           "gateway_hello_object_required");
    u32 tiny_hello_scratch[1]{};
    expect(decode_gateway(utf8_literal(R"({"op":10,"d":{"heartbeat_interval":45000}})"), &envelope).ok() &&
               gateway_decode_hello(envelope.data, Slice<u32>{tiny_hello_scratch, 1}, 8, &hello).error ==
                   GatewayEventDecodeError::scan_failed,
           "gateway_hello_scratch_bound");

    GatewayReady ready{};
    const GatewayDecodeResult user_ready_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"v":9,"user":{"id":"2"},"guilds":[],"private_channels":[],"session_id":"user-session","resume_gateway_url":"wss://gateway.discord.gg"},"s":8,"t":"READY"})"),
        &envelope);
    const GatewayEventDecodeResult user_ready_result =
        gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready);
    expect(user_ready_envelope.ok() && user_ready_result.ok() && ready.api_version == 9 &&
               ready.user_id == 2,
           "gateway_ready_normal_user_shape");

    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":10,"user":{},"guilds":[],"session_id":"s","resume_gateway_url":"wss://g","application":{}}})"),
            &envelope)
                .ok() &&
            gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                GatewayEventDecodeError::invalid_user,
        "gateway_ready_user_id_required");

    expect(decode_gateway(
               utf8_literal(
                   R"({"op":0,"d":{"v":10,"user":{},"session_id":"s","resume_gateway_url":"wss://g"}})"),
               &envelope)
                   .ok() &&
               gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                   GatewayEventDecodeError::missing_required_field,
           "gateway_ready_missing_required");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":"10","user":{},"guilds":[],"session_id":"s","resume_gateway_url":"wss://g","application":{}}})"),
            &envelope)
                .ok() &&
            gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                GatewayEventDecodeError::invalid_api_version,
        "gateway_ready_api_version_type");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":10,"user":[],"guilds":[],"session_id":"s","resume_gateway_url":"wss://g","application":{}}})"),
            &envelope)
                .ok() &&
            gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                GatewayEventDecodeError::invalid_user,
        "gateway_ready_user_type");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":10,"user":{},"guilds":{},"session_id":"s","resume_gateway_url":"wss://g","application":{}}})"),
            &envelope)
                .ok() &&
            gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                GatewayEventDecodeError::invalid_guilds,
        "gateway_ready_guilds_type");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":10,"user":{},"guilds":[],"session_id":"","resume_gateway_url":"wss://g","application":{}}})"),
            &envelope)
                .ok() &&
            gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                GatewayEventDecodeError::invalid_session_id,
        "gateway_ready_session_id_empty");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":10,"user":{},"guilds":[],"session_id":"s","resume_gateway_url":4,"application":{}}})"),
            &envelope)
                .ok() &&
            gateway_decode_ready(envelope.data, Slice<u32>{hello_scratch, 64}, 16, &ready).error ==
                GatewayEventDecodeError::invalid_resume_gateway_url,
        "gateway_ready_resume_url_type");
    expect(
        decode_gateway(utf8_literal(R"({"op":0,"d":{"_trace":[]},"s":8,"t":"RESUMED"})"), &envelope).ok() &&
            gateway_decode_resumed(envelope.data, Slice<u32>{hello_scratch, 64}, 8).ok(),
        "gateway_resumed_typed_decode");
    expect(
        decode_gateway(utf8_literal(R"({"op":0,"d":{"_trace":4},"s":8,"t":"RESUMED"})"), &envelope).ok() &&
            gateway_decode_resumed(envelope.data, Slice<u32>{hello_scratch, 64}, 8).error ==
                GatewayEventDecodeError::invalid_trace,
        "gateway_resumed_trace_type");

    bool resumable = false;
    expect(decode_gateway(utf8_literal(R"({"op":9,"d":true})"), &envelope).ok() &&
               gateway_decode_invalid_session(envelope.data, &resumable).ok() && resumable,
           "gateway_invalid_session_true");
    expect(decode_gateway(utf8_literal(R"({"op":9,"d":false})"), &envelope).ok() &&
               gateway_decode_invalid_session(envelope.data, &resumable).ok() && !resumable,
           "gateway_invalid_session_false");
    expect(decode_gateway(utf8_literal(R"({"op":9,"d":null})"), &envelope).ok() &&
               gateway_decode_invalid_session(envelope.data, &resumable).error ==
                   GatewayEventDecodeError::invalid_resumable,
           "gateway_invalid_session_type");

    const GatewayDecodeResult escaped_event =
        decode_gateway(utf8_literal(R"({"op":0,"d":{},"s":1,"t":"MESS\u0041GE_CREATE"})"), &envelope);
    expect(escaped_event.ok() &&
               json_string_equals_ascii(envelope.event_name, utf8_literal("MESSAGE_CREATE")),
           "gateway_escaped_event_name");

    const GatewayDecodeResult number_grammar =
        decode_gateway(utf8_literal(R"({"op":1,"d":[-0,0,1,-12,1.0,1e10,1E-2,0.01]})"), &envelope);
    expect(number_grammar.ok() && envelope.data.kind == JsonValueKind::array,
           "json_reader_valid_number_grammar");

    expect_gateway_error(utf8_literal(R"({"d":null})"), GatewayDecodeError::missing_opcode,
                         "gateway_missing_opcode");
    expect_gateway_error(utf8_literal(R"({"op":1})"), GatewayDecodeError::missing_data,
                         "gateway_missing_data");
    expect_gateway_error(utf8_literal(R"({"op":1,"op":2,"d":null})"), GatewayDecodeError::duplicate_field,
                         "gateway_duplicate_opcode");
    expect_gateway_error(utf8_literal(R"({"op":-1,"d":null})"), GatewayDecodeError::invalid_opcode,
                         "gateway_negative_opcode");
    expect_gateway_error(utf8_literal(R"({"op":1.0,"d":null})"), GatewayDecodeError::invalid_opcode,
                         "gateway_fractional_opcode");
    expect_gateway_error(utf8_literal(R"({"op":4294967296,"d":null})"), GatewayDecodeError::invalid_opcode,
                         "gateway_opcode_overflow");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":null,"s":18446744073709551616})"),
                         GatewayDecodeError::invalid_sequence, "gateway_sequence_overflow");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":null,"t":4})"), GatewayDecodeError::invalid_event_name,
                         "gateway_event_name_type");

    expect_gateway_error(utf8_literal(R"({"op":1,"d":null,})"), GatewayDecodeError::invalid_json,
                         "json_reader_trailing_comma");
    expect_gateway_error(utf8_literal(R"({"op":01,"d":null})"), GatewayDecodeError::invalid_json,
                         "json_reader_leading_zero");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":1.})"), GatewayDecodeError::invalid_json,
                         "json_reader_fraction_requires_digit");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":1e+})"), GatewayDecodeError::invalid_json,
                         "json_reader_exponent_requires_digit");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":"\q"})"), GatewayDecodeError::invalid_json,
                         "json_reader_invalid_escape");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":"\uD800"})"), GatewayDecodeError::invalid_json,
                         "json_reader_lone_surrogate");
    expect_gateway_error(utf8_literal(R"({"op":1,"d":null} true)"), GatewayDecodeError::invalid_json,
                         "json_reader_trailing_data");
    expect_gateway_error(utf8_literal(R"([1,2,3])"), GatewayDecodeError::invalid_json,
                         "json_reader_root_object_required");

    u32 structurals[64]{};
    const GatewayDecodeResult depth = gateway_decode_envelope(
        utf8_literal(R"({"op":1,"d":{"nested":true}})"), Slice<u32>{structurals, 64}, 1, &envelope);
    expect(depth.error == GatewayDecodeError::invalid_json &&
               depth.read_error == JsonReadError::depth_limit,
           "json_reader_depth_limit");

    u32 tiny_scratch[1]{};
    const GatewayDecodeResult scratch = gateway_decode_envelope(utf8_literal(R"({"op":1,"d":null})"),
                                                                Slice<u32>{tiny_scratch, 1}, 16, &envelope);
    expect(scratch.error == GatewayDecodeError::scan_failed &&
               scratch.scan_error == JsonScanError::output_overflow,
           "gateway_structural_scratch_bound");

    const u32 incomplete_index[] = {0};
    const JsonReadResult mismatch = json_visit_root_object(
        utf8_literal("{}"), ConstSlice<u32>{incomplete_index, 1}, 1, nullptr, nullptr);
    expect(mismatch.error == JsonReadError::structural_index_mismatch,
           "json_reader_structural_index_mismatch");

    const char invalid_utf8[] = {'{',
                                 '"',
                                 'o',
                                 'p',
                                 '"',
                                 ':',
                                 '1',
                                 ',',
                                 '"',
                                 'd',
                                 '"',
                                 ':',
                                 '"',
                                 static_cast<char>(0xC0),
                                 static_cast<char>(0xAF),
                                 '"',
                                 '}'};
    const GatewayDecodeResult bad_utf8 =
        decode_gateway(Utf8View{invalid_utf8, static_cast<u32>(sizeof(invalid_utf8))}, &envelope);
    expect(bad_utf8.error == GatewayDecodeError::invalid_json &&
               bad_utf8.read_error == JsonReadError::invalid_utf8,
           "json_reader_invalid_utf8");

    const GatewayDecodeResult null_output = gateway_decode_envelope(
        utf8_literal(R"({"op":1,"d":null})"), Slice<u32>{structurals, 64}, 16, nullptr);
    expect(null_output.error == GatewayDecodeError::invalid_json &&
               null_output.read_error == JsonReadError::invalid_input,
           "gateway_null_output");

    constexpr Utf8View complete_payload =
        utf8_literal(R"({"op":0,"d":{"items":[1,{"ok":true}]},"s":9,"t":"READY"})");
    bool truncations_rejected = true;
    for (u32 bytes = 0; bytes < complete_payload.bytes; ++bytes) {
        const GatewayDecodeResult truncated =
            decode_gateway(Utf8View{complete_payload.data, bytes}, &envelope);
        if (truncated.ok()) {
            truncations_rejected = false;
            break;
        }
    }
    expect(truncations_rejected, "json_reader_all_payload_truncations_rejected");
}

namespace {

void test_thread_authority_gateway() noexcept {
    GuildRecord guilds[2]{}; ChannelRecord channels[16]{};
    RoleRecord roles[8]{}; PermissionOverwriteRecord overwrites[8]{};
    u16 guild_order[2]{}, channel_order[16]{}; u8 names[4096]{};
    ChatStateStore state{};
    expect(chat_state_store_initialize(&state, {guilds,2}, {guild_order,2}, {channels,16}, {channel_order,16}, {names,4096}) &&
        chat_state_store_bind_permissions(&state, {roles,8}, {overwrites,8}), "thread authority: actual state and permission banks initialize");
    state.self_user_id = 99;
    MessageTimeline timeline{}; MessageEventPump pump{}; pump.timeline = &timeline; pump.state = &state;
    u64 guild_ids[2]{}; pump.guild_id_scratch = {guild_ids,2};
    u32 structurals[2048]{}; char bytes[8192]{}; u64 bulk[kGatewayBulkDeleteMaximumIds]{};
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    const auto route = [&](Utf8View name, Utf8View json) noexcept {
        GatewayEnvelope envelope{}; envelope.event_name = {name,0,false}; envelope.has_event_name = true;
        envelope.data = {json,0,JsonValueKind::object};
        event = {}; event.kind = GatewayClientEventKind::dispatch;
        return gateway_client_route_dispatch(envelope, {structurals,2048}, 32, {bytes,8192},
            {bulk,kGatewayBulkDeleteMaximumIds}, 99, &event, &payload, gateway_test_message_scratch()).ok();
    };
    const auto apply = [&]() noexcept { return message_event_pump_consume(&pump,event,payload).ok(); };
    expect(route(utf8_literal("GUILD_CREATE"), utf8_literal(R"({"id":"7","owner_id":"999","name":"native","roles":[{"id":"7","permissions":"274877975552"}],"members":[{"user":{"id":"99"},"roles":[]}],"channels":[{"id":"10","type":0,"name":"parent","permission_overwrites":[]}],"threads":[{"id":"20","parent_id":"10","type":12,"name":"private","thread_metadata":{"archived":false,"locked":true},"member":{"join_timestamp":"2026-10-05T12:00:00Z","flags":0}},{"id":"21","parent_id":"10","type":11,"name":"public","thread_metadata":{"archived":true,"locked":true}},{"id":"23","parent_id":"10","type":12,"name":"archived","thread_metadata":{"archived":true,"locked":false},"member":{"join_timestamp":"2026-10-05T12:00:00Z","flags":0}}]})")) && apply() &&
        chat_state_store_find_channel(state,20)->thread_self_joined_known &&
        chat_state_store_find_channel(state,20)->thread_locked &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,20)),
        "thread authority: GuildCreate omitted member IDs establish exact self and locked metadata");
    expect(route(utf8_literal("THREAD_MEMBERS_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"20","removed_member_ids":["99"]})")) && apply() &&
        !chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,20)) &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,21)),
        "thread authority: actual pump self removal revokes offselected private source while archived public reads remain");
    expect(route(utf8_literal("THREAD_MEMBER_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"20","user_id":"99","join_timestamp":"2026-10-05T12:00:00Z","flags":0})")) && apply() &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,20)),
        "thread authority: typed standalone current-self update restores authority independently of notifications");
    expect(route(utf8_literal("THREAD_MEMBER_LIST_UPDATE"),utf8_literal(R"({"guild_id":"7","thread_id":"20","members":[]})")) && apply() &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,20)),
        "thread authority: selected participant snapshot omission never proves current-self removal");
    expect(route(utf8_literal("THREAD_CREATE"),utf8_literal(R"({"guild_id":"7","id":"22","parent_id":"10","type":11,"name":"cached public","thread_metadata":{"archived":false,"locked":false}})")) && apply() &&
        chat_state_store_find_channel(state,22)->thread_active_known &&
        chat_state_store_find_channel(state,22)->thread_listed_active,
        "thread inventory: explicit unarchived create establishes positive active authority");
    expect(route(utf8_literal("THREAD_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"22","parent_id":"10","type":11,"name":"cached public"})")) && apply() &&
        chat_state_store_find_channel(state,22)->thread_active_known &&
        chat_state_store_find_channel(state,22)->thread_listed_active,
        "thread inventory: omitted update metadata preserves activity proof");
    expect(route(utf8_literal("CHANNEL_CREATE"),utf8_literal(R"({"guild_id":"7","id":"11","type":0,"name":"other parent","permission_overwrites":[]})")) && apply() &&
        route(utf8_literal("THREAD_CREATE"),utf8_literal(R"({"guild_id":"7","id":"26","parent_id":"11","type":11,"name":"other active","thread_metadata":{"archived":false,"locked":false}})")) && apply(),
        "thread inventory: actual gateway creates unrelated parent scope");
    const auto other_scope = *chat_state_store_find_channel(state,26);
    expect(route(utf8_literal("GUILD_CREATE"),utf8_literal(R"({"id":"8","owner_id":"999","name":"other guild","roles":[{"id":"8","permissions":"274877975552"}],"members":[{"user":{"id":"99"},"roles":[]}],"channels":[{"id":"80","type":0,"name":"other guild parent","permission_overwrites":[]}],"threads":[{"id":"81","parent_id":"80","type":11,"name":"other guild active","thread_metadata":{"archived":false}}]})")) && apply(),
        "thread inventory: actual guild create establishes unrelated guild authority");
    const auto other_guild_thread = *chat_state_store_find_channel(state,81);
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","channel_ids":["10"],"threads":[{"id":"24","parent_id":"10","type":10,"name":"announcement","thread_metadata":{"archived":false,"locked":true}}],"members":[]})")) && apply() &&
        !chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,20)) &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,23)) &&
        chat_state_store_find_channel(state,24)->thread_locked &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,24)) &&
        chat_state_store_find_channel(state,24)->thread_listed_active &&
        chat_state_store_find_channel(state,24)->thread_active_known &&
        chat_state_store_find_channel(state,10)->thread_listing_synced &&
        !chat_state_store_find_channel(state,11)->thread_listing_synced &&
        bytes_equal(&other_scope,chat_state_store_find_channel(state,26),sizeof(other_scope)) &&
        bytes_equal(&other_guild_thread,chat_state_store_find_channel(state,81),sizeof(other_guild_thread)) &&
        !chat_state_store_find_channel(state,22)->thread_listed_active &&
        chat_state_store_find_channel(state,22)->thread_active_known &&
        chat_state_store_find_channel(state,22)->thread_archived_known &&
        !chat_state_store_find_channel(state,22)->thread_archived &&
        chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,22)),
        "thread authority: complete scoped sync updates announcement metadata and active membership without inferring archived absence");
    expect(route(utf8_literal("THREAD_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"22","parent_id":"10","type":11,"name":"cached public"})")) && apply() &&
        chat_state_store_find_channel(state,22)->thread_active_known &&
        !chat_state_store_find_channel(state,22)->thread_listed_active,
        "thread inventory: omitted metadata cannot revive a retired cached unarchived public row");
    const auto* announcement = chat_state_store_find_channel(state,24);
    if (!announcement) { expect(false, "thread authority: coherent sync must establish the announcement source"); return; }
    const auto before = *announcement;
    const auto parent_before = *chat_state_store_find_channel(state,10);
    expect(!chat_state_store_set_thread_active(&state,24,98,false) &&
        !chat_state_store_set_thread_listing_synced(&state,10,98,false) &&
        bytes_equal(&before,chat_state_store_find_channel(state,24),sizeof(before)) &&
        bytes_equal(&parent_before,chat_state_store_find_channel(state,10),sizeof(parent_before)),
        "thread inventory: another self identity cannot change activity or listing proof");
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","channel_ids":["10"],"threads":[{"id":"24","parent_id":"10","type":10,"name":"changed","thread_metadata":{"archived":true,"locked":false}},{"id":"25","guild_id":"8","parent_id":"10","type":12,"name":"bad"}],"members":[]})")) &&
        !chat_state_apply_thread_authority(event,payload,&state).ok() &&
        bytes_equal(&before,chat_state_store_find_channel(state,24),sizeof(before)),
        "thread authority: malformed final sync record cannot partially publish earlier metadata");
    const u32 actual_capacity = state.channel_capacity;
    state.channel_capacity = state.channel_count;
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","channel_ids":["10"],"threads":[{"id":"24","parent_id":"10","type":10,"name":"changed"},{"id":"29","parent_id":"10","type":11,"name":"new"}],"members":[]})")) &&
        !chat_state_apply_thread_authority(event,payload,&state).ok() &&
        bytes_equal(&before,chat_state_store_find_channel(state,24),sizeof(before)) &&
        bytes_equal(&parent_before,chat_state_store_find_channel(state,10),sizeof(parent_before)),
        "thread inventory: capacity preflight rejects all publication including parent freshness");
    state.channel_capacity = actual_capacity;
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","channel_ids":[],"threads":[],"members":[]})")) && apply() &&
        chat_state_store_find_channel(state,24)->thread_listed_active &&
        chat_state_store_find_channel(state,26)->thread_listed_active &&
        !chat_state_store_find_channel(state,11)->thread_listing_synced,
        "thread inventory: explicit empty parent scope changes no cached activity or listing bits");
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","channel_ids":["10"],"threads":[{"id":"24","parent_id":"10","type":10,"name":"announcement"}],"members":[]})")) && apply() &&
        chat_state_store_find_channel(state,24)->thread_active_known &&
        chat_state_store_find_channel(state,24)->thread_listed_active &&
        chat_state_store_find_channel(state,24)->thread_locked &&
        chat_state_store_find_channel(state,24)->thread_archived_known,
        "thread inventory: sync listing proves activity independently of omitted cached metadata");
    expect(route(utf8_literal("THREAD_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"22","parent_id":"10","type":11,"name":"cached public","thread_metadata":{"archived":true}})")) && apply() &&
        chat_state_store_find_channel(state,22)->thread_active_known &&
        !chat_state_store_find_channel(state,22)->thread_listed_active &&
        route(utf8_literal("THREAD_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"22","parent_id":"10","type":11,"name":"cached public","thread_metadata":{"archived":false}})")) && apply() &&
        chat_state_store_find_channel(state,22)->thread_active_known &&
        chat_state_store_find_channel(state,22)->thread_listed_active,
        "thread inventory: explicit archive and unarchive updates retire and restore actual activity");
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","threads":[],"members":[]})")) && apply() &&
        chat_state_store_find_channel(state,10)->thread_listing_synced &&
        chat_state_store_find_channel(state,11)->thread_listing_synced &&
        chat_state_store_find_channel(state,26)->thread_active_known &&
        !chat_state_store_find_channel(state,26)->thread_listed_active &&
        !chat_state_store_find_channel(state,26)->thread_archived &&
        bytes_equal(&other_guild_thread,chat_state_store_find_channel(state,81),sizeof(other_guild_thread)) &&
        !chat_state_store_find_channel(state,80)->thread_listing_synced,
        "thread inventory: omitted parent IDs mean complete guild scope and retire cached rows without archiving them");
    expect(route(utf8_literal("THREAD_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"24","parent_id":"10","type":10,"name":"announcement","thread_metadata":{"archived":false}})")) && apply(),
        "thread inventory: gateway activity resumes after complete empty guild sync");
    expect(route(utf8_literal("THREAD_MEMBER_UPDATE"),utf8_literal(R"({"guild_id":"7","id":"20","user_id":"99","join_timestamp":"2026-10-05T12:00:00Z","flags":0})")) && apply(),
        "thread authority: fresh self proof admits after complete sync retirement");
    event = {}; event.kind = GatewayClientEventKind::resumed; payload = {};
    expect(apply() && chat_state_store_find_channel(state,20)->thread_self_joined_known &&
        chat_state_store_find_channel(state,24)->thread_active_known &&
        chat_state_store_find_channel(state,24)->thread_listed_active &&
        chat_state_store_find_channel(state,10)->thread_listing_synced,
        "thread authority: coherent RESUMED retains current membership and inventory proof");
    event = {}; event.kind = GatewayClientEventKind::dispatch; event.dispatch_kind = GatewayClientDispatchKind::private_state_incomplete;
    const auto gap = message_event_pump_consume(&pump,event,{});
    expect(gap.repaint && pump.resync_required && !chat_state_store_find_channel(state,20)->thread_self_joined_known &&
        !chat_state_store_channel_visible(state,*chat_state_store_find_channel(state,20)) &&
        !chat_state_store_find_channel(state,24)->thread_active_known &&
        !chat_state_store_find_channel(state,10)->thread_listing_synced &&
        !chat_state_store_find_channel(state,11)->thread_listing_synced,
        "thread authority: explicit unrecoverable incomplete state invalidates private proof before repaint");
    expect(route(utf8_literal("THREAD_LIST_SYNC"),utf8_literal(R"({"guild_id":"7","channel_ids":["10"],"threads":[{"id":"24","parent_id":"10","type":10,"name":"announcement"}],"members":[]})")) && apply() &&
        chat_state_store_find_channel(state,24)->thread_active_known &&
        chat_state_store_find_channel(state,10)->thread_listing_synced &&
        route(utf8_literal("GUILD_DELETE"),utf8_literal(R"({"id":"7","unavailable":true})")) && apply() &&
        !chat_state_store_find_channel(state,24)->thread_active_known &&
        !chat_state_store_find_channel(state,10)->thread_listing_synced,
        "thread inventory: temporary guild unavailability invalidates retained activity and listing freshness");
}

void test_thread_inventory_navigation() noexcept {
    GuildRecord guilds[1]{};
    ChannelRecord channels[8]{};
    RoleRecord roles[2]{};
    PermissionOverwriteRecord overwrites[2]{};
    u16 guild_order[1]{}, channel_order[8]{};
    u8 names[2048]{};
    ChatStateStore store{};
    const bool initialized = chat_state_store_initialize(&store, {guilds,1}, {guild_order,1},
        {channels,8}, {channel_order,8}, {names,2048}) &&
        chat_state_store_bind_permissions(&store, {roles,2}, {overwrites,2}) &&
        chat_state_store_upsert_guild(&store, {7,utf8_literal("Browser fixture"),
            GuildMutationKind::create,99}).ok() &&
        chat_state_store_upsert_role(&store, {7,7,kPermissionViewChannel |
            kPermissionReadMessageHistory,utf8_literal("everyone")}) &&
        chat_state_store_set_self_roles(&store,7,99,99,{});
    expect(initialized, "thread browser navigation: authoritative model banks initialize");
    if (!initialized) return;
    ChannelInput inputs[5]{
        {10,7,0,utf8_literal("general"),0,0,0},
        {15,7,0,utf8_literal("forum"),1,0,15},
        {16,7,0,utf8_literal("media"),2,0,16},
        {20,7,15,utf8_literal("formerly active"),0,0,11},
        {21,7,16,utf8_literal("archived API cache"),0,0,11}
    };
    inputs[3].last_message_id = 200;
    inputs[3].last_message_known = true;
    inputs[3].thread_archived_known = true;
    inputs[3].thread_listed_active = true;
    inputs[3].thread_active_known = true;
    inputs[3].thread_self_joined = inputs[3].thread_self_joined_known = true; // the rail lists joined threads
    inputs[4].last_message_id = 210;
    inputs[4].last_message_known = true;
    inputs[4].thread_archived = true;
    inputs[4].thread_archived_known = true;
    bool admitted = true;
    for (const auto& input : inputs)
        admitted = chat_state_store_upsert_channel(&store,input).ok() && admitted;
    chat_state_store_recompute_guild_permissions(&store,7);
    expect(admitted && chat_state_store_select_channel(&store,10).ok(),
        "thread browser navigation: native cached inputs admit without selecting discovery containers");
    if (!admitted) return;
    GuildViewRow guild_rows[1]{};
    ChannelViewRow channel_rows[8]{};
    u16 channel_scratch[8]{};
    u8 visibility_scratch[8]{};
    ChannelSortKey sort_keys[8]{};
    NavigationView navigation{};
    const bool view_ready = navigation_view_initialize(&navigation, {guild_rows,1},
        {channel_rows,8}, {channel_scratch,8}, {visibility_scratch,8}, {sort_keys,8}) &&
        navigation_view_rebuild(&navigation,store).ok();
    const auto* forum_row = navigation_view_find_channel(navigation,15);
    const auto* media_row = navigation_view_find_channel(navigation,16);
    expect(view_ready && forum_row && forum_row->selectable && media_row && media_row->selectable &&
        !channel_is_message_capable(15) && !channel_is_message_capable(16) &&
        !chat_state_store_select_channel(&store,15).ok() &&
        !chat_state_store_select_channel(&store,16).ok(),
        "thread browser navigation: forum and media rows open discovery while Core keeps them non-message-capable");
    expect(navigation_view_find_channel(navigation,20) &&
        !navigation_view_find_channel(navigation,21) &&
        chat_state_store_channel_visible(store,*chat_state_store_find_channel(store,21)),
        "thread browser navigation: archived API cache remains Core-readable without appearing in the rail");
    expect(chat_state_store_set_thread_active(&store,20,99,false) &&
        navigation_view_rebuild(&navigation,store).ok() &&
        !navigation_view_find_channel(navigation,20) &&
        !chat_state_store_find_channel(store,20)->thread_archived &&
        chat_state_store_channel_visible(store,*chat_state_store_find_channel(store,20)),
        "thread browser navigation: retired cached unarchived public inventory hides only its rail projection");
    expect(chat_state_store_select_channel(&store,20).ok() &&
        navigation_view_rebuild(&navigation,store).ok() &&
        navigation_view_find_channel(navigation,20) &&
        navigation_view_find_channel(navigation,20)->selected &&
        navigation_view_find_channel(navigation,20)->selectable &&
        !navigation_view_find_channel(navigation,21),
        "thread browser navigation: an open readable retired thread retains its selected conversation row");
    expect(chat_state_store_select_channel(&store,21).ok() &&
        navigation_view_rebuild(&navigation,store).ok() &&
        navigation_view_find_channel(navigation,21) &&
        navigation_view_find_channel(navigation,21)->selected &&
        !navigation_view_find_channel(navigation,20),
        "thread browser navigation: an archived cached thread retains its row only while open");
    expect(chat_state_store_select_channel(&store,10).ok() &&
        navigation_view_rebuild(&navigation,store).ok() &&
        !navigation_view_find_channel(navigation,20) &&
        !navigation_view_find_channel(navigation,21) && store.channel_count == 5 &&
        chat_state_store_find_channel(store,20)->last_message_id == 200 &&
        chat_state_store_find_channel(store,21)->last_message_id == 210 &&
        utf8_equal(chat_state_store_channel_name(store,*chat_state_store_find_channel(store,20)),
            utf8_literal("formerly active")) &&
        utf8_equal(chat_state_store_channel_name(store,*chat_state_store_find_channel(store,21)),
            utf8_literal("archived API cache")),
        "thread browser navigation: hiding discovery caches never deletes channel history anchors or names");
}

} // namespace

void test_chat_state() noexcept {
    test_thread_inventory_navigation();
    test_thread_authority_gateway();
    GuildRecord guild_records[4]{};
    ChannelRecord channel_records[8]{};
    RoleRecord role_records[16]{};
    PermissionOverwriteRecord overwrite_records[32]{};
    CustomEmojiRecord emoji_records[16]{};
    u64 emoji_roles[32]{};
    u16 guild_order[4]{};
    u16 channel_order[8]{};
    u8 names[2'048]{};
    u64 guild_ids[4]{};
    ChatStateStore store{};
    expect(chat_state_store_initialize(&store, Slice<GuildRecord>{guild_records, 4},
                                          Slice<u16>{guild_order, 4},
                                          Slice<ChannelRecord>{channel_records, 8},
                                          Slice<u16>{channel_order, 8}, Slice<u8>{names, 2'048}),
           "chat_state_initialize");
    expect(chat_state_store_bind_permissions(
               &store, Slice<RoleRecord>{role_records, 16},
               Slice<PermissionOverwriteRecord>{overwrite_records, 32}),
           "chat_state_permission_storage_bind");
    expect(chat_state_store_bind_emojis_sparse(&store, Slice<CustomEmojiRecord>{emoji_records, 16},
                                           Slice<u64>{emoji_roles, 32}),
           "chat_state_emoji_storage_bind");

    GatewayEnvelope envelope{};
    u32 scratch[512]{};
    u32 ready_object_scratch[512]{};
    char payload_storage[4'096]{};
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    GatewayReady ready{};
    const GatewayDecodeResult ready_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"v":9,"user":{"id":"1"},"guilds":[{"id":"100","unavailable":true},{"id":"200","unavailable":true}],"session_id":"s","resume_gateway_url":"wss://gateway.discord.gg"},"s":1,"t":"READY"})"),
        &envelope);
    const GatewayEventDecodeResult ready_decoded =
        gateway_decode_ready(envelope.data, Slice<u32>{scratch, 512}, 32, &ready);
    const GatewayClientDispatchRouteResult ready_routed = gateway_client_route_ready(
        ready, Slice<u32>{scratch, 512}, Slice<u32>{ready_object_scratch, 512}, 32,
        Slice<char>{payload_storage, 4'096}, &event, &payload);
    const ChatStateApplyResult ready_applied =
        chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 4});
    const GuildRecord* unavailable_guild = chat_state_store_find_guild(store, 100);
    expect(ready_envelope.ok() && ready_decoded.ok() && ready_routed.ok() && ready_applied.ok() &&
               store.guild_count == 2 && store.channel_count == 0 && store.selected_channel_id == 0 &&
               unavailable_guild && unavailable_guild->unavailable,
           "chat_state_ready_unavailable_catalog");

    const GatewayDecodeResult guild_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"id":"100","owner_id":"999","name":"Mai\u0043ord","icon":"0123456789abcdef0123456789abcdef","roles":[{"id":"100","permissions":"1024"},{"id":"300","permissions":"0","color":0,"colors":{"primary_color":12872687,"secondary_color":null,"tertiary_color":null}}],"members":[{"user":{"id":"1"},"roles":["300"]}],"channels":[{"id":"10","type":4,"position":0,"name":"text","permission_overwrites":[]},{"id":"11","type":0,"parent_id":"10","position":1,"name":"general","permission_overwrites":[]},{"id":"12","type":0,"position":0,"flags":131072,"name":"hidden","permission_overwrites":[]}],"threads":[{"id":"13","type":11,"parent_id":"11","name":"active","member":{"id":"13","user_id":"1","join_timestamp":"2026-01-01T00:00:00.000000+00:00","flags":0}}],"emojis":{"op":0,"items":[{"id":"500","name":"party_blob","roles":[],"animated":false,"available":true,"managed":false,"require_colons":true},{"id":"501","name":"reader_only","roles":["300"],"animated":true,"available":true},{"id":"502","name":"disabled_blob","roles":[],"available":false}]}},"s":2,"t":"GUILD_CREATE"})"),
        &envelope);
    const GatewayClientDispatchRouteResult guild_routed = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 512}, 32, Slice<char>{payload_storage, 4'096},
        Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, 1, &event, &payload, gateway_test_message_scratch());
    u32 cursor = 0;
    u32 channel_index = 0;
    GatewayClientStateMutation first_channel{};
    Utf8View first_channel_name{};
    const bool first_unpacked = gateway_client_event_next_guild_channel(
        event, payload, &cursor, &channel_index, &first_channel, &first_channel_name);
    Utf8View routed_guild_icon{};
    const bool guild_icon_unpacked = gateway_client_event_guild_icon(event, payload, &routed_guild_icon);
    const ChatStateApplyResult guild_applied =
        chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 4});
    const GuildRecord* guild = chat_state_store_find_guild(store, 100);
    const RoleRecord* enhanced_role = chat_state_store_find_role(store, 100, 300);
    u32 emoji_cursor = 0;
    u32 emoji_index = 0;
    GatewayClientEmojiMutation first_emoji{};
    ConstSlice<u8> first_emoji_roles{};
    Utf8View first_emoji_name{};
    const bool first_emoji_unpacked = gateway_client_event_next_guild_emoji(
        event, payload, &emoji_cursor, &emoji_index, &first_emoji, &first_emoji_roles, &first_emoji_name);
    const CustomEmojiRecord* party_emoji = chat_state_store_find_emoji(store, 500);
    const CustomEmojiRecord* restricted_emoji = chat_state_store_find_emoji(store, 501);
    const CustomEmojiRecord* disabled_emoji = chat_state_store_find_emoji(store, 502);
    expect(guild_envelope.ok() && guild_routed.ok() &&
               event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
               event.state.record_count == 4 && first_unpacked && first_channel.id == 10 &&
               first_channel.guild_id == 100 && first_channel_name.bytes == 4 && guild_applied.ok() &&
               store.channel_count == 4 && store.selected_channel_id == 11 && guild &&
               guild_icon_unpacked && routed_guild_icon.bytes == 32 && !guild->unavailable &&
               chat_state_store_guild_name(store, *guild).bytes == 7 &&
               chat_state_store_guild_icon_hash(*guild).bytes == 32 && guild->icon_variant != 0 &&
               enhanced_role && enhanced_role->color == 12'872'687u && event.state.emojis_known &&
               event.state.emoji_count == 3 && event.state.emoji_role_count == 1 && first_emoji_unpacked &&
               first_emoji.id == 500 && first_emoji_roles.count == 0 &&
               utf8_equal(first_emoji_name, utf8_literal("party_blob")) && store.emoji_count == 3 &&
               party_emoji && restricted_emoji && disabled_emoji &&
               chat_state_store_emoji_usable(store, *party_emoji) &&
               chat_state_store_emoji_usable(store, *restricted_emoji) &&
               !chat_state_store_emoji_usable(store, *disabled_emoji),
           "chat_state_guild_create_channels_selection");

    const GatewayDecodeResult emoji_update_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"guild_id":"100","emojis":[{"id":"503","name":"fresh_blob","roles":[],"available":true},{"id":"504","name":"staff_blob","roles":["9999"],"available":true,"animated":true}]},"s":3,"t":"GUILD_EMOJIS_UPDATE"})"),
        &envelope);
    const GatewayClientDispatchRouteResult emoji_update_routed = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 512}, 32, Slice<char>{payload_storage, 4'096},
        Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, 1, &event, &payload, gateway_test_message_scratch());
    const ChatStateApplyResult emoji_update_applied =
        chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 4});
    const CustomEmojiRecord* fresh_emoji = chat_state_store_find_emoji(store, 503);
    const CustomEmojiRecord* staff_emoji = chat_state_store_find_emoji(store, 504);
    ReactionPickerModel custom_picker{};
    ReactionPickerSelection custom_selection{};
    ReactionPreferenceStore custom_preferences{};
    const ReactionPickerResult custom_opened =
        reaction_preferences_initialize(&custom_preferences)
            ? reaction_picker_open(&custom_picker, 900, &custom_preferences, 1000, &store, 100)
            : ReactionPickerResult{};
    constexpr Utf8View fresh_query = utf8_literal("fresh");
    const bool custom_search = custom_opened.ok() && custom_picker.search &&
        message_composer_restore(custom_picker.search, 1, fresh_query, fresh_query.bytes, fresh_query.bytes) &&
        reaction_picker_refresh_search(&custom_picker).ok();
    bool custom_resolved = false;
    for (u32 i = 0; custom_search && !custom_resolved && i < custom_picker.result_count; ++i)
        custom_resolved = reaction_picker_result_at(custom_picker, i, &custom_selection) &&
                          custom_selection.custom && custom_selection.emoji_id == 503;
    expect(emoji_update_envelope.ok() && emoji_update_routed.ok() &&
               event.dispatch_kind == GatewayClientDispatchKind::guild_emojis_update &&
               emoji_update_applied.ok() && emoji_update_applied.emojis == 2 && store.emoji_count == 2 &&
               !chat_state_store_find_emoji(store, 500) && fresh_emoji && staff_emoji &&
               chat_state_store_emoji_usable(store, *fresh_emoji) &&
               !chat_state_store_emoji_usable(store, *staff_emoji) && custom_search && custom_resolved &&
               custom_selection.custom && custom_selection.emoji_id == 503 && !custom_selection.animated,
           "chat_state_emoji_convergence_usability_and_picker_selection");
    reaction_picker_release(&custom_picker);

    const GatewayDecodeResult invalid_emoji_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"guild_id":"100","emojis":[{"id":"505","name":"bad_roles","roles":["300","300"],"available":true}]},"s":4,"t":"GUILD_EMOJIS_UPDATE"})"),
        &envelope);
    const GatewayClientDispatchRouteResult invalid_emoji_routed = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 512}, 32, Slice<char>{payload_storage, 4'096},
        Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, 1, &event, &payload, gateway_test_message_scratch());
    expect(invalid_emoji_envelope.ok() && !invalid_emoji_routed.ok() && store.emoji_count == 2 &&
               chat_state_store_find_emoji(store, 503) && !chat_state_store_find_emoji(store, 505),
           "chat_state_emoji_malformed_update_is_atomic");

    const GatewayDecodeResult channel_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"id":"14","guild_id":"100","type":0,"position":0,"parent_id":null,"flags":0,"name":"first","member_list_id":"everyone","permission_overwrites":[]},"s":3,"t":"CHANNEL_CREATE"})"),
        &envelope);
    const GatewayClientDispatchRouteResult channel_routed = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 512}, 32, Slice<char>{payload_storage, 4'096},
        Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, &event, &payload, gateway_test_message_scratch());
    const ChatStateApplyResult channel_applied =
        chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 4});
    expect(channel_envelope.ok() && channel_routed.ok() && channel_applied.ok() &&
               store.channel_count == 5 && store.selected_channel_id == 11,
           "chat_state_channel_create_preserves_selection");
    const auto* member_list_channel = chat_state_store_find_channel(store, 14);
    expect(event.state.member_list_id_known && member_list_channel &&
        member_list_channel->member_list_id == kMemberListEveryone,
        "channel_server_member_list_id_survives_decode_and_model_apply");

    GuildViewRow guild_rows[4]{};
    ChannelViewRow channel_rows[8]{};
    u16 channel_view_scratch[8]{};
    u8 channel_visibility_scratch[8]{};
    ChannelSortKey channel_sort_keys[8]{};
    NavigationView navigation{};
    const bool navigation_initialized = navigation_view_initialize(
        &navigation, Slice<GuildViewRow>{guild_rows, 4},
        Slice<ChannelViewRow>{channel_rows, 8}, Slice<u16>{channel_view_scratch, 8},
        Slice<u8>{channel_visibility_scratch, 8}, Slice<ChannelSortKey>{channel_sort_keys, 8});
    const NavigationViewResult navigation_built =
        navigation_view_rebuild(&navigation, store);
    expect(navigation_initialized && navigation_built.ok() && navigation_built.changed &&
               navigation.guild_revision != 0 && navigation.channel_revision != 0 &&
               navigation.guild_count == 2 && navigation.channel_count == 4 &&
               navigation.channel_rows[0].id == 14 && navigation.channel_rows[0].depth == 0 &&
               navigation.channel_rows[1].id == 10 && navigation.channel_rows[1].selectable &&
               navigation.channel_rows[2].id == 11 && navigation.channel_rows[2].depth == 1 &&
               navigation.channel_rows[3].id == 13 && navigation.channel_rows[3].depth == 2 &&
               !navigation_view_find_channel(navigation, 12),
           "navigation_visual_order_hierarchy_obfuscated");
    const u32 navigation_guild_revision_before_selection = navigation.guild_revision;
    const u32 navigation_channel_revision_before_selection = navigation.channel_revision;

    UiScene shell_text_scene{};
    ShellNavigationScene shell_text_navigation_scene{};
    ShellNavigationRowWidget shell_text_navigation_rows[8]{};
    UiWidget shell_text_navigation_widgets[34]{};
    UiLayoutNode shell_text_navigation_layout[34]{};
    UiTextCommand shell_text_commands[kMaximumShellChromeTextCommands]{};
    char shell_text_owned_bytes[256]{};
    UiTextCommandStream shell_text_stream{};
    const bool shell_text_scene_ready = ui_scene_initialize_bootstrap(&shell_text_scene) &&
                                        ui_scene_layout_with_metrics(&shell_text_scene, UiShellView::guild_channel,
                                                                     kGuildChannelShellLayoutMetrics, 1280.0f,
                                                                     800.0f);
    const UiLayoutNode* shell_navigation =
        ui_scene_node(shell_text_scene, UiSemantic::channel_navigation);
    const bool shell_text_ready =
        shell_text_scene_ready &&
        shell_navigation_scene_initialize(&shell_text_navigation_scene,
                                          Slice<ShellNavigationRowWidget>{shell_text_navigation_rows, 8},
                                          Slice<UiWidget>{shell_text_navigation_widgets, 34},
                                          Slice<UiLayoutNode>{shell_text_navigation_layout, 34}) &&
        shell_navigation &&
        shell_navigation_scene_update(&shell_text_navigation_scene, &navigation,
                                      shell_navigation->rectangle, 0.0f)
            .ok() &&
        ui_text_command_stream_initialize(
            &shell_text_stream,
            Slice<UiTextCommand>{shell_text_commands, kMaximumShellChromeTextCommands},
            {shell_text_owned_bytes, sizeof(shell_text_owned_bytes)}) &&
        shell_text_presentation_build(shell_text_scene, store, navigation, kDefaultTheme,
                                      shell_text_navigation_scene, &shell_text_stream);
    const UiLayoutNode* shell_title =
        ui_scene_node(shell_text_scene, UiSemantic::title_bar);
    const UiLayoutNode* shell_title_label =
        ui_scene_node(shell_text_scene, UiSemantic::title_label);
    const UiLayoutNode* shell_member_toggle =
        ui_scene_node(shell_text_scene, UiSemantic::member_toggle);
    // Guild views draw the server name on the header's dropdown button.
    const UiLayoutNode* shell_channel_header =
        ui_scene_node(shell_text_scene, UiSemantic::guild_menu);
    bool found_channel_title = false;
    bool found_guild_header = false;
    bool found_member_toggle = false;
    bool found_home_label = false;
    u32 navigation_label_count = 0;
    for (u32 index = 0; index < shell_text_stream.count; ++index) {
        const UiTextCommand& command = shell_text_stream.storage[index];
        found_home_label = found_home_label || utf8_equal(command.text, utf8_literal("Home"));
        if (shell_title && shell_title_label && command.owner.manager == &shell_text_scene.widgets &&
            command.owner.handle.slot == ui_scene_widget(shell_text_scene, UiSemantic::title_label).slot) {
            found_channel_title = utf8_equal(command.text, utf8_literal("general")) &&
                                  command.rectangle.left == shell_title_label->rectangle.left &&
                                  command.clip.left == shell_title_label->rectangle.left &&
                                  command.vertical_alignment == UiTextAlignment::center;
        }
        if (shell_channel_header && command.owner.manager == &shell_text_scene.widgets &&
            command.owner.handle.slot == ui_scene_widget(shell_text_scene, UiSemantic::guild_menu).slot) {
            found_guild_header = utf8_equal(command.text, utf8_literal("MaiCord")) &&
                                 command.vertical_alignment == UiTextAlignment::center;
        }
        if (shell_member_toggle && command.owner.manager == &shell_text_scene.widgets &&
            command.owner.handle.slot == ui_scene_widget(shell_text_scene, UiSemantic::member_toggle).slot) {
            found_member_toggle = utf8_equal(command.text, utf8_literal("Hide Members"));
        }
        if (command.owner.manager == &shell_text_navigation_scene.widgets) {
            const ShellNavigationRowWidget* row =
                shell_navigation_scene_resolve(shell_text_navigation_scene, command.owner.handle);
            const UiLayoutNode* row_layout =
                row ? shell_navigation_scene_layout_node(shell_text_navigation_scene, *row) : nullptr;
            const UiLayoutNode* label_layout =
                row ? ui_widget_manager_layout_node(shell_text_navigation_scene.widgets, row->label)
                    : nullptr;
            const UiRect expected_rectangle =
                label_layout
                    ? ui_rect_intersection(label_layout->content_rectangle, shell_navigation->rectangle)
                    : UiRect{};
            const UiRect expected_clip =
                label_layout ? ui_rect_intersection(ui_rect_intersection(label_layout->clip_rectangle,
                                                                         label_layout->content_rectangle),
                                                    shell_navigation->rectangle)
                             : UiRect{};
            // A category's name ends where its chevron starts.
            if (row && row_layout && label_layout && command.rectangle.left == expected_rectangle.left &&
                (row->view.type == 4 || command.rectangle.right == expected_rectangle.right) &&
                command.clip.left == expected_clip.left && command.clip.top == expected_clip.top &&
                command.clip.right == expected_clip.right && command.clip.bottom == expected_clip.bottom &&
                command.wrapping == TextWrapping::no_wrap &&
                command.overflow == TextOverflow::ellipsis) {
                ++navigation_label_count;
            }
        }
    }
    expect(shell_text_ready && shell_text_stream.count == 6 && found_channel_title && found_guild_header &&
               !found_member_toggle && !found_home_label && navigation_label_count == 4,
           "shell_text_presentation_projects_owned_visible_labels");

    {
        const u64 selected_channel_before = store.selected_channel_id;
        const u64 navigation_selected_before = navigation.selected_channel_id;
        const auto* location = navigation_view_find_channel(navigation, 14);
        const auto* chat = navigation_view_find_channel(navigation, 11);
        const bool location_selected_before = location && location->selected;
        const bool chat_selected_before = chat && chat->selected;
        const bool location_text_ready = shell_text_ready &&
            shell_text_presentation_build(shell_text_scene, store, navigation, kDefaultTheme,
                shell_text_navigation_scene, nullptr, nullptr, nullptr, &shell_text_stream,
                nullptr, nullptr, 14);
        bool location_title = false, location_label = false;
        for (u32 i = 0; location_text_ready && i < shell_text_stream.count; ++i) {
            const auto& command = shell_text_stream.storage[i];
            if (shell_title_label && command.owner.manager == &shell_text_scene.widgets &&
                command.owner.handle.slot == ui_scene_widget(shell_text_scene, UiSemantic::title_label).slot)
                location_title = utf8_equal(command.text, utf8_literal("first"));
            if (command.owner.manager == &shell_text_navigation_scene.widgets &&
                utf8_equal(command.text, utf8_literal("first")))
                location_label = command.color.red == kDefaultTheme.text_primary.red &&
                    command.color.green == kDefaultTheme.text_primary.green &&
                    command.color.blue == kDefaultTheme.text_primary.blue;
        }
        PaintStream location_paint{};
        ShellPresentationRegions location_regions{};
        bool location_highlight = false, chat_highlight = false;
        const bool location_paint_ready = paint_stream_initialize(&location_paint) &&
            shell_presentation_build_chrome(shell_text_scene, &navigation, nullptr,
                &shell_text_navigation_scene, nullptr, nullptr, kDefaultTheme, 1280.f, 800.f,
                &location_paint, &location_regions, nullptr, 14);
        for (u32 i = 0; location_paint_ready && i < shell_text_navigation_scene.count; ++i) {
            const auto& row = shell_text_navigation_scene.rows[i];
            if (row.view.id != 14 && row.view.id != 11) continue;
            const auto* layout = shell_navigation_scene_layout_node(shell_text_navigation_scene, row);
            const auto* icon = ui_widget_manager_layout_node(shell_text_navigation_scene.widgets, row.icon);
            if (!layout || !icon) continue;
            const float right = layout->rectangle.right - kShellChannelHighlightRight;
            const UiRect highlight{layout->rectangle.left + kShellChannelHighlightLeft +
                                       static_cast<float>(row.view.depth) * kShellChannelIndent,
                                   layout->rectangle.top + 1.f, right, layout->rectangle.bottom - 1.f};
            for (u32 j = 0; j < location_paint.count; ++j) {
                const auto& quad = location_paint.quads[j];
                if (!ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, highlight)) continue;
                if (row.view.id == 14) location_highlight = true;
                if (row.view.id == 11) chat_highlight = true;
            }
        }
        expect(location_text_ready && location_title && location_label && location_paint_ready &&
            location_highlight && !chat_highlight && store.selected_channel_id == selected_channel_before &&
            navigation.selected_channel_id == navigation_selected_before && location && chat &&
            location->selected == location_selected_before && chat->selected == chat_selected_before,
            "shell_browser_location_projects_title_and_highlight_without_selecting_chat");
        ShellNavigationRowWidget* readonly_row = nullptr;
        for (u32 i = 0; i < shell_text_navigation_scene.count; ++i)
            if (shell_text_navigation_scene.rows[i].view.id == 14)
                readonly_row = &shell_text_navigation_scene.rows[i];
        const auto prior_row = readonly_row ? readonly_row->view : ChannelViewRow{};
        bool readonly_foreground = readonly_row && location_paint_ready;
        if (readonly_row) {
            readonly_row->view.type = 11;
            readonly_row->view.selectable = true;
            readonly_row->view.selected = readonly_row->view.unread = false;
            readonly_row->view.actions = channel_actions(11,
                kPermissionViewChannel | kPermissionReadMessageHistory, true);
        }
        const Theme* navigation_themes[]{&kDefaultTheme, &kLightTheme};
        for (const auto* navigation_theme : navigation_themes) {
            readonly_foreground = readonly_foreground &&
                shell_text_presentation_build(shell_text_scene, store, navigation, *navigation_theme,
                    shell_text_navigation_scene, nullptr, nullptr, nullptr, &shell_text_stream) &&
                shell_presentation_build_chrome(shell_text_scene, &navigation, nullptr,
                    &shell_text_navigation_scene, nullptr, nullptr, *navigation_theme, 1280.f, 800.f,
                    &location_paint, &location_regions, nullptr);
            bool readable_label = false, readable_icon = false;
            const auto* icon = readonly_row
                ? ui_widget_manager_layout_node(shell_text_navigation_scene.widgets, readonly_row->icon)
                : nullptr;
            for (u32 i = 0; readonly_foreground && i < shell_text_stream.count; ++i) {
                const auto& command = shell_text_stream.storage[i];
                if (command.owner.manager != &shell_text_navigation_scene.widgets ||
                    command.owner.handle.slot != readonly_row->label.slot) continue;
                const double foreground = background_color_luminance(command.color);
                const double background = background_color_luminance(navigation_theme->base_lower);
                const double contrast = (maximum(foreground, background) + .05) /
                                        (minimum(foreground, background) + .05);
                readable_label = bytes_equal(&command.color, &navigation_theme->text_secondary, sizeof(Color)) &&
                                 contrast >= 4.5;
            }
            // Threads draw the official spine (round-capped line quads) in place of an icon.
            for (u32 i = 0; readonly_foreground && icon && i < location_paint.count; ++i) {
                const auto& quad = location_paint.quads[i];
                if (quad.border_width == -1 && quad.right <= icon->rectangle.right &&
                    quad.left >= icon->rectangle.left - 16 && quad.top < icon->rectangle.bottom &&
                    quad.bottom > icon->rectangle.top &&
                    bytes_equal(&quad.fill, &navigation_theme->interactive_hover, sizeof(Color)))
                    readable_icon = true;
            }
            readonly_foreground = readonly_foreground && readable_label && readable_icon;
        }
        expect(readonly_foreground &&
            (readonly_row->view.actions & channel_action_read_history) != 0 &&
            (readonly_row->view.actions & channel_action_send_messages) == 0 &&
            store.selected_channel_id == selected_channel_before &&
            navigation.selected_channel_id == navigation_selected_before,
            "shell_readonly_thread_navigation_uses_readable_text_token_and_spine_in_dark_and_light_themes");
        if (readonly_row) readonly_row->view = prior_row;
        paint_stream_release(&location_paint);
    }

    const UiLayoutNode* shell_composer_host =
        ui_scene_node(shell_text_scene, UiSemantic::composer);
    char shell_composer_text[64]{};
    MessageComposer shell_composer_model{};
    MessageComposerScene shell_composer_scene{};
    const MessageComposerProjectionContext shell_composer_projection{nullptr, &store, navigation.selected_guild_id};
    const bool shell_composer_ready =
        shell_composer_host &&
        message_composer_initialize(&shell_composer_model,
                                    Slice<char>{shell_composer_text, sizeof(shell_composer_text)}) &&
        (message_composer_reset(&shell_composer_model, 11, false), true) &&
        message_composer_scene_initialize_owned(&shell_composer_scene) &&
        message_composer_scene_update(&shell_composer_scene, &shell_composer_model,
                                      shell_composer_host->rectangle, true, 0, {}, &shell_composer_projection)
            .ok() &&
        shell_text_presentation_build(shell_text_scene, store, navigation, kDefaultTheme,
                                      shell_text_navigation_scene, &shell_composer_scene, nullptr,
                                      &shell_text_stream);
    const UiLayoutNode* shell_composer_edit =
        message_composer_scene_layout_node(shell_composer_scene, MessageComposerControl::edit);
    bool found_centered_composer_placeholder = false;
    for (u32 index = 0; index < shell_text_stream.count; ++index) {
        const UiTextCommand& command = shell_text_stream.storage[index];
        if (command.owner.manager == &shell_composer_scene.widgets &&
            command.owner.handle.slot == shell_composer_scene.edit.slot) {
            found_centered_composer_placeholder =
                shell_composer_edit &&
                utf8_equal(command.text, utf8_literal("Message #general")) &&
                command.vertical_alignment == UiTextAlignment::center &&
                command.rectangle.top == shell_composer_edit->content_rectangle.top &&
                command.rectangle.bottom == shell_composer_edit->content_rectangle.bottom &&
                command.clip.top >= shell_composer_edit->rectangle.top &&
                command.clip.bottom <= shell_composer_edit->rectangle.bottom;
        }
    }
    expect(shell_composer_ready && found_centered_composer_placeholder,
           "shell_composer_text_uses_complete_control_vertical_center");
    UiRect typing_viewport = shell_composer_host ? shell_composer_host->rectangle : UiRect{};
    typing_viewport.top -= 24.0f;
    const bool typing_shell_ready = shell_composer_ready &&
        message_composer_scene_update(&shell_composer_scene, &shell_composer_model, typing_viewport,
            true, 0, {}, &shell_composer_projection, utf8_literal("Someone is typing…")).ok() &&
        shell_text_presentation_build(shell_text_scene, store, navigation, kDefaultTheme,
            shell_text_navigation_scene, &shell_composer_scene, nullptr, &shell_text_stream);
    bool typing_bright_above = false;
    for (u32 index = 0; index < shell_text_stream.count; ++index) {
        const UiTextCommand& command = shell_text_stream.storage[index];
        if (command.owner.manager == &shell_composer_scene.widgets &&
            command.owner.handle.slot == shell_composer_scene.typing_indicator.slot) {
            const Color expected = kDefaultTheme.text_primary;
            typing_bright_above = shell_composer_host &&
                command.rectangle.bottom < shell_composer_host->rectangle.top &&
                command.color.red == expected.red && command.color.green == expected.green &&
                command.color.blue == expected.blue && command.color.alpha == expected.alpha &&
                command.style_flags == text_style_subtext;
        }
    }
    expect(typing_shell_ready && typing_bright_above, "shell_typing_readable_small_text_above_composer");
    ui_text_command_stream_reset(&shell_text_stream);
    const UiTextCommand clipped_shell_label{
        utf8_literal("retained overscan label"),
        UiWidgetReference{&shell_text_scene.widgets,
                          shell_title_label ? ui_scene_widget(shell_text_scene, UiSemantic::title_label) : ui_widget_invalid_handle()},
        UiRect{16.0f, 16.0f, 160.0f, 36.0f},
        UiRect{16.0f, 36.0f, 160.0f, 36.0f},
        kDefaultTheme.text_primary,
        text_style_none,
        UiTextAlignment::start,
        UiTextAlignment::start,
        TextWrapping::no_wrap,
        TextOverflow::clip};
    expect(shell_title_label && ui_text_command_stream_push(&shell_text_stream, clipped_shell_label) &&
               shell_text_stream.count == 0 && shell_text_stream.rejected == 0,
           "text_presentation_clipped_overscan_is_nonfatal");
    UiTextCommand tiny_shell_text_commands[2]{};
    UiTextCommandStream tiny_shell_text_stream{};
    expect(ui_text_command_stream_initialize(&tiny_shell_text_stream,
                                             Slice<UiTextCommand>{tiny_shell_text_commands, 2}) &&
               !shell_text_presentation_build(shell_text_scene, store, navigation, kDefaultTheme,
                                              shell_text_navigation_scene, &tiny_shell_text_stream) &&
               tiny_shell_text_stream.count == 0,
           "shell_text_presentation_capacity_failure_is_atomic");
    bool friends_label_padded = false;
    if (ui_scene_layout_with_metrics(&shell_text_scene, UiShellView::friends, kFriendsShellLayoutMetrics,
                                     1280.f, 800.f)) {
        const auto* friends_text_navigation = ui_scene_node(shell_text_scene, UiSemantic::channel_navigation);
        const auto* friends_text_node =
            ui_scene_node(shell_text_scene, UiSemantic::friends_button);
        if (friends_text_navigation && friends_text_node &&
            shell_navigation_scene_update(&shell_text_navigation_scene, &navigation,
                                          friends_text_navigation->rectangle, 0.f)
                .ok() &&
            shell_text_presentation_build(shell_text_scene, store, navigation, kDefaultTheme,
                                          shell_text_navigation_scene, &shell_text_stream)) {
            for (u32 index = 0; index < shell_text_stream.count; ++index) {
                const auto& command = shell_text_commands[index];
                if (command.owner.handle.slot == ui_scene_widget(shell_text_scene, UiSemantic::friends_button).slot &&
                    command.owner.handle.generation == ui_scene_widget(shell_text_scene, UiSemantic::friends_button).generation &&
                    utf8_equal(command.text, utf8_literal("Friends"))) {
                    // The 16-DIP padding, then the 20-DIP people glyph and its 12-DIP gap.
                    friends_label_padded =
                        command.rectangle.left == friends_text_node->rectangle.left + 16.f + 32.f &&
                        command.rectangle.right == friends_text_node->rectangle.right - 16.f &&
                        ui_rect_equal(command.clip, friends_text_node->rectangle);
                }
            }
        }
    }
    expect(friends_label_padded, "shell_text_friends_label_follows_its_glyph_inside_the_16dip_content_padding");

    GuildViewRow tiny_guild_rows[1]{};
    ChannelViewRow tiny_channel_rows[2]{};
    u16 tiny_channel_scratch[2]{};
    ChannelSortKey tiny_sort_keys[2]{};
    NavigationView tiny_navigation{};
    const bool tiny_navigation_initialized = navigation_view_initialize(
        &tiny_navigation, Slice<GuildViewRow>{tiny_guild_rows, 1},
        Slice<ChannelViewRow>{tiny_channel_rows, 2}, Slice<u16>{tiny_channel_scratch, 2},
        Slice<u8>{channel_visibility_scratch, 8}, Slice<ChannelSortKey>{tiny_sort_keys, 2});
    const NavigationViewResult tiny_navigation_built =
        navigation_view_rebuild(&tiny_navigation, store);
    expect(tiny_navigation_initialized &&
               tiny_navigation_built.error == NavigationViewError::guild_capacity &&
               tiny_navigation.guild_count == 0 && tiny_navigation.channel_count == 0,
           "navigation_capacity_rejects_without_publishing");
    GuildViewRow narrow_guild_rows[4]{};
    ChannelViewRow narrow_channel_rows[2]{};
    u16 narrow_channel_scratch[2]{};
    ChannelSortKey narrow_sort_keys[2]{};
    NavigationView narrow_navigation{};
    const bool narrow_navigation_initialized = navigation_view_initialize(
        &narrow_navigation, Slice<GuildViewRow>{narrow_guild_rows, 4},
        Slice<ChannelViewRow>{narrow_channel_rows, 2}, Slice<u16>{narrow_channel_scratch, 2},
        Slice<u8>{channel_visibility_scratch, 8}, Slice<ChannelSortKey>{narrow_sort_keys, 2});
    const NavigationViewResult narrow_navigation_built =
        navigation_view_rebuild(&narrow_navigation, store);
    expect(narrow_navigation_initialized &&
               narrow_navigation_built.error == NavigationViewError::channel_capacity &&
               narrow_navigation.guild_count == 0 && narrow_navigation.channel_count == 0,
           "navigation_channel_capacity_rejects_without_publishing");

    const ChatStateSelectionResult select_channel = chat_state_store_select_channel(&store, 14);
    const ChatStateSelectionResult select_channel_again = chat_state_store_select_channel(&store, 14);
    const ChatStateSelectionResult select_category = chat_state_store_select_channel(&store, 10);
    const ChatStateSelectionResult select_unavailable_guild =
        chat_state_store_select_guild(&store, 200);
    const NavigationViewResult navigation_selected =
        navigation_view_rebuild(&navigation, store);
    expect(select_channel.ok() && select_channel.changed && select_channel_again.ok() &&
               !select_channel_again.changed &&
               select_category.error == ChatStateSelectionError::not_selectable &&
               select_unavailable_guild.error == ChatStateSelectionError::unavailable &&
               navigation_selected.ok() && navigation_selected.changed &&
               navigation.channel_rows[0].selected && navigation.selected_channel_id == 14 &&
               navigation.guild_revision == navigation_guild_revision_before_selection &&
               navigation.channel_revision == navigation_channel_revision_before_selection + 1,
           "navigation_explicit_selection_revision");

    const GatewayDecodeResult delete_envelope = decode_gateway(
        utf8_literal(R"({"op":0,"d":{"id":"11","guild_id":"100","type":0},"s":4,"t":"CHANNEL_DELETE"})"),
        &envelope);
    const GatewayClientDispatchRouteResult delete_routed = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 512}, 32, Slice<char>{payload_storage, 4'096},
        Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, &event, &payload, gateway_test_message_scratch());
    const ChatStateApplyResult delete_applied =
        chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 4});
    expect(delete_envelope.ok() && delete_routed.ok() && delete_applied.ok() && store.channel_count == 4 &&
               store.selected_channel_id == 14,
           "chat_state_selected_channel_delete_fallback");

    expect(decode_gateway(
               utf8_literal(R"({"op":0,"d":{"id":"100","unavailable":true},"s":5,"t":"GUILD_DELETE"})"),
               &envelope)
                   .ok() &&
               gateway_client_route_dispatch(
                   envelope, Slice<u32>{scratch, 512}, 32, Slice<char>{payload_storage, 4'096},
                   Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, &event, &payload, gateway_test_message_scratch())
                   .ok() &&
               chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 4}).ok() &&
               store.guild_count == 2 && store.channel_count == 4 && store.selected_channel_id == 0,
           "chat_state_outage_retains_but_deselects");
}

void test_channel_permissions() noexcept {
    GuildRecord guild_records[2]{};
    ChannelRecord channel_records[12]{};
    RoleRecord role_records[16]{};
    PermissionOverwriteRecord overwrite_records[32]{};
    u16 guild_order[2]{};
    u16 channel_order[12]{};
    u8 names[4'096]{};
    u64 guild_ids[2]{};
    ChatStateStore store{};
    const bool initialized = chat_state_store_initialize(
        &store, Slice<GuildRecord>{guild_records, 2}, Slice<u16>{guild_order, 2},
        Slice<ChannelRecord>{channel_records, 12}, Slice<u16>{channel_order, 12},
        Slice<u8>{names, 4'096});
    const bool permissions_bound = chat_state_store_bind_permissions(
        &store, Slice<RoleRecord>{role_records, 16},
        Slice<PermissionOverwriteRecord>{overwrite_records, 32});
    expect(initialized && permissions_bound, "channel_permissions_store_initialize");

    GatewayEnvelope envelope{};
    u32 scratch[2'048]{};
    u32 ready_object_scratch[2'048]{};
    char payload_storage[16'384]{};
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    GatewayReady ready{};
    const auto route_json = [&](Utf8View json) noexcept {
        return decode_gateway(json, &envelope).ok() &&
               gateway_client_route_dispatch(
                   envelope, Slice<u32>{scratch, 2'048}, 32, Slice<char>{payload_storage, 16'384},
                   Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, 1, &event, &payload, gateway_test_message_scratch())
                   .ok();
    };
    const auto apply_event = [&]() noexcept {
        return chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 2}).ok();
    };
    const bool ready_ok =
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"v":9,"user":{"id":"1"},"guilds":[{"id":"100","unavailable":true}],"session_id":"s","resume_gateway_url":"wss://gateway.discord.gg"},"s":1,"t":"READY"})"),
            &envelope)
            .ok() &&
        gateway_decode_ready(envelope.data, Slice<u32>{scratch, 2'048}, 32, &ready).ok() &&
        gateway_client_route_ready(ready, Slice<u32>{scratch, 2'048},
                                   Slice<u32>{ready_object_scratch, 2'048}, 32,
                                   Slice<char>{payload_storage, 16'384}, &event, &payload)
            .ok() &&
        chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 2}).ok();
    expect(ready_ok && store.self_user_id == 1, "channel_permissions_ready_self_identity");

    const bool permission_guild_decoded =
        gateway_decode_envelope(
            utf8_literal(
                R"({"op":0,"d":{"id":"100","owner_id":"999","name":"Permissions","roles":[{"id":"100","permissions":"0","name":"@everyone"},{"id":"2000","permissions":"0","name":"Reader"},{"id":"3000","permissions":"0","name":"Writer"}],"members":[{"user":{"id":"1"},"roles":["2000","3000"]}],"channels":[{"id":"10","type":4,"position":0,"name":"Private","permission_overwrites":[{"id":"100","type":0,"allow":"0","deny":"1024"}]},{"id":"11","type":0,"parent_id":"10","position":1,"name":"role-allow","permission_overwrites":[{"id":"3000","type":0,"allow":"1024","deny":"0"}]},{"id":"12","type":0,"position":2,"name":"role-collision","permission_overwrites":[{"id":"2000","type":0,"allow":"0","deny":"1024"},{"id":"3000","type":0,"allow":"1024","deny":"0"}]},{"id":"13","type":0,"position":3,"name":"member-deny","permission_overwrites":[{"id":"3000","type":0,"allow":"1024","deny":"0"},{"id":"1","type":1,"allow":"0","deny":"1024"}]},{"id":"14","type":0,"position":4,"name":"thread-parent","permission_overwrites":[{"id":"100","type":0,"allow":"1024","deny":"0"}]}],"threads":[{"id":"15","type":11,"parent_id":"14","position":0,"name":"thread","member":{"id":"15","user_id":"1","join_timestamp":"2026-01-01T00:00:00.000000+00:00","flags":0}}]},"s":2,"t":"GUILD_CREATE"})"),
            Slice<u32>{scratch, 2'048}, 32, &envelope)
            .ok();
    const bool permission_guild_routed =
        permission_guild_decoded &&
        gateway_client_route_dispatch(
            envelope, Slice<u32>{scratch, 2'048}, 32, Slice<char>{payload_storage, 16'384},
            Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, 1, &event, &payload, gateway_test_message_scratch())
            .ok();
    GatewayClientEvent permission_body_event{};
    ConstSlice<u8> permission_body{};
    const bool permission_body_valid=permission_guild_routed&&
        gateway_client_notification_trim(event,payload,&permission_body_event,&permission_body)&&
        permission_body_event.notification_payload_bytes==0&&
        permission_body_event.dispatch_payload_bytes==permission_body.count&&
        event.notification_payload_bytes>=sizeof(GatewayClientNotificationSidecar)&&
        event.notification_payload_bytes<=payload.count&&
        permission_body.data==payload.data&&permission_body.count==payload.count-event.notification_payload_bytes;
    GatewayClientRoleMutation packet_role{};
    Utf8View packet_role_name{};
    u32 packet_cursor = 0;
    u32 packet_channel_index = 0;
    bool packet_channels = true;
    u32 packet_overwrites = 0;
    while (permission_body_valid && packet_channel_index < permission_body_event.state.record_count) {
        GatewayClientStateMutation packet_channel{};
        Utf8View packet_name{};
        ConstSlice<u8> packet_overwrite_payload{};
        packet_channels = gateway_client_event_next_guild_channel(
                              permission_body_event, permission_body, &packet_cursor, &packet_channel_index, &packet_channel,
                              &packet_name, &packet_overwrite_payload) &&
                          packet_channels;
        packet_overwrites += packet_channel.overwrite_count;
    }
    const bool packet_role_read =
        gateway_client_event_guild_role(permission_body_event, permission_body, 0, &packet_role, &packet_role_name);
    const bool permission_packet_valid =
        permission_body_valid && event.state.role_count == 3 && event.state.member_role_count == 2 &&
        event.state.record_count == 6 && event.state.overwrite_count == 7 && packet_role_read &&
        packet_role.id == 100 && utf8_equal(packet_role_name, utf8_literal("@everyone")) &&
        packet_channels &&
        packet_cursor == permission_body.count && packet_overwrites == 7;
    const ChatStateApplyResult permission_guild_apply =
        permission_guild_routed
            ? chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 2})
            : ChatStateApplyResult{};
    const bool permission_guild_applied = permission_guild_routed && permission_guild_apply.ok();
    const bool guild_ok = permission_guild_decoded && permission_guild_routed && permission_guild_applied;
    expect(permission_packet_valid, "channel_permissions_guild_packet_layout");
    if(permission_body_valid) {
        GatewayClientEvent wrong_length=permission_body_event;
        ++wrong_length.dispatch_payload_bytes;
        const u32 guild_count_before=store.guild_count,channel_count_before=store.channel_count;
        const u32 roles_before=store.role_count,overwrites_before=store.overwrite_count;
        const u32 selection_before=store.selection_revision;
        const auto rejected=chat_state_apply_gateway_event(&store,wrong_length,permission_body,Slice<u64>{guild_ids,2});
        expect(rejected.error==ChatStateApplyError::invalid_input&&store.guild_count==guild_count_before&&
            store.channel_count==channel_count_before&&store.role_count==roles_before&&
            store.overwrite_count==overwrites_before&&store.selection_revision==selection_before,
            "channel_permissions_normalized_body_length_preflight_preserves_state");
    }
    const ChannelRecord* category = chat_state_store_find_channel(store, 10);
    const ChannelRecord* role_allow = chat_state_store_find_channel(store, 11);
    const ChannelRecord* role_collision = chat_state_store_find_channel(store, 12);
    const ChannelRecord* member_deny = chat_state_store_find_channel(store, 13);
    const ChannelRecord* thread_parent = chat_state_store_find_channel(store, 14);
    const ChannelRecord* thread = chat_state_store_find_channel(store, 15);
    const RoleRecord* reader_role = chat_state_store_find_role(store, 100, 2000);
    expect(
        guild_ok && store.role_count == 3 && store.overwrite_count == 7 && category && role_allow &&
            role_collision && member_deny && thread_parent && thread && reader_role &&
            utf8_equal(chat_state_store_role_name(store, *reader_role), utf8_literal("Reader")) &&
            !chat_state_store_channel_visible(store, *category) &&
            chat_state_store_channel_visible(store, *role_allow) &&
            chat_state_store_channel_visible(store, *role_collision) &&
            !chat_state_store_channel_visible(store, *member_deny) &&
            chat_state_store_channel_visible(store, *thread_parent) &&
            chat_state_store_channel_visible(store, *thread) && store.selected_channel_id == 11,
        "channel_permissions_overwrite_precedence_thread_inheritance");

    const bool missing_create_policy_routed = route_json(utf8_literal(
        R"({"op":0,"d":{"id":"16","guild_id":"100","type":0,"position":5,"name":"missing-policy"},"s":3,"t":"CHANNEL_CREATE"})"));
    const ChatStateApplyResult missing_create_policy =
        missing_create_policy_routed
            ? chat_state_apply_gateway_event(&store, event, payload, Slice<u64>{guild_ids, 2})
            : ChatStateApplyResult{};
    expect(missing_create_policy_routed && !missing_create_policy.ok() &&
               missing_create_policy.error == ChatStateApplyError::invalid_payload &&
               !chat_state_store_find_channel(store, 16),
           "channel_permissions_channel_create_without_policy_fails_closed");

    GuildViewRow guild_rows[2]{};
    ChannelViewRow channel_rows[12]{};
    u16 channel_scratch[12]{};
    u8 channel_visibility_scratch[12]{};
    ChannelSortKey channel_sort_keys[12]{};
    NavigationView navigation{};
    expect(navigation_view_initialize(&navigation, Slice<GuildViewRow>{guild_rows, 2},
                                              Slice<ChannelViewRow>{channel_rows, 12},
                                              Slice<u16>{channel_scratch, 12},
                                              Slice<u8>{channel_visibility_scratch, 12},
                                              Slice<ChannelSortKey>{channel_sort_keys, 12}) &&
               navigation_view_rebuild(&navigation, store).ok() && navigation.channel_count == 5 &&
               navigation_view_find_channel(navigation, 10) &&
               navigation_view_find_channel(navigation, 11) &&
               !navigation_view_find_channel(navigation, 13),
           "channel_permissions_hidden_category_retained_for_visible_child");

    const bool member_update_ok =
        route_json(utf8_literal(
            R"({"op":0,"d":{"guild_id":"100","roles":["2000"],"user":{"id":"1"}},"s":3,"t":"GUILD_MEMBER_UPDATE"})")) &&
        event.dispatch_kind == GatewayClientDispatchKind::guild_member_update && apply_event();
    role_allow = chat_state_store_find_channel(store, 11);
    role_collision = chat_state_store_find_channel(store, 12);
    expect(member_update_ok && role_allow && role_collision &&
               !chat_state_store_channel_visible(store, *role_allow) &&
               !chat_state_store_channel_visible(store, *role_collision) &&
               store.selected_channel_id == 14,
           "channel_permissions_self_member_update_selection_fallback");

    const bool administrator_ok =
        route_json(utf8_literal(
            R"({"op":0,"d":{"guild_id":"100","role":{"id":"2000","permissions":"8"}},"s":4,"t":"GUILD_ROLE_UPDATE"})")) &&
        event.dispatch_kind == GatewayClientDispatchKind::guild_role_update && apply_event();
    member_deny = chat_state_store_find_channel(store, 13);
    const ChatStateSelectionResult selected_member = chat_state_store_select_channel(&store, 13);
    expect(administrator_ok && member_deny && chat_state_store_channel_visible(store, *member_deny) &&
               selected_member.ok() && store.selected_channel_id == 13,
           "channel_permissions_administrator_bypasses_overwrites");

    const bool administrator_removed = route_json(utf8_literal(
                                           R"({"op":0,"d":{"guild_id":"100","role":{"id":"2000","permissions":"0"}},"s":5,"t":"GUILD_ROLE_UPDATE"})")) &&
                                       apply_event();
    expect(administrator_removed && store.selected_channel_id == 14,
           "channel_permissions_role_update_revokes_selected_channel");

    const bool partial_parent_update =
        route_json(utf8_literal(
            R"({"op":0,"d":{"id":"14","guild_id":"100","type":0,"position":4,"name":"thread-parent-renamed"},"s":6,"t":"CHANNEL_UPDATE"})")) &&
        (event.state.fields & gateway_client_state_field_permission_overwrites) == 0 && apply_event();
    thread_parent = chat_state_store_find_channel(store, 14);
    expect(partial_parent_update && thread_parent && store.overwrite_count == 7 &&
               chat_state_store_channel_visible(store, *thread_parent),
           "channel_permissions_partial_channel_update_preserves_overwrites");

    const bool parent_hidden = route_json(utf8_literal(
                                   R"({"op":0,"d":{"id":"14","guild_id":"100","type":0,"position":4,"name":"thread-parent","permission_overwrites":[{"id":"100","type":0,"allow":"0","deny":"1024"}]},"s":6,"t":"CHANNEL_UPDATE"})")) &&
                               apply_event();
    thread = chat_state_store_find_channel(store, 15);
    expect(parent_hidden && thread && !chat_state_store_channel_visible(store, *thread) &&
               store.selected_channel_id == 0,
           "channel_permissions_parent_update_hides_inherited_thread");

    const bool owner_update = route_json(utf8_literal(
                                  R"({"op":0,"d":{"id":"100","owner_id":"1","name":"Permissions","roles":[{"id":"100","permissions":"0"},{"id":"2000","permissions":"0"},{"id":"3000","permissions":"0"}]},"s":7,"t":"GUILD_UPDATE"})")) &&
                              apply_event();
    member_deny = chat_state_store_find_channel(store, 13);
    expect(owner_update && member_deny && chat_state_store_channel_visible(store, *member_deny) &&
               store.selected_channel_id == 11,
           "channel_permissions_owner_bypasses_overwrites");
}

namespace {

void test_pin_permission_overwrites() noexcept {
    GuildRecord guilds[1]{};
    ChannelRecord channels[2]{};
    RoleRecord roles[2]{};
    PermissionOverwriteRecord overwrites[3]{};
    u16 guild_order[1]{}, channel_order[2]{};
    u8 names[256]{};
    ChatStateStore state{};
    const u64 member_roles[]{55};
    const bool ready = chat_state_store_initialize(&state, {guilds, 1}, {guild_order, 1},
        {channels, 2}, {channel_order, 2}, {names, 256}) &&
        chat_state_store_bind_permissions(&state, {roles, 2}, {overwrites, 3}) &&
        chat_state_store_upsert_guild(&state, {100, utf8_literal("Pins"), GuildMutationKind::create, 999}).ok() &&
        chat_state_store_upsert_channel(&state, {7, 100, 0, utf8_literal("Parent"), 0, 0, 0}).ok() &&
        chat_state_store_upsert_channel(&state, {8, 100, 7, utf8_literal("Thread"), 1, 0, 11}).ok() &&
        chat_state_store_upsert_role(&state, {100, 100,
            kPermissionViewChannel | kPermissionReadMessageHistory, utf8_literal("@everyone")}) &&
        chat_state_store_upsert_role(&state, {55, 100, kPermissionPinMessages, utf8_literal("Pinner")}) &&
        chat_state_store_set_self_roles(&state, 100, 999, 42, {member_roles, 1});
    auto can_pin = [&](u64 channel_id) noexcept {
        const auto* channel = chat_state_store_find_channel(state, channel_id);
        return channel && channel->permissions_resolved &&
            (channel->actions & channel_action_pin_messages) != 0;
    };
    chat_state_store_recompute_guild_permissions(&state, 100);
    expect(ready && can_pin(7) && can_pin(8),
        "pin_assigned_role_grants_parent_and_inherited_thread_without_send");
    const bool everyone_deny = chat_state_store_append_channel_overwrite(&state, 7,
        {100, 0, kPermissionPinMessages, 0});
    chat_state_store_recompute_guild_permissions(&state, 100);
    expect(everyone_deny && !can_pin(7) && !can_pin(8),
        "pin_everyone_overwrite_deny_removes_parent_and_thread_action");
    const bool role_allow = chat_state_store_append_channel_overwrite(&state, 7,
        {55, kPermissionPinMessages, 0, 0});
    chat_state_store_recompute_guild_permissions(&state, 100);
    expect(role_allow && can_pin(7) && can_pin(8),
        "pin_assigned_role_allow_overrides_everyone_deny");
    const bool member_deny = chat_state_store_append_channel_overwrite(&state, 7,
        {42, 0, kPermissionPinMessages, 1});
    chat_state_store_recompute_guild_permissions(&state, 100);
    expect(member_deny && !can_pin(7) && !can_pin(8),
        "pin_member_deny_overrides_role_allow_and_reaches_thread");
    const bool administrator = chat_state_store_upsert_role(&state,
        {55, 100, kPermissionAdministrator, utf8_literal("Pinner")});
    chat_state_store_recompute_guild_permissions(&state, 100);
    expect(administrator && can_pin(7) && can_pin(8),
        "pin_administrator_bypasses_pin_overwrite_denies");
    const bool owner = chat_state_store_upsert_role(&state,
        {55, 100, 0, utf8_literal("Pinner")}) &&
        chat_state_store_set_self_roles(&state, 100, 42, 42, {member_roles, 1});
    chat_state_store_recompute_guild_permissions(&state, 100);
    expect(owner && can_pin(7) && can_pin(8),
        "pin_owner_bypasses_denies_without_a_pin_role");
}

} // namespace

void test_activity_and_actions() noexcept {
    test_pin_permission_overwrites();
    constexpr u64 view = kPermissionViewChannel;
    constexpr u64 send_dependents = kPermissionEmbedLinks | kPermissionAttachFiles |
                                    kPermissionMentionEveryone | kPermissionSendVoiceMessages;
    const u32 unresolved = channel_actions(0, ~0ull, false);
    const u32 hidden = channel_actions(0, kPermissionSendMessages, true);
    const u32 send_denied =
        channel_actions(0, view | send_dependents | kPermissionAddReactions, true);
    const u32 send_allowed =
        channel_actions(0, view | kPermissionSendMessages | send_dependents, true);
    const u32 thread_parent_send = channel_actions(11, view | kPermissionSendMessages, true);
    const u32 thread_send =
        channel_actions(11, view | kPermissionSendMessagesInThreads | send_dependents, true);
    const u32 voice_denied = channel_actions(
        2, view | kPermissionSpeak | kPermissionStream | kPermissionMoveMembers, true);
    const u32 voice_allowed =
        channel_actions(2,
                                view | kPermissionConnect | kPermissionSpeak |
                                    kPermissionStream | kPermissionMoveMembers,
                                true);
    const u32 administrator = channel_actions(0, ~0ull, true);
    expect(
        unresolved == 0 && hidden == 0 && (send_denied & channel_action_add_reactions) != 0 &&
            (send_denied & (channel_action_send_messages | channel_action_embed_links |
                            channel_action_attach_files | channel_action_mention_everyone |
                            channel_action_send_voice_messages)) == 0 &&
            (send_allowed & channel_action_send_messages) != 0 &&
            (send_allowed & channel_action_embed_links) != 0 &&
            (send_allowed & channel_action_attach_files) != 0 &&
            (send_allowed & channel_action_mention_everyone) != 0 &&
            (send_allowed & channel_action_send_voice_messages) != 0,
        "channel_actions_view_and_send_implicit_gates");
    expect((thread_parent_send & channel_action_send_messages) == 0 &&
               (thread_send & channel_action_send_messages) != 0,
           "channel_actions_thread_send_permission");
    expect((voice_denied & (channel_action_connect | channel_action_speak |
                            channel_action_stream | channel_action_move_members)) == 0 &&
               (voice_allowed & channel_action_connect) != 0 &&
               (voice_allowed & channel_action_speak) != 0 &&
               (voice_allowed & channel_action_stream) != 0 &&
               (voice_allowed & channel_action_move_members) != 0,
           "channel_actions_voice_connect_implicit_gate");
    expect(administrator == (channel_action_pin_messages << 1) - 1u,
           "channel_actions_owner_administrator_full_mask");
    const u32 pin_only = channel_actions(0, view | kPermissionPinMessages, true);
    const u32 manage_only = channel_actions(0, view | kPermissionManageMessages, true);
    expect((pin_only & channel_action_pin_messages) != 0 &&
        (pin_only & (channel_action_manage_messages | channel_action_send_messages)) == 0 &&
        (manage_only & channel_action_manage_messages) != 0 &&
        (manage_only & channel_action_pin_messages) == 0 &&
        channel_actions(0, kPermissionPinMessages, true) == 0 &&
        channel_actions(0, view | kPermissionPinMessages, false) == 0,
        "channel_actions_pin_bit51_is_independent_from_manage_send_and_requires_resolved_view");
    expect((channel_actions(11, view | kPermissionPinMessages, true) &
        channel_action_pin_messages) != 0,
        "channel_actions_thread_pin_inherits_pin_authority_without_send_in_threads");
    expect((channel_actions(1, 0, true) & channel_action_pin_messages) != 0 &&
        (channel_actions(3, 0, true) & channel_action_pin_messages) != 0 &&
        channel_actions(1, ~0ull, false) == 0 && channel_actions(3, ~0ull, false) == 0,
        "channel_actions_resolved_dm_and_group_participants_can_pin_without_role_permissions");

    GuildRecord guild_records[1]{};
    ChannelRecord channel_records[3]{};
    RoleRecord role_records[3]{};
    PermissionOverwriteRecord overwrite_records[4]{};
    u16 guild_order[1]{};
    u16 channel_order[3]{};
    u8 names[512]{};
    ChatStateStore state{};
    ReadStateStore reads{};
    ReadStateEntry pending_reads[3]{};
    constexpr u64 base_permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
                                     kPermissionSendMessages | kPermissionAddReactions |
                                     kPermissionEmbedLinks | kPermissionAttachFiles;
    const u64 self_roles[] = {55};
    const bool state_ready =
        chat_state_store_initialize(&state, Slice<GuildRecord>{guild_records, 1},
                                       Slice<u16>{guild_order, 1},
                                       Slice<ChannelRecord>{channel_records, 3},
                                       Slice<u16>{channel_order, 3}, Slice<u8>{names, 512}) &&
        read_state_initialize(&reads, {pending_reads, 3}) &&
        read_state_bind(&state, &reads) &&
        chat_state_store_bind_permissions(
            &state, Slice<RoleRecord>{role_records, 3},
            Slice<PermissionOverwriteRecord>{overwrite_records, 4}) &&
        chat_state_store_upsert_guild(
            &state, GuildInput{100, utf8_literal("Activity"), GuildMutationKind::create, 999})
            .ok() &&
        chat_state_store_upsert_channel(
            &state, ChannelInput{7, 100, 0, utf8_literal("selected"), 0, 0, 0, 100, true})
            .ok() &&
        chat_state_store_upsert_channel(
            &state, ChannelInput{8, 100, 0, utf8_literal("activity"), 1, 0, 0, 100, true})
            .ok() &&
        chat_state_store_upsert_channel(
            &state, ChannelInput{9, 100, 0, utf8_literal("snapshot"), 2, 0, 0, 500, true})
            .ok() &&
        chat_state_store_upsert_role(
            &state, RoleInput{100, 100, base_permissions, utf8_literal("@everyone")}) &&
        chat_state_store_upsert_role(&state, RoleInput{55, 100, 0, utf8_literal("Ping")}) &&
        chat_state_store_set_self_roles(&state, 100, 999, 42, ConstSlice<u64>{self_roles, 1});
    chat_state_store_recompute_guild_permissions(&state, 100);
    const auto* no_server_cursor = chat_state_store_find_channel(state, 7);
    expect(state_ready && no_server_cursor && no_server_cursor->last_read_message_id == 7 &&
               !no_server_cursor->read_state_known && chat_state_store_channel_unread(state, *no_server_cursor),
           "activity_missing_server_cursor_uses_channel_baseline");
    const auto server_read = [&](u64 channel_id, u64 cursor) noexcept {
        ReadStateEntry entry{};
        entry.channel_id = channel_id; entry.message_id = cursor;
        entry.fields = read_state_cursor | read_state_mentions;
        return read_state_upsert(&state, entry, 1);
    };
    const bool server_baseline = server_read(7, 100) && server_read(8, 100) && server_read(9, 500);
    const ChatStateSelectionResult selected = chat_state_store_select_channel(&state, 7);
    const ChannelRecord* selected_channel = chat_state_store_find_channel(state, 7);
    const ChannelRecord* snapshot_channel = chat_state_store_find_channel(state, 9);
    const RoleRecord* named_role = chat_state_store_find_role(state, 100, 55);
    expect(state_ready && server_baseline && selected.ok() && selected_channel && snapshot_channel &&
               selected_channel->last_message_id == 100 && selected_channel->last_read_message_id == 100 &&
               !chat_state_store_channel_unread(state, *selected_channel) &&
               snapshot_channel->last_message_id == 500 && snapshot_channel->last_read_message_id == 500 &&
               !chat_state_store_channel_unread(state, *snapshot_channel) && named_role &&
               utf8_equal(chat_state_store_role_name(state, *named_role), utf8_literal("Ping")) &&
               (selected_channel->actions & channel_action_send_messages) != 0,
           "activity_snapshot_baseline_and_actions");

    MessageRecord message_records[8]{};
    u32 message_order[8]{};
    u8 message_text[256]{};
    u32 message_heights[8]{};
    u64 message_fenwick[8]{};
    u8 payload_scratch[kMessageEventPumpPayloadCapacity]{};
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds]{};
    u64 guild_scratch[1]{};
    MessageStore messages{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageEventPump pump{};
    const bool pump_ready =
        message_store_initialize(&messages, Slice<MessageRecord>{message_records, 8},
                                 Slice<u32>{message_order, 8}, Slice<u8>{message_text, 256}) &&
        ui_variable_list_initialize(&list, Slice<u32>{message_heights, 8}, Slice<u64>{message_fenwick, 8},
                                    0, 100) &&
        message_timeline_initialize(&timeline, &messages, &list, 100, 100, true) &&
        message_event_pump_initialize(
            &pump, &timeline, Slice<u8>{payload_scratch, kMessageEventPumpPayloadCapacity},
            Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds}, MessageContentAvailability::available) &&
        message_event_pump_set_state_store(&pump, &state, Slice<u64>{guild_scratch, 1});
    message_event_pump_set_view_active(&pump, true);
    expect(pump_ready, "activity_pump_fixture");

    u8 role_payload[sizeof(u64)]{};
    role_payload[0] = 55;
    const MessageEventPumpConsumeResult role_mention =
        message_event_pump_consume(&pump, activity_message_event(101, 8, 77, 0, 1),
                                   ConstSlice<u8>{role_payload, static_cast<u32>(sizeof(role_payload))});
    role_payload[0] = 66;
    const MessageEventPumpConsumeResult other_role =
        message_event_pump_consume(&pump, activity_message_event(102, 8, 77, 0, 1),
                                   ConstSlice<u8>{role_payload, static_cast<u32>(sizeof(role_payload))});
    const MessageEventPumpConsumeResult everyone = message_event_pump_consume(
        &pump, activity_message_event(103, 8, 77, gateway_client_message_mention_everyone),
        ConstSlice<u8>{nullptr, 0});
    const MessageEventPumpConsumeResult direct = message_event_pump_consume(
        &pump, activity_message_event(104, 8, 77, gateway_client_message_mention_current_user),
        ConstSlice<u8>{nullptr, 0});
    const ChannelRecord* activity = chat_state_store_find_channel(state, 8);
    const GuildRecord* activity_guild = chat_state_store_find_guild(state, 100);
    expect(role_mention.ok() && role_mention.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
               role_mention.repaint && other_role.ok() && everyone.ok() && direct.ok() && activity &&
               chat_state_store_channel_unread(state, *activity) && activity->last_message_id == 104 &&
               activity->mention_count == 3 && activity_guild && activity_guild->unread_channels == 1 &&
               activity_guild->mention_count == 3,
           "activity_nonselected_direct_everyone_role_mentions");

    GuildViewRow guild_rows[1]{};
    ChannelViewRow channel_rows[3]{};
    u16 navigation_scratch[3]{};
    u8 navigation_visibility[3]{};
    ChannelSortKey navigation_keys[3]{};
    NavigationView navigation{};
    const ChannelViewRow* activity_row = nullptr;
    const bool navigation_ready =
        navigation_view_initialize(
            &navigation, Slice<GuildViewRow>{guild_rows, 1},
            Slice<ChannelViewRow>{channel_rows, 3}, Slice<u16>{navigation_scratch, 3},
            Slice<u8>{navigation_visibility, 3}, Slice<ChannelSortKey>{navigation_keys, 3}) &&
        navigation_view_rebuild(&navigation, state).ok();
    if (navigation_ready) {
        activity_row = navigation_view_find_channel(navigation, 8);
    }
    expect(navigation_ready && activity_row && activity_row->unread && activity_row->mention_count == 3 &&
               (activity_row->actions & channel_action_send_messages) != 0 &&
               navigation.guild_rows[0].unread && navigation.guild_rows[0].mention_count == 3,
           "activity_navigation_projection");

    const MessageEventPumpConsumeResult self_authored =
        message_event_pump_consume(&pump, activity_message_event(105, 8, 42), ConstSlice<u8>{nullptr, 0});
    activity = chat_state_store_find_channel(state, 8);
    activity_guild = chat_state_store_find_guild(state, 100);
    expect(self_authored.ok() && activity && !chat_state_store_channel_unread(state, *activity) &&
               activity->mention_count == 0 && activity_guild && activity_guild->unread_channels == 0 &&
               activity_guild->mention_count == 0,
           "activity_self_authored_is_locally_read");

    const MessageEventPumpConsumeResult ordinary =
        message_event_pump_consume(&pump, activity_message_event(106, 8, 77), ConstSlice<u8>{nullptr, 0});
    activity = chat_state_store_find_channel(state, 8);
    activity_guild = chat_state_store_find_guild(state, 100);
    expect(ordinary.ok() && activity && chat_state_store_channel_unread(state, *activity) &&
               activity->mention_count == 0 && activity_guild && activity_guild->unread_channels == 1 &&
               activity_guild->mention_count == 0 &&
               read_state_mark_local(&state, 8, activity->last_message_id, activity->read_last_viewed,
                                     activity->read_flags) &&
               !chat_state_store_channel_unread(state, *activity),
           "activity_unmentioned_and_explicit_read");

    const MessageEventPumpConsumeResult focused =
        message_event_pump_consume(&pump, activity_message_event(201, 7, 77), ConstSlice<u8>{nullptr, 0});
    selected_channel = chat_state_store_find_channel(state, 7);
    expect(focused.ok() && focused.kind == MessageEventPumpConsumeKind::applied && selected_channel &&
               chat_state_store_channel_unread(state, *selected_channel) &&
               selected_channel->last_read_message_id == 100,
           "activity_focused_follow_bottom_defers_read_to_shared_observer");
    expect(read_state_mark_local(&state, 7, 201, 1, 1) && selected_channel &&
               !chat_state_store_channel_unread(state, *selected_channel) &&
               selected_channel->last_read_message_id == 201,
           "activity_admitted_local_read_clears_focused_activity");
    message_event_pump_set_view_active(&pump, false);
    const MessageEventPumpConsumeResult inactive =
        message_event_pump_consume(&pump, activity_message_event(202, 7, 77), ConstSlice<u8>{nullptr, 0});
    selected_channel = chat_state_store_find_channel(state, 7);
    expect(inactive.ok() && selected_channel &&
               chat_state_store_channel_unread(state, *selected_channel) &&
               read_state_mark_local(&state, 7, selected_channel->last_message_id,
                                     selected_channel->read_last_viewed, selected_channel->read_flags),
           "activity_inactive_selected_is_unread");
    message_event_pump_set_view_active(&pump, true);
    timeline.follow_bottom = false;
    const MessageEventPumpConsumeResult scrollback =
        message_event_pump_consume(&pump, activity_message_event(203, 7, 77), ConstSlice<u8>{nullptr, 0});
    selected_channel = chat_state_store_find_channel(state, 7);
    expect(scrollback.ok() && selected_channel &&
               chat_state_store_channel_unread(state, *selected_channel) &&
               read_state_mark_local(&state, 7, selected_channel->last_message_id,
                                     selected_channel->read_last_viewed, selected_channel->read_flags),
           "activity_scrollback_selected_is_unread");

    const ChatStateStoreResult snapshot_advanced = chat_state_store_upsert_channel(
        &state, ChannelInput{9, 100, 0, Utf8View{}, 2, 0, 0, 600, true});
    snapshot_channel = chat_state_store_find_channel(state, 9);
    const bool snapshot_is_unread =
        snapshot_channel && chat_state_store_channel_unread(state, *snapshot_channel);
    const ChatStateStoreResult partial_preserved =
        chat_state_store_upsert_channel(&state, ChannelInput{9, 100, 0, Utf8View{}, 2, 0, 0});
    const bool preserved = snapshot_channel && snapshot_channel->last_message_id == 600 &&
                           snapshot_channel->last_read_message_id == 500;
    const ChatStateStoreResult snapshot_cleared = chat_state_store_upsert_channel(
        &state, ChannelInput{9, 100, 0, Utf8View{}, 2, 0, 0, 0, true});
    expect(snapshot_advanced.ok() && snapshot_is_unread && partial_preserved.ok() && preserved &&
               snapshot_cleared.ok() && snapshot_channel && snapshot_channel->last_message_id == 0 &&
               snapshot_channel->last_read_message_id == 500 && snapshot_channel->mention_count == 0,
           "activity_last_message_partial_and_null_semantics");

    GatewayEnvelope envelope{};
    u32 structural_scratch[512]{};
    char state_payload[512]{};
    u64 route_ids[kGatewayBulkDeleteMaximumIds]{};
    GatewayClientEvent routed{};
    ConstSlice<u8> routed_payload{};
    const bool routed_last_message =
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"9","guild_id":"100","type":0,"last_message_id":"700"},"s":20,"t":"CHANNEL_UPDATE"})"),
            &envelope)
            .ok() &&
        gateway_client_route_dispatch(
            envelope, Slice<u32>{structural_scratch, 512}, 16, Slice<char>{state_payload, 512},
            Slice<u64>{route_ids, kGatewayBulkDeleteMaximumIds}, 42, &routed, &routed_payload, gateway_test_message_scratch())
            .ok() &&
        (routed.state.fields & gateway_client_state_field_last_message_id) != 0 &&
        routed.state.last_message_id == 700 &&
        chat_state_apply_gateway_event(&state, routed, routed_payload, Slice<u64>{guild_scratch, 1})
            .ok();
    snapshot_channel = chat_state_store_find_channel(state, 9);
    expect(routed_last_message && snapshot_channel && snapshot_channel->last_message_id == 700 &&
               chat_state_store_channel_unread(state, *snapshot_channel),
           "activity_gateway_last_message_route");
    const bool routed_null =
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"9","guild_id":"100","type":0,"last_message_id":null},"s":21,"t":"CHANNEL_UPDATE"})"),
            &envelope)
            .ok() &&
        gateway_client_route_dispatch(
            envelope, Slice<u32>{structural_scratch, 512}, 16, Slice<char>{state_payload, 512},
            Slice<u64>{route_ids, kGatewayBulkDeleteMaximumIds}, 42, &routed, &routed_payload, gateway_test_message_scratch())
            .ok() &&
        (routed.state.fields & gateway_client_state_field_last_message_id) != 0 &&
        routed.state.last_message_id == 0 &&
        chat_state_apply_gateway_event(&state, routed, routed_payload, Slice<u64>{guild_scratch, 1})
            .ok();
    snapshot_channel = chat_state_store_find_channel(state, 9);
    expect(routed_null && snapshot_channel && snapshot_channel->last_message_id == 0 &&
               snapshot_channel->last_read_message_id == 500,
           "activity_gateway_last_message_null");
}

void test_gateway_message_events() noexcept {
    u64 timestamp = 0;
    expect(parse_iso_timestamp(JsonString{utf8_literal("2024-08-30T12:34:56.789123+02:30"), 0, false},
                               &timestamp) &&
               timestamp == 1'725'012'296'789ull,
           "gateway_message_timestamp_offset_fraction");
    expect(parse_iso_timestamp(JsonString{utf8_literal("2024-02-29T23:59:59.999999-01:00"), 0, false},
                               &timestamp) &&
               timestamp == 1'709'254'799'999ull &&
               !parse_iso_timestamp(JsonString{utf8_literal("2023-02-29T00:00:00Z"), 0, false},
                                    &timestamp) &&
               !parse_iso_timestamp(JsonString{utf8_literal("2024-01-01T00:00:60Z"), 0, false},
                                    &timestamp),
           "gateway_message_timestamp_calendar_validation");

    u64 decimal = 0;
    expect(json_string_to_u64(JsonString{utf8_literal("12\\u0033"), 0, true}, &decimal) && decimal == 123 &&
               !json_string_to_u64(JsonString{utf8_literal("18446744073709551616"), 0, false}, &decimal) &&
               !json_string_to_u64(JsonString{utf8_literal("12x"), 0, false}, &decimal),
           "json_string_u64_decoded_overflow");

    GatewayEnvelope envelope{};
    u32 scratch[256]{};
    char content[64]{};
    GatewayMessageEvent message{};
    const GatewayDecodeResult create_envelope = decode_gateway(
        utf8_literal(
            R"({"op":0,"d":{"id":"1270000000000000001","channel_id":"1260000000000000001","author":{"id":"1250000000000000001","username":"x","avatar":"0123456789abcdef0123456789abcdef"},"content":"hello €\n","timestamp":"2024-08-30T12:34:56.789123+02:30","edited_timestamp":null,"flags":4,"nonce":"987654321012345678","unknown":{"nested":true}},"s":9,"t":"MESSAGE_CREATE"})"),
        &envelope);
    const GatewayEventDecodeResult created = gateway_decode_message_create(
        envelope.data, Slice<u32>{scratch, 256}, 16, Slice<char>{content, 64}, &message);
    constexpr u32 all_fields = gateway_message_field_id | gateway_message_field_channel_id |
                               gateway_message_field_author_id | gateway_message_field_content |
                               gateway_message_field_timestamp | gateway_message_field_edited_timestamp |
                               gateway_message_field_flags | gateway_message_field_nonce |
                               gateway_message_field_author_policy | gateway_message_field_interaction_metadata;
    expect(
        create_envelope.ok() && created.ok() && message.id == 1'270'000'000'000'000'001ull &&
            message.channel_id == 1'260'000'000'000'000'001ull &&
            message.author_id == 1'250'000'000'000'000'001ull &&
            message.timestamp_ms == 1'725'012'296'789ull && message.edited_timestamp_ms == 0 &&
            message.flags == 4 && message.nonce == 987'654'321'012'345'678ull &&
            message.fields == all_fields && message.content.bytes == 10 &&
            utf8_validate(message.content).valid && message.content.data[6] == '\xE2' &&
            message.content.data[9] == '\n' &&
            utf8_equal(message.author_avatar_hash, utf8_literal("0123456789abcdef0123456789abcdef")),
        "gateway_message_create_typed_decode");
    expect(created.ok()&&message.type==0&&message.author_public_flags==0&&!message.author_bot&&
        !message.activity_instance_present&&!message.application_present&&!message.changelog_present&&
        !message.interaction_metadata_present&&
        (message.fields&(gateway_message_field_author_policy|gateway_message_field_interaction_metadata))==
            (gateway_message_field_author_policy|gateway_message_field_interaction_metadata)&&
        !(message.fields&(gateway_message_field_type|gateway_message_field_activity_instance|
            gateway_message_field_application_id|gateway_message_field_changelog_id)),
        "gateway_message_create_absent_optional_metadata_preserves_presence_and_full_author_defaults");
    GatewayEnvelope default_envelope{};GatewayMessageEvent default_message{};char default_content[8]{};
    const bool default_decoded=decode_gateway(utf8_literal(
        R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"","timestamp":"2024-08-30T12:34:56Z","type":0,"activity_instance":null,"application_id":null,"changelog_id":null},"t":"MESSAGE_CREATE"})"),
        &default_envelope).ok()&&gateway_decode_message_create(default_envelope.data,{scratch,256},16,{default_content,8},&default_message).ok();
    constexpr u32 policy_fields=gateway_message_field_type|gateway_message_field_author_policy|
        gateway_message_field_activity_instance|gateway_message_field_application_id|
        gateway_message_field_changelog_id|gateway_message_field_interaction_metadata;
    expect(default_decoded&&(default_message.fields&policy_fields)==policy_fields&&default_message.type==0&&
        default_message.author_public_flags==0&&!default_message.author_bot&&!default_message.activity_instance_present&&
        !default_message.application_present&&!default_message.changelog_present&&!default_message.interaction_metadata_present,
        "gateway_message_create_explicit_null_policy_fields_are_known_false");

    char routed_content[64]{};
    u64 routed_ids[kGatewayBulkDeleteMaximumIds]{};
    GatewayClientEvent routed_event{};
    ConstSlice<u8> routed_payload{};
    const GatewayClientDispatchRouteResult routed_create = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 256}, 16, Slice<char>{routed_content, 64},
        Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch());
    GatewayMessageEvent routed_message{};
    Utf8View routed_author_name{};
    Utf8View routed_avatar_hash{};
    expect(
        routed_create.ok() && routed_event.dispatch_kind == GatewayClientDispatchKind::message_create &&
            routed_event.sequence == 9 && routed_event.has_sequence &&
            routed_event.dispatch_payload_bytes == 43 && routed_payload.count == 43 &&
            routed_event.message.transient_bytes == 43 && routed_event.message.author_name_bytes == 1 &&
            routed_event.message.author_avatar_hash_bytes == 32 &&
            gateway_client_event_as_message(routed_event, routed_payload, &routed_message) &&
            gateway_client_event_message_author_name(routed_event, routed_payload, &routed_author_name) &&
            gateway_client_event_message_author_avatar_hash(routed_event, routed_payload,
                                                            &routed_avatar_hash) &&
            routed_message.id == message.id && routed_message.author_id == message.author_id &&
            routed_message.nonce == message.nonce && routed_event.message.nonce == message.nonce &&
            routed_message.content.bytes == 10 && routed_message.content.data[9] == '\n' &&
            routed_author_name.bytes == 1 && routed_author_name.data[0] == 'x' &&
            utf8_equal(routed_avatar_hash, utf8_literal("0123456789abcdef0123456789abcdef")) &&
            utf8_equal(routed_message.author_avatar_hash, routed_avatar_hash),
        "gateway_client_route_message_create_owned_payload");

    const GatewayClientDispatchRouteResult aliased_route = gateway_client_route_dispatch(
        envelope, Slice<u32>{scratch, 256}, 16,
        Slice<char>{const_cast<char*>(envelope.data.source.data), envelope.data.source.bytes},
        Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch());
    expect(!aliased_route.ok() && aliased_route.error == GatewayClientDispatchRouteError::invalid_input,
           "gateway_client_route_rejects_source_output_aliasing");

    u32 tiny_route_scratch[1]{};
    const GatewayClientDispatchRouteResult overflow_route = gateway_client_route_dispatch(
        envelope, Slice<u32>{tiny_route_scratch, 1}, 16, Slice<char>{routed_content, 64},
        Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch());
    expect(!overflow_route.ok() && overflow_route.error == GatewayClientDispatchRouteError::decode &&
               overflow_route.event_error == GatewayEventDecodeError::scan_failed &&
               overflow_route.scan_error == JsonScanError::output_overflow &&
               overflow_route.error_offset != 0,
           "gateway_client_route_preserves_message_decode_diagnostics");

    GatewayEnvelope reaction_envelope{};
    GatewayMessageReactionEvent routed_reaction{};
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123","name":"wave","animated":true},"type":1,"burst":true},"s":10,"t":"MESSAGE_REACTION_ADD"})"),
            &reaction_envelope)
                .ok() &&
            gateway_client_route_dispatch(
                reaction_envelope, Slice<u32>{scratch, 256}, 16, Slice<char>{routed_content, 64},
                Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                .ok() &&
            routed_event.dispatch_kind == GatewayClientDispatchKind::message_reaction_add &&
            routed_event.dispatch_payload_bytes == 4 &&
            gateway_client_event_as_message_reaction(routed_event, routed_payload, &routed_reaction) &&
            routed_reaction.message_id == 77 && routed_reaction.channel_id == 42 &&
            routed_reaction.user_id == 9 && routed_reaction.emoji_id == 123 && routed_reaction.animated &&
            routed_reaction.type == MessageReactionType::burst &&
            utf8_equal(routed_reaction.name, utf8_literal("wave")),
        "gateway_client_routes_typed_reaction_add_with_owned_name");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"message_id":"77","channel_id":"42"},"s":11,"t":"MESSAGE_REACTION_REMOVE_ALL"})"),
            &reaction_envelope)
                .ok() &&
            gateway_client_route_dispatch(
                reaction_envelope, Slice<u32>{scratch, 256}, 16, Slice<char>{routed_content, 64},
                Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                .ok() &&
            routed_event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove_all &&
            routed_payload.count == 0 &&
            gateway_client_event_as_message_reaction(routed_event, routed_payload, &routed_reaction) &&
            routed_reaction.kind == GatewayMessageReactionEventKind::remove_all &&
            routed_reaction.name.bytes == 0,
        "gateway_client_routes_reaction_remove_all_without_emoji_payload");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"message_id":"77","channel_id":"42","emoji":{"id":null,"name":"👍"}},"s":12,"t":"MESSAGE_REACTION_REMOVE"})"),
            &reaction_envelope)
                .ok() &&
            gateway_client_route_dispatch(
                reaction_envelope, Slice<u32>{scratch, 256}, 16, Slice<char>{routed_content, 64},
                Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                    .event_error == GatewayEventDecodeError::missing_required_field,
        "gateway_client_rejects_reaction_remove_without_user");

    constexpr Utf8View deleted_custom_reactions[] = {
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123","name":null,"animated":true},"type":0},"s":14,"t":"MESSAGE_REACTION_ADD"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"name":null,"id":"123"},"type":1,"burst":true},"s":15,"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","emoji":{"id":"123","name":null}},"s":16,"t":"MESSAGE_REACTION_REMOVE_EMOJI"})"),
    };
    for (u32 index = 0; index < 3; ++index) {
        routed_reaction.name = utf8_literal("stale name");
        expect(decode_gateway(deleted_custom_reactions[index], &reaction_envelope).ok() &&
            gateway_client_route_dispatch(reaction_envelope, {scratch, 256}, 16,
                {routed_content, 64}, {routed_ids, kGatewayBulkDeleteMaximumIds},
                &routed_event, &routed_payload, gateway_test_message_scratch()).ok() &&
            routed_payload.count == 0 && routed_event.reaction.name_bytes == 0 &&
            gateway_client_event_as_message_reaction(routed_event, routed_payload, &routed_reaction) &&
            routed_reaction.emoji_id == 123 && routed_reaction.name.bytes == 0 &&
            routed_reaction.message_id == 77 && routed_reaction.channel_id == 42 &&
            routed_reaction.kind == (index == 0 ? GatewayMessageReactionEventKind::add :
                index == 1 ? GatewayMessageReactionEventKind::remove :
                             GatewayMessageReactionEventKind::remove_emoji) &&
            routed_reaction.type == (index == 1 ? MessageReactionType::burst :
                                                 MessageReactionType::normal),
            "gateway_deleted_custom_reaction_null_name_preserves_id_through_dispatch");
    }
    GatewayClientEvent deleted_custom_packed = routed_event;
    deleted_custom_packed.reaction.emoji_id = 0;
    expect(!gateway_client_event_as_message_reaction(deleted_custom_packed, {}, &routed_reaction),
        "gateway_packed_unnamed_unicode_reaction_rejected");
    deleted_custom_packed = routed_event;
    deleted_custom_packed.reaction.type = 3;
    expect(!gateway_client_event_as_message_reaction(deleted_custom_packed, {}, &routed_reaction),
        "gateway_packed_unnamed_custom_invalid_type_rejected");
    deleted_custom_packed = routed_event;
    deleted_custom_packed.reaction.flags = 2;
    expect(!gateway_client_event_as_message_reaction(deleted_custom_packed, {}, &routed_reaction),
        "gateway_packed_unnamed_custom_invalid_flags_rejected");
    constexpr Utf8View invalid_deleted_custom_reactions[] = {
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":null,"name":null}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":null,"name":""}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"name":null}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"0","name":null}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"bad","name":null}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123"}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123","name":""}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123","name":false}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123","name":null,"name":"wave"}},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","user_id":"9","emoji":{"id":"123","name":null},"type":3},"t":"MESSAGE_REACTION_REMOVE"})"),
        utf8_literal(R"({"op":0,"d":{"message_id":"77","channel_id":"42","emoji":{"id":"123","name":null}},"t":"MESSAGE_REACTION_REMOVE"})"),
    };
    for (Utf8View invalid : invalid_deleted_custom_reactions) {
        expect(decode_gateway(invalid, &reaction_envelope).ok() &&
            !gateway_client_route_dispatch(reaction_envelope, {scratch, 256}, 16,
                {routed_content, 64}, {routed_ids, kGatewayBulkDeleteMaximumIds},
                &routed_event, &routed_payload, gateway_test_message_scratch()).ok(),
            "gateway_deleted_custom_null_name_does_not_weaken_identity_type_or_required_fields");
    }

    GatewayTypingStartEvent routed_typing{};
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"channel_id":"42","guild_id":"7","user_id":"9","timestamp":1725000000},"s":13,"t":"TYPING_START"})"),
            &reaction_envelope)
                .ok() &&
            gateway_client_route_dispatch(
                reaction_envelope, Slice<u32>{scratch, 256}, 16, Slice<char>{routed_content, 64},
                Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                .ok() &&
            routed_event.dispatch_kind == GatewayClientDispatchKind::typing_start &&
            routed_payload.count == 0 &&
            gateway_client_event_as_typing_start(routed_event, routed_payload, &routed_typing) &&
            routed_typing.channel_id == 42 && routed_typing.guild_id == 7 && routed_typing.user_id == 9 &&
            routed_typing.timestamp_seconds == 1'725'000'000,
        "gateway_client_routes_typed_typing_start_without_transient_payload");

    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"x","timestamp":"2024-01-01T00:00:00Z","nonce":42},"s":1,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 64}, &message)
                .ok() &&
            message.nonce == 42 && (message.fields & gateway_message_field_nonce) != 0 &&
            decode_gateway(
                utf8_literal(
                    R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"x","timestamp":"2024-01-01T00:00:00Z","nonce":null},"s":1,"t":"MESSAGE_CREATE"})"),
                &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 64}, &message)
                .ok() &&
            message.nonce == 0 && (message.fields & gateway_message_field_nonce) != 0,
        "gateway_message_numeric_and_null_nonce_decode");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"x","timestamp":"2024-01-01T00:00:00Z","nonce":true},"s":1,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 64}, &message)
                    .error == GatewayEventDecodeError::invalid_message_nonce,
        "gateway_message_invalid_nonce_rejected");

    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds]{};
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1270000000000000002","channel_id":"1260000000000000001","author":{"id":"1250000000000000001"},"content":"ping","timestamp":"2024-08-30T12:34:56Z","mention_everyone":true,"mentions":[{"id":"42","username":"self"},{"id":"77","username":"other"}],"mention_roles":["55","66"]},"s":10,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(
                envelope.data, Slice<u32>{scratch, 256}, 16, Slice<char>{content, 64},
                Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds}, Slice<GatewayMessageNamedUser>{},
                42, &message)
                .ok() &&
            message.mention_everyone && message.mentions_current_user && message.mention_roles.count == 2 &&
            message.mention_roles[0] == 55 && message.mention_roles[1] == 66,
        "gateway_message_mentions_typed_decode");
    u64 routed_role_0 = 0;
    u64 routed_role_1 = 0;
    u64 routed_user_0 = 0;
    u64 routed_user_1 = 0;
    Utf8View routed_user_name_0{};
    Utf8View routed_user_name_1{};
    expect(
        gateway_client_route_dispatch(
            envelope, Slice<u32>{scratch, 256}, 16, Slice<char>{routed_content, 64},
            Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, 42, &routed_event, &routed_payload, gateway_test_message_scratch())
                .ok() &&
            routed_event.dispatch_kind == GatewayClientDispatchKind::message_create &&
            routed_event.message.content_bytes == 4 && routed_event.message.transient_bytes == 13 &&
            routed_event.message.mention_role_count == 2 && routed_event.message.mention_user_count == 2 &&
            routed_payload.count == 61 &&
            (routed_event.message.mention_flags & gateway_client_message_mention_everyone) != 0 &&
            (routed_event.message.mention_flags & gateway_client_message_mention_current_user) != 0 &&
            gateway_client_event_as_message(routed_event, routed_payload, &routed_message) &&
            routed_message.content.bytes == 4 &&
            gateway_client_event_message_mention_role_at(routed_event, routed_payload, 0, &routed_role_0) &&
            gateway_client_event_message_mention_role_at(routed_event, routed_payload, 1, &routed_role_1) &&
            gateway_client_event_message_mention_user_at(routed_event, routed_payload, 0, &routed_user_0,
                                                         &routed_user_name_0) &&
            gateway_client_event_message_mention_user_at(routed_event, routed_payload, 1, &routed_user_1,
                                                         &routed_user_name_1) &&
            routed_role_0 == 55 && routed_role_1 == 66 && routed_user_0 == 42 &&
            routed_user_name_0.bytes == 4 && routed_user_1 == 77 && routed_user_name_1.bytes == 5,
        "gateway_client_route_mentions_owned_payload");
    expect(gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 16,
                                         Slice<char>{content, 64}, &message)
                   .ok() &&
               message.mention_roles.count == 0,
           "gateway_message_legacy_decoder_discards_role_storage_safely");

    GatewayEnvelope duplicate_mentions{};
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"x","timestamp":"2024-01-01T00:00:00Z","mention_roles":["55","55"]},"s":1,"t":"MESSAGE_CREATE"})"),
            &duplicate_mentions)
                .ok() &&
            gateway_decode_message_create(
                duplicate_mentions.data, Slice<u32>{scratch, 256}, 16, Slice<char>{content, 64},
                Slice<u64>{mention_roles, kGatewayMessageMentionRoleMaximumIds}, Slice<GatewayMessageNamedUser>{},
                42, &message)
                    .error == GatewayEventDecodeError::duplicate_field,
        "gateway_message_duplicate_mention_role_rejected");
    GatewayMessageNamedUser kept_user[1]{};
    expect(gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 16,
                                         Slice<char>{content, 64}, Slice<u64>{mention_roles, 1},
                                         Slice<GatewayMessageNamedUser>{kept_user, 1}, 77, &message)
                   .ok() &&
               message.mention_roles.count == 1 && message.mention_roles[0] == 55 &&
               message.mention_users.count == 1 && message.mention_users[0].id == 42 &&
               message.mentions_current_user,
           "gateway_message_mentions_past_capacity_truncate_and_still_mention_self");

    expect(decode_gateway(
               utf8_literal(
                   R"({"op":0,"d":{"id":"1","channel_id":"2","content":""},"s":10,"t":"MESSAGE_UPDATE"})"),
               &envelope)
                   .ok() &&
               gateway_decode_message_update(envelope.data, Slice<u32>{scratch, 256}, 8,
                                             Slice<char>{nullptr, 0}, &message)
                   .ok() &&
               message.fields == (gateway_message_field_id | gateway_message_field_channel_id |
                                  gateway_message_field_content) &&
               message.content.bytes == 0 && message.author_id == 0 && message.timestamp_ms == 0,
           "gateway_message_update_absent_vs_empty");
    expect(gateway_client_route_dispatch(
               envelope, Slice<u32>{scratch, 256}, 8, Slice<char>{routed_content, 64},
               Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                   .ok() &&
               routed_event.dispatch_kind == GatewayClientDispatchKind::message_update &&
               routed_event.dispatch_payload_bytes == 0 &&
               gateway_client_event_as_message(routed_event, routed_payload, &routed_message) &&
               (routed_message.fields & gateway_message_field_content) != 0 &&
               routed_message.content.bytes == 0,
           "gateway_client_route_empty_update_payload");

    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"abcd","timestamp":"2024-01-01T00:00:00Z"},"s":1,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 3}, &message)
                    .error == GatewayEventDecodeError::message_content_capacity,
        "gateway_message_content_capacity");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":1,"channel_id":"2","author":{"id":"3"},"content":"x","timestamp":"2024-01-01T00:00:00Z"},"s":1,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 64}, &message)
                    .error == GatewayEventDecodeError::invalid_message_id,
        "gateway_message_id_string_required");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1","channel_id":"2","author":{},"content":"x","timestamp":"2024-01-01T00:00:00Z"},"s":1,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 64}, &message)
                    .error == GatewayEventDecodeError::invalid_author_id,
        "gateway_message_author_id_required");
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"1","channel_id":"2","author":{"id":"3"},"content":"x","timestamp":"2023-02-29T00:00:00Z"},"s":1,"t":"MESSAGE_CREATE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                          Slice<char>{content, 64}, &message)
                    .error == GatewayEventDecodeError::invalid_message_timestamp,
        "gateway_message_timestamp_reject");
    expect(decode_gateway(
               utf8_literal(R"({"op":0,"d":{"id":"1","channel_id":"2"},"s":1,"t":"MESSAGE_CREATE"})"),
               &envelope)
                   .ok() &&
               gateway_decode_message_create(envelope.data, Slice<u32>{scratch, 256}, 8,
                                             Slice<char>{content, 64}, &message)
                       .error == GatewayEventDecodeError::missing_required_field,
           "gateway_message_create_required_fields");

    u32 rich_structurals[2'048]{};
    char rich_text[32 * 1024]{};
    u64 rich_roles[kGatewayMessageMentionRoleMaximumIds]{};
    u64 rich_member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    GatewayMessageNamedUser rich_users[kGatewayMessageMentionUserMaximum]{};
    GatewayMessageReaction rich_reactions[kGatewayMessageReactionMaximum]{};
    GatewayMessageAttachment rich_attachments[kGatewayMessageAttachmentMaximum]{};
    GatewayMessageEmbed rich_embeds[kGatewayMessageEmbedMaximum]{};
    GatewayMessageEmbedField rich_embed_fields[kGatewayMessageEmbedFieldMaximum]{};
    GatewayMessageSticker rich_stickers[kGatewayMessageStickerMaximum]{};
    GatewayMessageComponent rich_components[kGatewayMessageComponentMaximum]{};
    const GatewayMessageRichScratch rich_scratch{
        Slice<GatewayMessageReaction>{rich_reactions, kGatewayMessageReactionMaximum},
        Slice<GatewayMessageAttachment>{rich_attachments, kGatewayMessageAttachmentMaximum},
        Slice<GatewayMessageEmbed>{rich_embeds, kGatewayMessageEmbedMaximum},
        Slice<GatewayMessageEmbedField>{rich_embed_fields, kGatewayMessageEmbedFieldMaximum},
        Slice<GatewayMessageSticker>{rich_stickers, kGatewayMessageStickerMaximum},
        Slice<GatewayMessageComponent>{rich_components, kGatewayMessageComponentMaximum},
    };
    const Utf8View rich_payload = utf8_literal(
        R"({"op":0,"d":{"id":"900","channel_id":"7","author":{"id":"11","username":"Writer"},"member":{"nick":"Ink","roles":["99","100"]},"mentions":[{"id":"13","username":"Pal"}],"content":"rich","timestamp":"2024-08-30T12:34:56Z","message_reference":{"type":0,"message_id":"800","channel_id":"7","guild_id":"5"},"referenced_message":{"id":"800","channel_id":"7","author":{"id":"12","username":"Source","global_name":"Source Display","avatar":"fedcba9876543210fedcba9876543210"},"content":"the referenced text"},"reactions":[{"count":4,"count_details":{"normal":3,"burst":1},"me":true,"me_burst":false,"emoji":{"id":"44","name":"party","animated":true}},{"count":1,"me":false,"emoji":{"id":null,"name":"🔥"}}],"attachments":[{"id":"70","filename":"clip.mp4","title":"Clip","description":"alt text","content_type":"video/mp4","size":12345,"url":"https://cdn.discordapp.com/attachments/7/70/clip.mp4","width":1280,"height":720,"flags":1,"ephemeral":false}],"embeds":[{"title":"Build","type":"rich","description":"green","url":"https://example.com/build","color":65280,"author":{"name":"CI"},"footer":{"text":"done"},"image":{"url":"https://example.com/original.png","proxy_url":"https://media.discordapp.net/external/fixture/image.png","content_type":"image/png","description":"preview alt","width":640,"height":360,"flags":32},"fields":[{"name":"Target","value":"Release","inline":true}]}],"sticker_items":[{"id":"88","name":"Wave","format_type":2}],"components":[{"type":17,"id":1,"components":[{"type":10,"id":2,"content":"Component markdown"},{"type":1,"id":3,"components":[{"type":2,"id":4,"style":5,"label":"Open","url":"https://example.com"}]}]}]},"s":90,"t":"MESSAGE_CREATE"})");
    const bool rich_envelope_decoded =
        gateway_decode_envelope(rich_payload, Slice<u32>{rich_structurals, 2'048}, 32, &envelope).ok();
    const GatewayEventDecodeResult rich_decoded =
        rich_envelope_decoded
            ? gateway_decode_message_create(
                  envelope.data, Slice<u32>{rich_structurals, 2'048}, 32, Slice<char>{rich_text, 32 * 1024},
                  Slice<u64>{rich_roles, kGatewayMessageMentionRoleMaximumIds},
                  Slice<u64>{rich_member_roles, kGatewayMessageMemberRoleMaximumIds},
                  Slice<GatewayMessageNamedUser>{rich_users, kGatewayMessageMentionUserMaximum},
                  rich_scratch, 11, &message)
            : GatewayEventDecodeResult{};
    expect(rich_decoded.ok() && message.reference.state == GatewayMessageReferenceState::available &&
               message.reference.message_id == 800 && message.reference.author_id == 12 &&
               utf8_equal(message.reference.author_name, utf8_literal("Source Display")) &&
               utf8_equal(message.reference.author_avatar_hash,
                          utf8_literal("fedcba9876543210fedcba9876543210")) &&
               utf8_equal(message.reference.content, utf8_literal("the referenced text")) &&
               message.reactions.count == 2 && message.reactions[0].emoji_id == 44 &&
               message.reactions[0].normal_count == 3 && message.reactions[0].burst_count == 1 &&
               message.reactions[0].me && message.reactions[0].animated && message.attachments.count == 1 &&
               message.attachments[0].size == 12'345 && message.attachments[0].width == 1'280 &&
               message.attachments[0].height == 720 &&
               utf8_equal(message.attachments[0].filename, utf8_literal("clip.mp4")) &&
               message.embeds.count == 1 && message.embeds[0].type == GatewayMessageEmbedType::rich &&
               message.embeds[0].field_count == 1 && message.embed_fields.count == 1 &&
               message.embeds[0].image.width == 640 && message.embeds[0].image.height == 360 &&
               message.embeds[0].image.flags == 32 && message.embeds[0].image.has_width &&
               message.embeds[0].image.has_height &&
               utf8_equal(message.embeds[0].image.proxy_url,
                          utf8_literal("https://media.discordapp.net/external/fixture/image.png")) &&
               utf8_equal(message.embeds[0].image.description, utf8_literal("preview alt")) &&
               utf8_equal(message.embed_fields[0].value, utf8_literal("Release")) &&
               message.embed_fields[0].inline_field && message.stickers.count == 1 &&
               message.stickers[0].format_type == 2 && message.components.count == 4 &&
               message.components[0].parent == 0xFFFFu && message.components[1].parent == 0 &&
               message.components[2].parent == 0 && message.components[3].parent == 2 &&
               message.rich_truncations == 0 && message.member_roles.count == 2 &&
               message.member_roles[0] == 99 && message.member_roles[1] == 100 &&
               (message.fields & gateway_message_field_member) != 0,
           "gateway_message_rich_resource_fields");
    expect(rich_decoded.ok() && utf8_equal(message.member_nick, utf8_literal("Ink")) &&
               message.member_nick.data == message.author_name.data + message.author_name.bytes &&
               message.mention_users.count == 1 &&
               utf8_equal(message.mention_users[0].name, utf8_literal("Pal")),
           "gateway_message_member_nick_follows_the_author_name");

    char rich_owned_payload[32 * 1024]{};
    ConstSlice<u8> packed_rich_payload{};
    // The standalone decode above rescanned the shared index; the route needs the frame's own.
    expect(gateway_decode_envelope(rich_payload, Slice<u32>{rich_structurals, 2'048}, 32, &envelope).ok(),
           "gateway_envelope_index_rebuilt_for_route");
    const GatewayClientDispatchRouteResult rich_routed = gateway_client_route_dispatch(
        envelope, Slice<u32>{rich_structurals, 2'048}, 32, Slice<char>{rich_text, 32 * 1024},
        Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, 11, &routed_event, &packed_rich_payload, gateway_test_message_scratch());
    bool rich_payload_copied = rich_routed.ok() && packed_rich_payload.count <= sizeof(rich_owned_payload);
    if (rich_payload_copied) {
        for (u32 byte = 0; byte < packed_rich_payload.count; ++byte) {
            rich_owned_payload[byte] = static_cast<char>(packed_rich_payload[byte]);
            rich_text[byte] = 0;
        }
    }
    const ConstSlice<u8> owned_rich_payload{reinterpret_cast<const u8*>(rich_owned_payload),
                                            rich_payload_copied ? packed_rich_payload.count : 0};
    memset(rich_reactions, 0xA5, sizeof(rich_reactions));
    memset(rich_attachments, 0xA5, sizeof(rich_attachments));
    memset(rich_embeds, 0xA5, sizeof(rich_embeds));
    memset(rich_embed_fields, 0xA5, sizeof(rich_embed_fields));
    memset(rich_stickers, 0xA5, sizeof(rich_stickers));
    memset(rich_components, 0xA5, sizeof(rich_components));
    const bool rich_unpacked =
        rich_payload_copied &&
        gateway_client_event_as_message(routed_event, owned_rich_payload, rich_scratch, &routed_message);
    u64 routed_member_role_0 = 0;
    u64 routed_member_role_1 = 0;
    expect(
        rich_unpacked && routed_event.message.rich_payload_bytes > sizeof(GatewayClientMessageRichHeader) &&
            routed_event.message.member_role_count == 2 &&
            gateway_client_event_message_member_role_at(routed_event, owned_rich_payload, 0,
                                                        &routed_member_role_0) &&
            gateway_client_event_message_member_role_at(routed_event, owned_rich_payload, 1,
                                                        &routed_member_role_1) &&
            routed_member_role_0 == 99 && routed_member_role_1 == 100 &&
            routed_message.reference.state == GatewayMessageReferenceState::available &&
            routed_message.reference.message_id == 800 &&
            utf8_equal(routed_message.reference.author_name, utf8_literal("Source Display")) &&
            utf8_equal(routed_message.reference.author_avatar_hash,
                       utf8_literal("fedcba9876543210fedcba9876543210")) &&
            routed_message.reactions.count == 2 && routed_message.reactions[0].emoji_id == 44 &&
            routed_message.reactions[0].me &&
            utf8_equal(routed_message.reactions[1].name, utf8_literal("🔥")) &&
            routed_message.attachments.count == 1 &&
            utf8_equal(routed_message.attachments[0].filename, utf8_literal("clip.mp4")) &&
            routed_message.embeds.count == 1 && routed_message.embed_fields.count == 1 &&
            routed_message.embeds[0].image.width == 640 && routed_message.embeds[0].image.height == 360 &&
            utf8_equal(routed_message.embeds[0].image.proxy_url,
                       utf8_literal("https://media.discordapp.net/external/fixture/image.png")) &&
            utf8_equal(routed_message.embed_fields[0].value, utf8_literal("Release")) &&
            routed_message.stickers.count == 1 && routed_message.components.count == 4 &&
            routed_message.components[3].parent == 2,
        "gateway_client_route_rich_pointer_free_payload");
    Utf8View routed_nick{};
    u64 routed_mention_id = 0;
    Utf8View routed_mention_name{};
    expect(rich_unpacked && routed_event.message.member_nick_bytes == 3 &&
               gateway_client_event_message_member_nick(routed_event, owned_rich_payload, &routed_nick) &&
               utf8_equal(routed_nick, utf8_literal("Ink")) &&
               utf8_equal(routed_message.member_nick, utf8_literal("Ink")) &&
               gateway_client_event_message_mention_user_at(routed_event, owned_rich_payload, 0,
                                                            &routed_mention_id, &routed_mention_name) &&
               routed_mention_id == 13 && utf8_equal(routed_mention_name, utf8_literal("Pal")),
           "gateway_client_route_carries_the_member_nick");
    const auto scratch_poisoned = [](const void* pointer, u32 bytes) noexcept {
        const auto* data = static_cast<const u8*>(pointer);
        for (u32 index = 0; index < bytes; ++index) {
            if (data[index] != 0xA5) return false;
        }
        return true;
    };
    expect(rich_unpacked && !rich_reactions[0].me_burst && !rich_reactions[1].animated &&
               !rich_attachments[0].ephemeral && !rich_embeds[0].thumbnail.has_width &&
               !rich_embeds[0].video.has_height && rich_embed_fields[0].embed_index == 0 &&
               rich_stickers[0].format_type == 2 && !rich_components[3].has_accent_color &&
               scratch_poisoned(rich_reactions + 2, sizeof(rich_reactions) - 2 * sizeof(*rich_reactions)) &&
               scratch_poisoned(rich_attachments + 1, sizeof(rich_attachments) - sizeof(*rich_attachments)) &&
               scratch_poisoned(rich_embeds + 1, sizeof(rich_embeds) - sizeof(*rich_embeds)) &&
               scratch_poisoned(rich_embed_fields + 1, sizeof(rich_embed_fields) - sizeof(*rich_embed_fields)) &&
               scratch_poisoned(rich_stickers + 1, sizeof(rich_stickers) - sizeof(*rich_stickers)) &&
               scratch_poisoned(rich_components + 4, sizeof(rich_components) - 4 * sizeof(*rich_components)),
           "gateway_client_rich_decode_clears_used_fields_and_preserves_poisoned_tails");
    if (rich_payload_copied && packed_rich_payload.count != 0) {
        const u32 final_byte = packed_rich_payload.count - 1;
        rich_owned_payload[final_byte] = 1;
        expect(!gateway_client_event_as_message(routed_event, owned_rich_payload, rich_scratch,
                                                &routed_message),
               "gateway_client_route_rich_corruption_rejected");
        rich_owned_payload[final_byte] = 0;
    }

    constexpr Utf8View deleted_custom_snapshot_payloads[] = {
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2024-08-30T12:34:56Z","reactions":[{"count":4,"count_details":{"normal":3,"burst":1},"me":true,"me_burst":true,"emoji":{"id":"44","name":null,"animated":true}}]},"s":92,"t":"MESSAGE_CREATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":4,"count_details":{"normal":3,"burst":1},"me":true,"me_burst":true,"emoji":{"name":null,"id":"44","animated":true}}]},"s":93,"t":"MESSAGE_UPDATE"})"),
    };
    for (Utf8View snapshot_payload : deleted_custom_snapshot_payloads) {
        bool snapshot_ok = gateway_decode_envelope(snapshot_payload,
            {rich_structurals, 2'048}, 32, &envelope).ok() &&
            gateway_client_route_dispatch(envelope, {rich_structurals, 2'048}, 32,
                {rich_text, 32 * 1024}, {routed_ids, kGatewayBulkDeleteMaximumIds},
                11, &routed_event, &packed_rich_payload, gateway_test_message_scratch()).ok() &&
            packed_rich_payload.count <= sizeof(rich_owned_payload);
        if (snapshot_ok) {
            for (u32 byte = 0; byte < packed_rich_payload.count; ++byte) {
                rich_owned_payload[byte] = static_cast<char>(packed_rich_payload[byte]);
                rich_text[byte] = 0;
            }
        }
        const ConstSlice<u8> snapshot_owned{
            reinterpret_cast<const u8*>(rich_owned_payload),
            snapshot_ok ? packed_rich_payload.count : 0};
        memset(rich_reactions, 0xA5, sizeof(rich_reactions));
        snapshot_ok = snapshot_ok && gateway_client_event_as_message(
            routed_event, snapshot_owned, rich_scratch, &routed_message);
        expect(snapshot_ok && (routed_message.fields & gateway_message_field_reactions) &&
            routed_message.reactions.count == 1 && routed_message.reactions[0].emoji_id == 44 &&
            routed_message.reactions[0].name.bytes == 0 &&
            routed_message.reactions[0].count == 4 && routed_message.reactions[0].normal_count == 3 &&
            routed_message.reactions[0].burst_count == 1 && routed_message.reactions[0].me &&
            routed_message.reactions[0].me_burst && routed_message.reactions[0].animated,
            "gateway_custom_null_name_full_snapshot_create_update_survives_owned_transport");
        if (snapshot_ok) {
            const u32 reaction_offset = routed_event.message.transient_bytes +
                static_cast<u32>(sizeof(GatewayClientMessageRichHeader));
            GatewayClientMessageReactionMutation packed_snapshot_reaction{};
            memcpy(&packed_snapshot_reaction, rich_owned_payload + reaction_offset,
                sizeof(packed_snapshot_reaction));
            packed_snapshot_reaction.emoji_id = 0;
            memcpy(rich_owned_payload + reaction_offset, &packed_snapshot_reaction,
                sizeof(packed_snapshot_reaction));
            expect(!gateway_client_event_as_message(routed_event, snapshot_owned,
                rich_scratch, &routed_message),
                "gateway_packed_full_snapshot_empty_unicode_identity_rejected");
        }
    }
    constexpr Utf8View invalid_deleted_custom_snapshots[] = {
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":null,"name":null}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":null,"name":""}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":"44","name":""}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":"44"}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":"0","name":null}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":"bad","name":null}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":"44","name":false}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"me":false,"emoji":{"id":"44","name":null,"name":"wave"}}]},"t":"MESSAGE_UPDATE"})"),
        utf8_literal(R"({"op":0,"d":{"id":"900","channel_id":"7","reactions":[{"count":1,"count_details":{"normal":2,"burst":0},"me":false,"emoji":{"id":"44","name":null}}]},"t":"MESSAGE_UPDATE"})"),
    };
    for (Utf8View invalid_snapshot : invalid_deleted_custom_snapshots) {
        expect(gateway_decode_envelope(invalid_snapshot, {rich_structurals, 2'048}, 32,
            &envelope).ok() && !gateway_client_route_dispatch(envelope,
                {rich_structurals, 2'048}, 32, {rich_text, 32 * 1024},
                {routed_ids, kGatewayBulkDeleteMaximumIds}, 11,
                &routed_event, &packed_rich_payload, gateway_test_message_scratch()).ok(),
            "gateway_full_snapshot_null_custom_does_not_allow_empty_missing_or_malformed_identity");
    }

    const Utf8View deleted_reference_payload = utf8_literal(
        R"({"op":0,"d":{"id":"901","channel_id":"7","author":{"id":"11"},"content":"reply","timestamp":"2024-08-30T12:34:56Z","message_reference":{"message_id":"801","channel_id":"7"},"referenced_message":null,"reactions":[],"attachments":[],"embeds":[],"sticker_items":[],"components":[]},"s":91,"t":"MESSAGE_CREATE"})");
    expect(decode_gateway(deleted_reference_payload, &envelope).ok() &&
               gateway_decode_message_create(
                   envelope.data, Slice<u32>{rich_structurals, 2'048}, 32,
                   Slice<char>{rich_text, 32 * 1024},
                   Slice<u64>{rich_roles, kGatewayMessageMentionRoleMaximumIds},
                   Slice<u64>{rich_member_roles, kGatewayMessageMemberRoleMaximumIds},
                   Slice<GatewayMessageNamedUser>{rich_users, kGatewayMessageMentionUserMaximum},
                   rich_scratch, 11, &message)
                   .ok() &&
               message.reference.state == GatewayMessageReferenceState::deleted &&
               message.reactions.count == 0 && message.attachments.count == 0 &&
               message.embeds.count == 0 && message.stickers.count == 0 && message.components.count == 0,
           "gateway_message_rich_empty_and_deleted_reference");

    GatewayMessageDeleteEvent deleted{};
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"id":"123","channel_id":"456","guild_id":"789"},"s":1,"t":"MESSAGE_DELETE"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_delete(envelope.data, Slice<u32>{scratch, 256}, 8, &deleted).ok() &&
            deleted.id == 123 && deleted.channel_id == 456,
        "gateway_message_delete_typed_decode");

    u64 ids[3]{};
    GatewayMessageBulkDeleteEvent bulk{};
    expect(
        decode_gateway(
            utf8_literal(
                R"({"op":0,"d":{"ids":["1","2","\u0033"],"channel_id":"456"},"s":1,"t":"MESSAGE_DELETE_BULK"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_bulk_delete(envelope.data, Slice<u32>{scratch, 256}, 8,
                                               Slice<u64>{ids, 3}, &bulk)
                .ok() &&
            bulk.channel_id == 456 && bulk.id_count == 3 && ids[0] == 1 && ids[1] == 2 && ids[2] == 3,
        "gateway_message_bulk_delete_typed_decode");
    expect(gateway_client_route_dispatch(
               envelope, Slice<u32>{scratch, 256}, 8, Slice<char>{routed_content, 64},
               Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                   .ok() &&
               routed_event.dispatch_kind == GatewayClientDispatchKind::message_bulk_delete &&
               routed_event.dispatch_payload_bytes == 3 * sizeof(u64),
           "gateway_client_route_bulk_owned_payload");
    u64 unpacked_ids[3]{};
    GatewayMessageBulkDeleteEvent unpacked_bulk{};
    expect(gateway_client_event_as_message_bulk_delete(routed_event, routed_payload,
                                                       Slice<u64>{unpacked_ids, 3}, &unpacked_bulk) &&
               unpacked_bulk.channel_id == 456 && unpacked_bulk.id_count == 3 && unpacked_ids[0] == 1 &&
               unpacked_ids[1] == 2 && unpacked_ids[2] == 3,
           "gateway_client_unpack_bulk_payload");
    expect(gateway_decode_message_bulk_delete(envelope.data, Slice<u32>{scratch, 256}, 8,
                                              Slice<u64>{ids, 2}, &bulk)
                   .error == GatewayEventDecodeError::message_id_capacity,
           "gateway_message_bulk_delete_capacity");
    expect(
        decode_gateway(
            utf8_literal(R"({"op":0,"d":{"ids":[],"channel_id":"456"},"s":1,"t":"MESSAGE_DELETE_BULK"})"),
            &envelope)
                .ok() &&
            gateway_decode_message_bulk_delete(envelope.data, Slice<u32>{scratch, 256}, 8,
                                               Slice<u64>{ids, 3}, &bulk)
                    .error == GatewayEventDecodeError::invalid_message_ids,
        "gateway_message_bulk_delete_nonempty");

    u32 array_structurals[16]{};
    const Utf8View array_input = utf8_literal("[1,{\"x\":2},3]");
    const JsonScanResult array_scan =
        json_scan_structurals_avx1(array_input, Slice<u32>{array_structurals, 16});
    const JsonReadResult array_read = json_visit_root_array(
        array_input, ConstSlice<u32>{array_structurals, array_scan.count}, 8, nullptr, nullptr);
    expect(array_scan.ok() && array_read.ok() && array_read.member_count == 3, "json_root_array_visit");

    expect(
        decode_gateway(utf8_literal(R"({"op":0,"d":{"future":true},"s":11,"t":"FUTURE_EVENT"})"), &envelope)
                .ok() &&
            gateway_client_route_dispatch(
                envelope, Slice<u32>{scratch, 256}, 8, Slice<char>{routed_content, 64},
                Slice<u64>{routed_ids, kGatewayBulkDeleteMaximumIds}, &routed_event, &routed_payload, gateway_test_message_scratch())
                .ok() &&
            routed_event.dispatch_kind == GatewayClientDispatchKind::unknown &&
            routed_event.event_name_bytes == 12 && routed_payload.count == 0,
        "gateway_client_route_unknown_metadata_only");

    MessageRecord records[4]{};
    u32 order[4]{};
    u8 text[128]{};
    MessageStore store{};
    expect(message_store_initialize(&store, Slice<MessageRecord>{records, 4}, Slice<u32>{order, 4},
                                    Slice<u8>{text, 128}),
           "gateway_message_reconcile_store_initialize");
    constexpr u32 reconcile_create_fields = gateway_message_field_id | gateway_message_field_channel_id |
                                            gateway_message_field_author_id |
                                            gateway_message_field_content | gateway_message_field_timestamp;
    const GatewayMessageEvent create_event{
        1, 2,
        3, 1'725'012'296'789ull,
        0, utf8_literal("first"),
        1, reconcile_create_fields | gateway_message_field_edited_timestamp | gateway_message_field_flags,
    };
    const MessageReconcileResult inserted = message_reconcile_create(&store, create_event);
    const GatewayMessageEvent partial_update{
        1,
        2,
        0,
        0,
        0,
        Utf8View{},
        9,
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_flags |
            gateway_message_field_edited_timestamp,
    };
    const MessageReconcileResult updated = message_reconcile_update(&store, partial_update);
    const MessageRecord* updated_record = message_store_resolve(store, updated.handle);
    expect(inserted.ok() && inserted.inserted && updated.ok() && !updated.inserted && updated_record &&
               updated_record->author_id == 3 && updated_record->timestamp_ms == 1'725'012'296'789ull &&
               updated_record->edited_timestamp_ms == 0 && updated_record->flags == 9 &&
               utf8_equal(message_store_content(store, updated.handle), utf8_literal("first")),
           "gateway_message_reconcile_partial_merge");
    expect(message_reconcile_update(
               &store, GatewayMessageEvent{1, 99, 0, 0, 0, Utf8View{}, 0,
                                           gateway_message_field_id | gateway_message_field_channel_id})
                       .error == MessageReconcileError::channel_mismatch &&
               message_reconcile_update(
                   &store, GatewayMessageEvent{999, 2, 0, 0, 0, Utf8View{}, 0,
                                               gateway_message_field_id | gateway_message_field_channel_id})
                       .error == MessageReconcileError::not_found,
           "gateway_message_reconcile_update_guards");
    expect(message_reconcile_create(&store, GatewayMessageEvent{2, 2, 4, 2, 0, utf8_literal("second"), 0,
                                                                reconcile_create_fields})
                   .ok() &&
               message_reconcile_create(&store, GatewayMessageEvent{3, 8, 5, 3, 0, utf8_literal("third"), 0,
                                                                    reconcile_create_fields})
                   .ok(),
           "gateway_message_reconcile_more_creates");
    expect(message_reconcile_delete(&store, GatewayMessageDeleteEvent{1, 8}).error ==
               MessageReconcileError::channel_mismatch,
           "gateway_message_reconcile_delete_channel_guard");
}

void test_gateway_lifecycle() noexcept {
    GatewayLifecycle lifecycle{};
    gateway_lifecycle_initialize(&lifecycle);
    expect(gateway_lifecycle_connected(&lifecycle, false).ok() &&
               lifecycle.phase == GatewayLifecyclePhase::awaiting_hello,
           "gateway_lifecycle_connected");
    GatewayLifecycle pre_hello = lifecycle;
    expect(gateway_lifecycle_reconnect_requested(&pre_hello).action ==
               GatewayLifecycleAction::reconnect_identify,
           "gateway_reconnect_before_hello");

    const GatewayLifecycleStep hello = gateway_lifecycle_hello(&lifecycle, GatewayHello{45000}, 0, 1000);
    expect(hello.ok() && hello.action == GatewayLifecycleAction::send_identify &&
               lifecycle.next_heartbeat_ms == 1000 && lifecycle.phase == GatewayLifecyclePhase::awaiting_session,
           "gateway_lifecycle_hello_identify");
    expect(gateway_lifecycle_hello(&lifecycle, GatewayHello{45000}, 0, 1000).error ==
               GatewayLifecycleError::invalid_state,
           "gateway_lifecycle_duplicate_hello");

    u8 heartbeat[128]{};
    const GatewayQosPayload both = gateway_qos_payload(gateway_qos_foregrounded | gateway_qos_rtc_connected);
    GatewayWriteResult written =
        gateway_write_heartbeat(lifecycle, &both, Slice<u8>{heartbeat, static_cast<u32>(sizeof(heartbeat))});
    expect(written.ok() &&
               bytes_equal(ConstSlice<u8>{heartbeat, written.bytes},
                           byte_literal(R"({"op":40,"d":{"seq":0,"qos":{"active":true,"ver":31,"reasons":["foregrounded","rtc_connected"]}}})")),
           "gateway_qos_heartbeat_before_any_event");
    expect(gateway_write_heartbeat(lifecycle, &both, Slice<u8>{heartbeat, written.bytes - 1}).error ==
               GatewayLifecycleError::output_too_small,
           "gateway_heartbeat_output_bound");
    const GatewayQosPayload idle = gateway_qos_payload(0);
    const GatewayQosPayload calling = gateway_qos_payload(gateway_qos_rtc_connected);
    const auto heartbeat_equal = [&](const GatewayQosPayload* qos, ConstSlice<u8> expected) noexcept {
        const GatewayWriteResult result =
            gateway_write_heartbeat(lifecycle, qos, Slice<u8>{heartbeat, static_cast<u32>(sizeof(heartbeat))});
        return result.ok() && bytes_equal(ConstSlice<u8>{heartbeat, result.bytes}, expected);
    };
    expect(heartbeat_equal(&idle, byte_literal(R"({"op":40,"d":{"seq":0,"qos":{"active":false,"ver":31,"reasons":[]}}})")) &&
               heartbeat_equal(&calling, byte_literal(R"({"op":40,"d":{"seq":0,"qos":{"active":true,"ver":31,"reasons":["rtc_connected"]}}})")) &&
               heartbeat_equal(nullptr, byte_literal(R"({"op":40,"d":{"seq":0,"qos":null}})")),
           "gateway_qos_heartbeat_payloads");

    // The QoS payload follows the desktop client: activity anywhere in an
    // interval is reported, becoming active sends at once in a session.
    GatewayQosState qos{};
    GatewayQosPayload taken{};
    const GatewayQosPayload focused = gateway_qos_payload(gateway_qos_foregrounded);
    expect(!gateway_qos_take(&qos, &taken) && !gateway_qos_update(&qos, idle, true) &&
               gateway_qos_take(&qos, &taken) && taken == idle,
           "gateway_qos_initial_inactive_state");
    expect(gateway_qos_update(&qos, focused, true) && gateway_qos_take(&qos, &taken) && taken == focused &&
               gateway_qos_take(&qos, &taken) && taken == focused,
           "gateway_qos_becoming_active_sends_now");
    expect(!gateway_qos_update(&qos, calling, true) && gateway_qos_take(&qos, &taken) && taken == both &&
               gateway_qos_take(&qos, &taken) && taken == calling,
           "gateway_qos_merges_reasons_for_the_interval");
    expect(!gateway_qos_update(&qos, idle, true) && gateway_qos_take(&qos, &taken) && taken == calling &&
               gateway_qos_take(&qos, &taken) && taken == idle,
           "gateway_qos_inactive_reported_after_the_active_interval");
    expect(!gateway_qos_update(&qos, focused, false) && gateway_qos_take(&qos, &taken) && taken == focused,
           "gateway_qos_no_immediate_heartbeat_without_session");
    GatewayQosState pending{};
    gateway_qos_update(&pending, idle, false);
    expect(gateway_qos_update(&pending, focused, true) && (gateway_qos_send_now(&pending), true) &&
               gateway_qos_take(&pending, &taken) && taken == focused && gateway_qos_take(&pending, &taken) &&
               taken == idle && gateway_qos_take(&pending, &taken) && taken == focused,
           "gateway_qos_second_immediate_heartbeat_consumes_when_written");
    char time_spent[256]{};
    const GatewayCommandWriteResult announced = gateway_write_time_spent_session(
        1'700'000'000'123ull, utf8_literal("0123abcd-4567-4890-a123-456789abcdef"),
        utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
        Slice<u8>{reinterpret_cast<u8*>(time_spent), static_cast<u32>(sizeof(time_spent))});
    expect(announced.ok() &&
               utf8_equal({time_spent, announced.bytes},
                          utf8_literal(R"({"op":41,"d":{"initialization_timestamp":1700000000123,"session_id":"0123abcd-4567-4890-a123-456789abcdef","client_launch_id":"00112233-4455-6677-8899-aabbccddeeff"}})")) &&
               gateway_write_time_spent_session(1, utf8_literal("x"), utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
                                                Slice<u8>{reinterpret_cast<u8*>(time_spent), 256})
                       .error == GatewayCommandError::invalid_input &&
               gateway_write_time_spent_session(1'700'000'000'123ull, utf8_literal("0123abcd-4567-4890-a123-456789abcdef"),
                                                utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
                                                Slice<u8>{reinterpret_cast<u8*>(time_spent), announced.bytes - 1})
                       .error == GatewayCommandError::output_too_small,
           "gateway_time_spent_session_write");

    expect(gateway_lifecycle_tick(&lifecycle, 999).action == GatewayLifecycleAction::none,
           "gateway_heartbeat_before_deadline");
    const GatewayLifecycleStep first_heartbeat = gateway_lifecycle_tick(&lifecycle, 1000);
    expect(first_heartbeat.action == GatewayLifecycleAction::send_heartbeat &&
               lifecycle.next_heartbeat_ms == 46000 && lifecycle.awaiting_heartbeat_ack,
           "gateway_first_heartbeat");
    expect(gateway_lifecycle_heartbeat_acknowledged(&lifecycle).ok() && !lifecycle.awaiting_heartbeat_ack,
           "gateway_heartbeat_ack");
    expect(gateway_lifecycle_dispatch(&lifecycle, ~0ull).ok() && lifecycle.has_sequence,
           "gateway_dispatch_sequence_tracking");
    written = gateway_write_heartbeat(lifecycle, &idle, Slice<u8>{heartbeat, static_cast<u32>(sizeof(heartbeat))});
    expect(written.ok() &&
               bytes_equal(ConstSlice<u8>{heartbeat, written.bytes},
                           byte_literal(R"({"op":40,"d":{"seq":18446744073709551615,"qos":{"active":false,"ver":31,"reasons":[]}}})")),
           "gateway_heartbeat_u64_sequence");

    const u64 regular_deadline = lifecycle.next_heartbeat_ms;
    expect(gateway_lifecycle_heartbeat_requested(&lifecycle).action ==
                   GatewayLifecycleAction::send_heartbeat &&
               lifecycle.next_heartbeat_ms == regular_deadline,
           "gateway_server_heartbeat_request");
    expect(gateway_lifecycle_heartbeat_acknowledged(&lifecycle).ok(), "gateway_server_heartbeat_ack");
    expect(gateway_lifecycle_session_ready(&lifecycle).ok() &&
               lifecycle.phase == GatewayLifecyclePhase::ready && lifecycle.resume_available,
           "gateway_session_ready");
    expect(gateway_lifecycle_tick(&lifecycle, regular_deadline).action ==
               GatewayLifecycleAction::send_heartbeat,
           "gateway_regular_heartbeat");
    expect(gateway_lifecycle_tick(&lifecycle, lifecycle.next_heartbeat_ms).action ==
                   GatewayLifecycleAction::reconnect_resume &&
               lifecycle.phase == GatewayLifecyclePhase::reconnecting,
           "gateway_missing_ack_resume");

    expect(gateway_lifecycle_connected(&lifecycle, true).ok(), "gateway_resume_reconnected");
    const GatewayLifecycleStep resume_hello =
        gateway_lifecycle_hello(&lifecycle, GatewayHello{45000}, 0xFFFFFFFFu, 5000);
    expect(resume_hello.action == GatewayLifecycleAction::send_resume &&
               lifecycle.next_heartbeat_ms == 49999,
           "gateway_resume_and_full_jitter");
    expect(gateway_lifecycle_invalid_session(&lifecycle, true).action ==
               GatewayLifecycleAction::reconnect_resume,
           "gateway_invalid_session_resumable");
    expect(gateway_lifecycle_connected(&lifecycle, true).ok() &&
               gateway_lifecycle_hello(&lifecycle, GatewayHello{45000}, 0, 6000).action ==
                   GatewayLifecycleAction::send_resume,
           "gateway_second_resume_attempt");
    expect(gateway_lifecycle_invalid_session(&lifecycle, false).action ==
                   GatewayLifecycleAction::reconnect_identify &&
               !lifecycle.resume_available && !lifecycle.has_sequence,
           "gateway_invalid_session_reidentify");

    expect(gateway_classify_close(4000) == GatewayCloseDisposition::resume &&
               gateway_classify_close(4008) == GatewayCloseDisposition::resume,
           "gateway_close_resumable_codes");
    expect(gateway_classify_close(4007) == GatewayCloseDisposition::reidentify &&
               gateway_classify_close(4009) == GatewayCloseDisposition::reidentify,
           "gateway_close_new_session_codes");
    expect(gateway_classify_close(1000) == GatewayCloseDisposition::resume &&
               gateway_classify_close(1001) == GatewayCloseDisposition::resume &&
               gateway_classify_close(1006) == GatewayCloseDisposition::resume,
           "gateway_close_server_going_away_reconnects");
    expect(!gateway_resume_expired(0, 1'000'000) && !gateway_resume_expired(1'000, 181'000) &&
               gateway_resume_expired(1'000, 181'001) && !gateway_resume_expired(5'000, 1'000),
           "gateway_resume_expires_after_three_minutes");
    expect(gateway_classify_close(4004) == GatewayCloseDisposition::stop &&
               gateway_classify_close(4010) == GatewayCloseDisposition::stop &&
               gateway_classify_close(4011) == GatewayCloseDisposition::stop &&
               gateway_classify_close(4012) == GatewayCloseDisposition::stop &&
               gateway_classify_close(4013) == GatewayCloseDisposition::stop &&
               gateway_classify_close(4014) == GatewayCloseDisposition::stop,
           "gateway_close_fatal_codes");

    GatewayLifecycle close_state{};
    gateway_lifecycle_initialize(&close_state);
    expect(gateway_lifecycle_connected(&close_state, false).ok() &&
               gateway_lifecycle_closed(&close_state, 4004).action == GatewayLifecycleAction::stop &&
               close_state.phase == GatewayLifecyclePhase::idle,
           "gateway_close_fatal_stops");
}

void test_gateway_runtime() noexcept {
    GatewayReady ready{};
    ready.session_id = JsonString{utf8_literal("session\\u0031"), 0, true};
    ready.resume_gateway_url = JsonString{utf8_literal("wss:\\/\\/gateway.discord.gg"), 0, true};
    GatewayResumeState resume{};
    const GatewayResumeCaptureError captured = gateway_resume_capture(&resume, ready, 0x123456789ull);
    expect(captured == GatewayResumeCaptureError::none && resume.valid && resume.session_id_bytes == 8 &&
               resume.resume_url_bytes == 24 && resume.session_id[7] == '1' &&
               resume.resume_url[3] == ':' && resume.sequence == 0x123456789ull &&
               gateway_resume_validate(resume),
           "gateway_resume_capture");

    GatewayResumeState invalid_resume = resume;
    invalid_resume.resume_url[0] = 'h';
    expect(!gateway_resume_validate(invalid_resume), "gateway_resume_validate_scheme");
    invalid_resume = resume;
    invalid_resume.session_id_bytes = kGatewaySessionIdCapacity;
    expect(!gateway_resume_validate(invalid_resume), "gateway_resume_validate_bounds");

    char oversized_id[kGatewaySessionIdCapacity]{};
    for (u32 index = 0; index < kGatewaySessionIdCapacity; ++index) {
        oversized_id[index] = 'x';
    }
    GatewayReady oversized = ready;
    oversized.session_id = JsonString{Utf8View{oversized_id, kGatewaySessionIdCapacity}, 0, false};
    const u64 preserved_sequence = resume.sequence;
    expect(gateway_resume_capture(&resume, oversized, 7) ==
                   GatewayResumeCaptureError::session_id_too_large &&
               resume.valid && resume.sequence == preserved_sequence,
           "gateway_resume_capture_atomic_bound");
    gateway_resume_clear(&resume);
    bool resume_zero = true;
    const u8* resume_bytes = reinterpret_cast<const u8*>(&resume);
    for (u32 index = 0; index < sizeof(resume); ++index) {
        resume_zero = resume_zero && resume_bytes[index] == 0;
    }
    expect(resume_zero, "gateway_resume_clear");

    GatewayReconnectBackoff backoff{};
    const GatewayReconnectDelay first = gateway_reconnect_next(&backoff, 0);
    const GatewayReconnectDelay second = gateway_reconnect_next(&backoff, ~0u);
    expect(first.delay_ms == 1'000 && second.delay_ms == 3'000 && backoff.delay_ms == 3'000,
           "gateway_reconnect_backoff_grows_from_one_second");
    GatewayReconnectBackoff steady{};
    expect(gateway_reconnect_next(&steady, 0).delay_ms == 1'000 && gateway_reconnect_next(&steady, 0).delay_ms == 1'000,
           "gateway_reconnect_backoff_never_shrinks");
    GatewayReconnectDelay capped{};
    for (u32 index = 0; index < 8; ++index) {
        capped = gateway_reconnect_next(&backoff, ~0u);
    }
    expect(capped.delay_ms == 60'000, "gateway_reconnect_backoff_cap");
    gateway_reconnect_reset(&backoff);
    expect(backoff.delay_ms == 0, "gateway_reconnect_reset");
    // A READY that cannot be projected re-identifies on the floor plus the reconnect jitter. Even on the
    // shortest schedule (zero jitter randomness) it never comes back within 5 s, nor makes more than four
    // sessions in any minute, where the desktop client's dispatch-error reset retries at once and then from 1 s.
    {
        GatewayReconnectBackoff ready_backoff{};
        u64 identified_at[17]{};
        bool spaced = true;
        for (u32 failures = 1; failures <= 16; ++failures) {
            const u32 delay =
                gateway_ready_failure_delay(failures) + gateway_reconnect_next(&ready_backoff, 0).delay_ms;
            spaced = spaced && delay >= 6'000;
            identified_at[failures] = identified_at[failures - 1] + delay;
        }
        bool bounded = true;
        for (u32 index = 0; index + 4 <= 16; ++index)
            bounded = bounded && identified_at[index + 4] - identified_at[index] > 60'000;
        expect(spaced && bounded && gateway_ready_failure_delay(16) == 600'000,
               "gateway_ready_failure_reidentify_at_most_four_sessions_a_minute");
    }
    // An invalid session is retried on the same socket at once: identify, or resume when it is resumable.
    {
        GatewayLifecycle retry{};
        gateway_lifecycle_initialize(&retry);
        gateway_lifecycle_connected(&retry, false);
        gateway_lifecycle_hello(&retry, GatewayHello{41'250}, 0, 1'000);
        gateway_lifecycle_session_ready(&retry);
        gateway_lifecycle_dispatch(&retry, 7);
        const GatewayLifecycleStep invalid = gateway_lifecycle_invalid_session(&retry, false);
        const GatewayLifecycleStep again = gateway_lifecycle_session_retry(&retry);
        expect(invalid.action == GatewayLifecycleAction::reconnect_identify &&
                   again.action == GatewayLifecycleAction::send_identify &&
                   retry.phase == GatewayLifecyclePhase::awaiting_session && retry.heartbeat_interval_ms == 41'250 &&
                   !retry.has_sequence && gateway_lifecycle_session_retry(&retry).error ==
                                              GatewayLifecycleError::invalid_state,
               "gateway_invalid_session_identifies_again_on_the_same_socket");
        gateway_lifecycle_dispatch(&retry, 9);
        gateway_lifecycle_session_ready(&retry);
        gateway_lifecycle_invalid_session(&retry, true);
        expect(gateway_lifecycle_session_retry(&retry).action == GatewayLifecycleAction::send_resume &&
                   retry.last_sequence == 9,
               "gateway_resumable_invalid_session_resumes_on_the_same_socket");
    }

    GatewayRateLimiter limiter{};
    bool admitted = true;
    for (u32 index = 0; index < kGatewaySendLimit; ++index) {
        admitted = admitted && gateway_rate_limit_acquire(&limiter, 10'000).acquired;
    }
    const GatewayRateLimitStep limited = gateway_rate_limit_acquire(&limiter, 69'999);
    const GatewayRateLimitStep reopened = gateway_rate_limit_acquire(&limiter, 70'000);
    expect(admitted && limiter.count == 1 && !limited.acquired && limited.next_allowed_ms == 70'000 &&
               reopened.acquired && reopened.next_allowed_ms == 70'000,
           "gateway_rate_limit_sliding_window");

    GatewayOutboundQueue outbound{};
    expect(gateway_outbound_enqueue(&outbound, GatewayLifecycleAction::send_identify) &&
               gateway_outbound_enqueue(&outbound, GatewayLifecycleAction::send_heartbeat) &&
               gateway_outbound_enqueue(&outbound, GatewayLifecycleAction::send_resume),
           "gateway_outbound_enqueue");
    const GatewayOutboundStep identify = gateway_outbound_take(&outbound, 100);
    const GatewayOutboundStep heartbeat = gateway_outbound_take(&outbound, 101);
    const GatewayOutboundStep resume_action = gateway_outbound_take(&outbound, 102);
    expect(identify.state == GatewayOutboundState::action &&
               identify.action == GatewayLifecycleAction::send_identify &&
               heartbeat.action == GatewayLifecycleAction::send_heartbeat &&
               resume_action.action == GatewayLifecycleAction::send_resume && outbound.count == 0,
           "gateway_outbound_fifo");
    bool outbound_full = true;
    for (u32 index = 0; index < kGatewayOutboundActionCapacity; ++index) {
        outbound_full =
            outbound_full && gateway_outbound_enqueue(&outbound, GatewayLifecycleAction::send_heartbeat);
    }
    expect(outbound_full && !gateway_outbound_enqueue(&outbound, GatewayLifecycleAction::send_heartbeat),
           "gateway_outbound_bound");
    gateway_outbound_reset(&outbound);
    for (u32 index = 0; index < kGatewaySendLimit; ++index) {
        gateway_rate_limit_acquire(&outbound.limiter, 5'000);
    }
    gateway_outbound_enqueue(&outbound, GatewayLifecycleAction::send_heartbeat);
    const GatewayOutboundStep outbound_limited = gateway_outbound_take(&outbound, 64'999);
    expect(outbound_limited.state == GatewayOutboundState::rate_limited &&
               outbound_limited.next_allowed_ms == 65'000 && outbound.count == 1,
           "gateway_outbound_rate_wait");
}

void test_gateway_endpoint() noexcept {
    GatewayEndpoint endpoint{};
    expect(
        gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
                GatewayEndpointError::none &&
            endpoint.port == 443 && endpoint.host_bytes == 18 && endpoint.host_wide[0] == L'g' &&
            endpoint.service_wide[0] == L'4' &&
            endpoint.policy == GatewayEndpointPolicy::application_v10_json &&
            bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(endpoint.target), endpoint.target_bytes},
                        byte_literal("/?v=10&encoding=json")),
        "gateway_endpoint_default");

    expect(
        gateway_endpoint_parse_with_policy(utf8_literal("wss://edge.example:8443/socket?old=1"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
                GatewayEndpointError::none &&
            endpoint.port == 8443 && endpoint.service_wide[3] == L'3' &&
            bytes_equal(
                ConstSlice<u8>{reinterpret_cast<const u8*>(endpoint.authority), endpoint.authority_bytes},
                byte_literal("edge.example:8443")) &&
            bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(endpoint.target), endpoint.target_bytes},
                        byte_literal("/socket?v=10&encoding=json")),
        "gateway_endpoint_port_path");

    expect(
        gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg/socket?old=1"),
                                           GatewayEndpointPolicy::user_v9_etf_zstd_stream,
                                           &endpoint) == GatewayEndpointError::none &&
            endpoint.policy == GatewayEndpointPolicy::user_v9_etf_zstd_stream &&
            bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(endpoint.target), endpoint.target_bytes},
                        byte_literal("/socket?encoding=etf&v=9&compress=zstd-stream")) &&
            gateway_endpoint_validate(endpoint),
        "gateway_endpoint_user_v9_etf_zstd_policy_path");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway-us-east1-b.discord.gg"),
                                              GatewayEndpointPolicy::user_v9_etf_zstd_stream,
                                              &endpoint) == GatewayEndpointError::none &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(endpoint.target), endpoint.target_bytes},
                           byte_literal("/?encoding=etf&v=9&compress=zstd-stream")) &&
               gateway_endpoint_validate(endpoint),
           "gateway_endpoint_user_v9_etf_zstd_policy");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg"),
                                              static_cast<GatewayEndpointPolicy>(0xFF),
                                              &endpoint) == GatewayEndpointError::invalid_input,
           "gateway_endpoint_reject_policy");
    GatewayEndpoint mismatched_policy = endpoint;
    mismatched_policy.policy = GatewayEndpointPolicy::application_v10_json;
    expect(!gateway_endpoint_validate(mismatched_policy), "gateway_endpoint_reject_policy_target_mismatch");

    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://[2001:db8::1]:444/"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
                   GatewayEndpointError::none &&
               endpoint.port == 444 && endpoint.host_wide[0] == L'2' && endpoint.host_wide[10] == L'1',
           "gateway_endpoint_ipv6");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("https://gateway.discord.gg"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
               GatewayEndpointError::invalid_scheme,
           "gateway_endpoint_reject_scheme");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://user@gateway.discord.gg"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
               GatewayEndpointError::invalid_authority,
           "gateway_endpoint_reject_userinfo");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg:0"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
               GatewayEndpointError::invalid_port,
           "gateway_endpoint_reject_port_zero");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg:70000"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
               GatewayEndpointError::invalid_port,
           "gateway_endpoint_reject_port_overflow");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg:999999999999999999999999"), GatewayEndpointPolicy::application_v10_json,
                                  &endpoint) == GatewayEndpointError::invalid_port,
           "gateway_endpoint_reject_port_integer_wrap");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://bad..example"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
               GatewayEndpointError::invalid_host,
           "gateway_endpoint_reject_empty_label");
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg/path#fragment"), GatewayEndpointPolicy::application_v10_json, &endpoint) ==
               GatewayEndpointError::invalid_target,
           "gateway_endpoint_reject_fragment");
    GatewayEndpoint validated{};
    expect(gateway_endpoint_parse_with_policy(utf8_literal("wss://edge.example:8443/socket"), GatewayEndpointPolicy::application_v10_json, &validated) ==
                   GatewayEndpointError::none &&
               gateway_endpoint_validate(validated),
           "gateway_endpoint_canonical_validation");
    validated.authority[0] = 'x';
    expect(!gateway_endpoint_validate(validated), "gateway_endpoint_reject_inconsistent_copy");
}

void test_gateway_commands() noexcept {
    expect(kGatewayUserCapabilitiesDefault == 1'734'653 && (kGatewayUserCapabilitiesDefault & (1u << 8)) != 0,
           "gateway_advertises_auth_token_refresh_as_the_desktop_client");
    u8 output[2048]{};
    u8 guarded[2048]{};
    guard_fill(guarded, sizeof(guarded));
    bool unchanged = true;
    const char invalid_utf8[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};

    const GatewayUserIdentify user_identify{
        utf8_literal("user\"\n-token"),
        GatewayUserProperties{
            utf8_literal("Windows"), utf8_literal("Discord Client"), utf8_literal("stable"),
            utf8_literal("1.0.9256"), utf8_literal("10.0.19045"), utf8_literal("x64"), utf8_literal("x64"),
            utf8_literal("en-US"), utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
            utf8_literal("Agent\\1.0\nNative"), utf8_literal("42.9.0"), utf8_literal("19045"), 603738},
        0,
        GatewayUserStatus::online,
        false,
        false};
    const ConstSlice<u8> expected_user_properties = byte_literal(
        R"({"os":"Windows","browser":"Discord Client","release_channel":"stable","client_version":"1.0.9256","os_version":"10.0.19045","os_version_is_release":true,"os_sdk_version":"19045","os_arch":"x64","app_arch":"x64","system_locale":"en-US","has_client_mods":false,"client_launch_id":"00112233-4455-6677-8899-aabbccddeeff","browser_user_agent":"Agent\\1.0\nNative","browser_version":"42.9.0","client_build_number":603738,"client_event_source":null})");
    const ConstSlice<u8> expected_user_identify = byte_literal(
        R"({"op":2,"d":{"token":"user\"\n-token","capabilities":1734653,"properties":{"os":"Windows","browser":"Discord Client","release_channel":"stable","client_version":"1.0.9256","os_version":"10.0.19045","os_version_is_release":true,"os_sdk_version":"19045","os_arch":"x64","app_arch":"x64","system_locale":"en-US","has_client_mods":false,"client_launch_id":"00112233-4455-6677-8899-aabbccddeeff","browser_user_agent":"Agent\\1.0\nNative","browser_version":"42.9.0","client_build_number":603738,"client_event_source":null,"client_heartbeat_session_id":null,"is_fast_connect":false,"gateway_connect_reasons":"COLD_START"},"presence":{"status":"online","since":0,"activities":[],"afk":false},"compress":false,"client_state":{"guild_versions":{}}}})");
    const GatewayCommandWriteResult user_identified =
        gateway_write_user_identify(user_identify, Slice<u8>{output, expected_user_identify.count});
    expect(user_identified.ok() && user_identified.bytes == expected_user_identify.count &&
               bytes_equal(ConstSlice<u8>{output, user_identified.bytes}, expected_user_identify),
           "gateway_user_identify_fresh_write");
    // The desktop client's capabilities with channel obfuscation off, AUTH_TOKEN_REFRESH (bit 8) included: READY's
    // replacement auth_token is adopted by the Gateway, REST and the saved profile. No installation_id is sent (a
    // server-issued analytics ID the client keeps in local storage).
    bool installation_absent = true;
    for (u32 index = 0; index + 15 <= user_identified.bytes; ++index)
        installation_absent = installation_absent && !bytes_equal(ConstSlice<u8>{output + index, 15},
                                                                  ConstSlice<u8>{reinterpret_cast<const u8*>("installation_id"), 15});
    expect(installation_absent, "gateway_user_identify_capabilities_and_properties_pinned");

    // The X-Super-Properties header carries the same object: measured without
    // an output, written whole or not at all.
    const GatewayCommandWriteResult properties_measured =
        gateway_write_user_properties(user_identify.properties, {});
    const GatewayCommandWriteResult properties_written = gateway_write_user_properties(
        user_identify.properties, Slice<u8>{output, expected_user_properties.count});
    expect(properties_measured.ok() && properties_measured.bytes == expected_user_properties.count &&
               properties_written.ok() && properties_written.bytes == expected_user_properties.count &&
               bytes_equal(ConstSlice<u8>{output, properties_written.bytes}, expected_user_properties),
           "gateway_user_properties_write");
    guard_fill(guarded, sizeof(guarded), 0x5A);
    unchanged = gateway_write_user_properties(user_identify.properties,
                                              Slice<u8>{guarded, expected_user_properties.count - 1})
                        .error == GatewayCommandError::output_too_small &&
                guard_intact(guarded, sizeof(guarded), 0x5A);
    expect(unchanged, "gateway_user_properties_output_bound");

#define MAICORD_TEST_PROPERTIES_HEAD \
    R"({"os":"Windows","browser":"Discord Client","release_channel":"stable","client_version":"1.0.9256","os_version":"10.0.19045","os_version_is_release":true,"os_sdk_version":"19045","os_arch":"x64","app_arch":"x64","system_locale":"en-US","has_client_mods":false,"client_launch_id":"00112233-4455-6677-8899-aabbccddeeff","browser_user_agent":"Agent\\1.0\nNative","browser_version":"42.9.0","client_build_number":603738,"native_build_number":96160,"client_event_source":null,"launch_signature":"10203040-5060-4708-9090-a0b0c0d0e0f0")"
    // The desktop client's later properties: native build, launch signature,
    // then the request-time heartbeat session and app state. Identify replaces
    // the app state in place or appends it.
    GatewayUserProperties live_properties = user_identify.properties;
    live_properties.native_build_number = 96160;
    live_properties.launch_signature = utf8_literal("10203040-5060-4708-9090-a0b0c0d0e0f0");
    live_properties.client_heartbeat_session_id = utf8_literal("ffeeddcc-bbaa-4998-8776-655443322110");
    live_properties.client_app_state = utf8_literal("unfocused");
    const auto properties_equal = [&](const GatewayUserProperties& properties, ConstSlice<u8> expected) noexcept {
        const GatewayCommandWriteResult written =
            gateway_write_user_properties(properties, Slice<u8>{output, static_cast<u32>(sizeof(output))});
        return written.ok() && bytes_equal(ConstSlice<u8>{output, written.bytes}, expected);
    };
    expect(properties_equal(live_properties,
                            byte_literal(MAICORD_TEST_PROPERTIES_HEAD
                                         R"(,"client_heartbeat_session_id":"ffeeddcc-bbaa-4998-8776-655443322110","client_app_state":"unfocused"})")),
           "gateway_user_properties_desktop_tail_order");
    // Windows reports its update build revision after browser_version.
    GatewayUserProperties revision_properties = user_identify.properties;
    revision_properties.os_build_revision = utf8_literal("1000");
    expect(properties_equal(revision_properties,
                            byte_literal(R"({"os":"Windows","browser":"Discord Client","release_channel":"stable","client_version":"1.0.9256","os_version":"10.0.19045","os_version_is_release":true,"os_sdk_version":"19045","os_arch":"x64","app_arch":"x64","system_locale":"en-US","has_client_mods":false,"client_launch_id":"00112233-4455-6677-8899-aabbccddeeff","browser_user_agent":"Agent\\1.0\nNative","browser_version":"42.9.0","os_build_revision":"1000","client_build_number":603738,"client_event_source":null})")),
           "gateway_user_properties_windows_build_revision");
    GatewayUserIdentify live_identify = user_identify;
    live_identify.token = utf8_literal("t");
    live_identify.properties = live_properties;
    live_identify.client_app_state = utf8_literal("focused");
    const auto identify_equal = [&](const GatewayUserIdentify& identify, ConstSlice<u8> expected) noexcept {
        const GatewayCommandWriteResult written =
            gateway_write_user_identify(identify, Slice<u8>{output, static_cast<u32>(sizeof(output))});
        return written.ok() && bytes_equal(ConstSlice<u8>{output, written.bytes}, expected);
    };
    expect(identify_equal(live_identify,
                          byte_literal(R"({"op":2,"d":{"token":"t","capabilities":1734653,"properties":)"
                                       MAICORD_TEST_PROPERTIES_HEAD
                                       R"(,"client_heartbeat_session_id":"ffeeddcc-bbaa-4998-8776-655443322110","client_app_state":"focused","is_fast_connect":false,"gateway_connect_reasons":"COLD_START"},"presence":{"status":"online","since":0,"activities":[],"afk":false},"compress":false,"client_state":{"guild_versions":{}}}})")),
           "gateway_user_identify_replaces_app_state_in_place");
    live_identify.properties.client_app_state = {};
    live_identify.properties.client_heartbeat_session_id = {};
    // The page's properties carry the heartbeat session key as null while there is none.
    expect(identify_equal(live_identify,
                          byte_literal(R"({"op":2,"d":{"token":"t","capabilities":1734653,"properties":)"
                                       MAICORD_TEST_PROPERTIES_HEAD
                                       R"(,"client_heartbeat_session_id":null,"client_app_state":"focused","is_fast_connect":false,"gateway_connect_reasons":"COLD_START"},"presence":{"status":"online","since":0,"activities":[],"afk":false},"compress":false,"client_state":{"guild_versions":{}}}})")),
           "gateway_user_identify_appends_absent_app_state");
    // Later identifies: the connect reasons the client counted and its QoS token.
    live_identify.connect_reasons = utf8_literal("AppSkeleton");
    live_identify.qos_token = utf8_literal("CgIIAQ==");
    expect(identify_equal(live_identify,
                          byte_literal(R"({"op":2,"d":{"token":"t","capabilities":1734653,"properties":)"
                                       MAICORD_TEST_PROPERTIES_HEAD
                                       R"(,"client_heartbeat_session_id":null,"client_app_state":"focused","is_fast_connect":false,"gateway_connect_reasons":"AppSkeleton"},"presence":{"status":"online","since":0,"activities":[],"afk":false},"compress":false,"client_state":{"guild_versions":{}},"qos_token":"CgIIAQ=="}})")),
           "gateway_user_identify_connect_reasons_and_qos_token");
    // Before the launch's first session opens the page's presence is unknown.
    live_identify.status = GatewayUserStatus::unknown;
    expect(identify_equal(live_identify,
                          byte_literal(R"({"op":2,"d":{"token":"t","capabilities":1734653,"properties":)"
                                       MAICORD_TEST_PROPERTIES_HEAD
                                       R"(,"client_heartbeat_session_id":null,"client_app_state":"focused","is_fast_connect":false,"gateway_connect_reasons":"AppSkeleton"},"presence":{"status":"unknown","since":0,"activities":[],"afk":false},"compress":false,"client_state":{"guild_versions":{}},"qos_token":"CgIIAQ=="}})")),
           "gateway_user_identify_unknown_presence");
    live_identify.status = GatewayUserStatus::online;
    live_identify.qos_token = utf8_literal("CgII AQ=");
    expect(gateway_write_user_identify(live_identify, Slice<u8>{output, static_cast<u32>(sizeof(output))}).error ==
               GatewayCommandError::invalid_input,
           "gateway_user_identify_qos_token_validation");
    // The fast-connect identify: the page's reduced properties (no launch
    // signature, heartbeat session or app state), no presence, compress or
    // QoS token.
    live_identify.properties.client_heartbeat_session_id = utf8_literal("ffeeddcc-bbaa-4998-8776-655443322110");
    live_identify.properties.client_app_state = utf8_literal("unfocused");
    live_identify.properties.os_build_revision = utf8_literal("1000");
    live_identify.properties.client_launch_id = utf8_literal("99887766-5544-4332-a110-ffeeddccbbaa");
    live_identify.fast_connect = true;
    live_identify.has_since = true;
    live_identify.since_ms = 1759800000000ull;
    live_identify.qos_token = utf8_literal("CgIIAQ==");
    expect(identify_equal(live_identify,
                          byte_literal(R"({"op":2,"d":{"token":"t","capabilities":1734653,"properties":{"os":"Windows","browser":"Discord Client","release_channel":"stable","client_version":"1.0.9256","os_version":"10.0.19045","os_version_is_release":true,"os_sdk_version":"19045","os_arch":"x64","app_arch":"x64","system_locale":"en-US","has_client_mods":false,"client_launch_id":"99887766-5544-4332-a110-ffeeddccbbaa","browser_user_agent":"Agent\\1.0\nNative","browser_version":"42.9.0","os_build_revision":"1000","client_build_number":603738,"native_build_number":96160,"client_event_source":null,"is_fast_connect":true},"client_state":{"guild_versions":{}}}})")),
           "gateway_user_identify_fast_connect");
    {
        u8 measured[2048];
        const auto fast = gateway_write_user_identify(live_identify, Slice<u8>{measured, sizeof(measured)});
        expect(fast.ok() && gateway_write_user_identify(live_identify, Slice<u8>{measured, fast.bytes - 1}).error ==
                                GatewayCommandError::output_too_small,
               "gateway_user_identify_fast_connect_output_bound");
    }
    // QoS tokens: client_provided {is_active}, then the READY data re-encoded
    // in field order with unknown fields last.
    {
        char token[kGatewayQosTokenMaximumBytes];
        u8 derived[kGatewayQosDerivedMaximumBytes];
        const u32 idle = gateway_qos_token_write(false, false, {}, {token, sizeof(token)});
        expect(idle == 4 && maicord::bytes_equal(token, "CgA=", 4), "gateway_qos_token_idle");
        const u32 active = gateway_qos_token_write(true, false, {}, {token, sizeof(token)});
        expect(active == 8 && maicord::bytes_equal(token, "CgIIAQ==", 8), "gateway_qos_token_active");
        const u8 canonical[] = {0x0a, 0x03, 'a', 'b', 'c', 0x12, 0x02, 'x', 'y', 0x18, 0x07, 0x20, 0x05};
        u32 reordered = 0;
        expect(gateway_qos_derived_from_ready(utf8_literal("GAcgBQoDYWJjEgJ4eQ=="), {derived, sizeof(derived)},
                                              &reordered) &&
                   reordered == sizeof(canonical) && maicord::bytes_equal(derived, canonical, sizeof(canonical)),
               "gateway_qos_derived_reencoded");
        const u32 with_derived = gateway_qos_token_write(true, true, {derived, reordered}, {token, sizeof(token)});
        expect(with_derived == 28 && maicord::bytes_equal(token, "CgIIARINCgNhYmMSAnh5GAcgBQ==", 28),
               "gateway_qos_token_with_derived");
        // An empty message is still sent (as an empty derived field).
        u32 empty = 1, key_only = 0, unused = 0;
        expect(gateway_qos_derived_from_ready(utf8_literal("CgA="), {derived, sizeof(derived)}, &empty) && empty == 0 &&
                   gateway_qos_token_write(false, true, {derived, 0}, {token, sizeof(token)}) == 8 &&
                   maicord::bytes_equal(token, "CgASAA==", 8),
               "gateway_qos_token_empty_derived");
        expect(!gateway_qos_derived_from_ready(utf8_literal("CgNhYmMSAnh5GAc"), {derived, sizeof(derived)}, &unused) &&
                   gateway_qos_derived_from_ready(utf8_literal("GAo="), {derived, sizeof(derived)}, &key_only) &&
                   key_only == 2 &&
                   !gateway_qos_derived_from_ready(utf8_literal("Cg=="), {derived, sizeof(derived)}, &unused) &&
                   !gateway_qos_derived_from_ready(utf8_literal("EgB4"), {derived, sizeof(derived)}, &unused) &&
                   gateway_qos_token_write(true, false, {}, {token, 7}) == 0,
               "gateway_qos_token_invalid_inputs");
    }
#undef MAICORD_TEST_PROPERTIES_HEAD
    live_properties.launch_signature = utf8_literal("10203040-5060-4708-9090-a0b0c0d0e0fg");
    expect(gateway_write_user_properties(live_properties, {}).error == GatewayCommandError::invalid_input,
           "gateway_user_properties_launch_signature_validation");
    live_properties.launch_signature = utf8_literal("10203040-5060-4708-9090-a0b0c0d0e0f0");
    live_properties.client_heartbeat_session_id = utf8_literal("not-a-session");
    expect(gateway_write_user_properties(live_properties, {}).error == GatewayCommandError::invalid_input,
           "gateway_user_properties_heartbeat_session_validation");
    char header[1024]{};
    const Base64Result header_expected =
        base64_encode(expected_user_properties, Slice<char>{header, static_cast<u32>(sizeof(header))});
    const RestUserPropertiesResult header_written = write_rest_user_super_properties(
        user_identify.properties, Slice<u8>{guarded, static_cast<u32>(sizeof(guarded))},
        Slice<u8>{output, static_cast<u32>(sizeof(output))});
    expect(header_expected.ok() && header_written.ok() && header_written.bytes == header_expected.bytes &&
               bytes_equal(ConstSlice<u8>{output, header_written.bytes},
                           ConstSlice<u8>{reinterpret_cast<const u8*>(header), header_expected.bytes}),
           "rest_user_super_properties_match_identify");

    guard_fill(guarded, sizeof(guarded), 0x5A);
    expect(gateway_write_user_identify(user_identify, Slice<u8>{guarded, expected_user_identify.count - 1})
                   .error == GatewayCommandError::output_too_small,
           "gateway_user_identify_output_bound");
    unchanged = guard_intact(guarded, sizeof(guarded), 0x5A);
    expect(unchanged, "gateway_user_identify_no_partial_secret");

    GatewayUserIdentify invalid_user_identify = user_identify;
    invalid_user_identify.properties.client_launch_id =
        utf8_literal("00112233-4455-6677-8899-aabbccddeefg");
    expect(gateway_write_user_identify(invalid_user_identify,
                                       Slice<u8>{output, static_cast<u32>(sizeof(output))})
                   .error == GatewayCommandError::invalid_input,
           "gateway_user_identify_launch_id_validation");
    invalid_user_identify = user_identify;
    invalid_user_identify.properties.client_build_number = 0;
    expect(gateway_write_user_identify(invalid_user_identify,
                                       Slice<u8>{output, static_cast<u32>(sizeof(output))})
                   .error == GatewayCommandError::invalid_input,
           "gateway_user_identify_build_required");
    invalid_user_identify = user_identify;
    invalid_user_identify.status = static_cast<GatewayUserStatus>(0xFF);
    expect(gateway_write_user_identify(invalid_user_identify,
                                       Slice<u8>{output, static_cast<u32>(sizeof(output))})
                   .error == GatewayCommandError::invalid_input,
           "gateway_user_identify_status_validation");
    invalid_user_identify = user_identify;
    invalid_user_identify.properties.browser_user_agent =
        Utf8View{invalid_utf8, static_cast<u32>(sizeof(invalid_utf8))};
    expect(gateway_write_user_identify(invalid_user_identify,
                                       Slice<u8>{output, static_cast<u32>(sizeof(output))})
                   .error == GatewayCommandError::invalid_utf8,
           "gateway_user_identify_utf8_validation");
    char oversized_user_token[kGatewayUserMaximumTokenBytes + 1]{};
    guard_fill(reinterpret_cast<u8*>(oversized_user_token), sizeof(oversized_user_token), 'x');
    invalid_user_identify = user_identify;
    invalid_user_identify.token =
        Utf8View{oversized_user_token, static_cast<u32>(sizeof(oversized_user_token))};
    expect(gateway_write_user_identify(invalid_user_identify,
                                       Slice<u8>{output, static_cast<u32>(sizeof(output))})
                   .error == GatewayCommandError::value_too_large,
           "gateway_user_identify_token_bound");
    char maximum_escaped_user_token[kGatewayUserMaximumTokenBytes]{};
    guard_fill(reinterpret_cast<u8*>(maximum_escaped_user_token), sizeof(maximum_escaped_user_token), '\n');
    invalid_user_identify = user_identify;
    invalid_user_identify.token =
        Utf8View{maximum_escaped_user_token, static_cast<u32>(sizeof(maximum_escaped_user_token))};
    expect(gateway_write_user_identify(invalid_user_identify,
                                       Slice<u8>{guarded, static_cast<u32>(sizeof(guarded))})
                   .error == GatewayCommandError::value_too_large,
           "gateway_user_identify_command_bound");
    unchanged = guard_intact(guarded, sizeof(guarded), 0x5A);
    expect(unchanged, "gateway_user_identify_command_bound_atomic");

    GatewayUserIdentify idle_user_identify = user_identify;
    idle_user_identify.since_ms = 987654321;
    idle_user_identify.status = GatewayUserStatus::idle;
    idle_user_identify.has_since = true;
    idle_user_identify.afk = true;
    const ConstSlice<u8> expected_idle_user_identify = byte_literal(
        R"({"op":2,"d":{"token":"user\"\n-token","capabilities":1734653,"properties":{"os":"Windows","browser":"Discord Client","release_channel":"stable","client_version":"1.0.9256","os_version":"10.0.19045","os_version_is_release":true,"os_sdk_version":"19045","os_arch":"x64","app_arch":"x64","system_locale":"en-US","has_client_mods":false,"client_launch_id":"00112233-4455-6677-8899-aabbccddeeff","browser_user_agent":"Agent\\1.0\nNative","browser_version":"42.9.0","client_build_number":603738,"client_event_source":null,"client_heartbeat_session_id":null,"is_fast_connect":false,"gateway_connect_reasons":"COLD_START"},"presence":{"status":"idle","since":987654321,"activities":[],"afk":true},"compress":false,"client_state":{"guild_versions":{}}}})");
    const GatewayCommandWriteResult idle_user_identified = gateway_write_user_identify(
        idle_user_identify, Slice<u8>{output, expected_idle_user_identify.count});
    expect(idle_user_identified.ok() && idle_user_identified.bytes == expected_idle_user_identify.count &&
               bytes_equal(ConstSlice<u8>{output, idle_user_identified.bytes}, expected_idle_user_identify),
           "gateway_user_identify_presence_write");

    const GatewayResume resume{utf8_literal("t\\"), utf8_literal("s\"\n"), ~0ull};
    const ConstSlice<u8> expected_resume =
        byte_literal(R"({"op":6,"d":{"token":"t\\","session_id":"s\"\n","seq":18446744073709551615}})");
    const GatewayCommandWriteResult resumed =
        gateway_write_resume(resume, Slice<u8>{output, expected_resume.count});
    expect(resumed.ok() && resumed.bytes == expected_resume.count &&
               bytes_equal(ConstSlice<u8>{output, resumed.bytes}, expected_resume),
           "gateway_resume_write");
    expect(gateway_write_resume(resume, Slice<u8>{output, expected_resume.count - 1}).error ==
               GatewayCommandError::output_too_small,
           "gateway_resume_output_bound");
}

void test_gateway_session() noexcept {
    u8 input_storage[8192]{};
    u8 message_storage[4096]{};
    u32 structural_scratch[512]{};
    const ConstSlice<u8> key = byte_literal("dGhlIHNhbXBsZSBub25jZQ==");
    GatewaySession session{};
    expect(gateway_session_initialize(
               &session, Slice<u8>{input_storage, static_cast<u32>(sizeof(input_storage))},
               Slice<u8>{message_storage, static_cast<u32>(sizeof(message_storage))},
               Slice<u32>{structural_scratch,
                          static_cast<u32>(sizeof(structural_scratch) / sizeof(structural_scratch[0]))},
               key, 4096, 4096, false, false, 0),
           "gateway_session_initialize");
    static CookieJar gateway_cookies;
    session.cookies = &gateway_cookies;
    session.cookie_request = {utf8_literal("gateway.discord.gg"), utf8_literal("/?encoding=json&v=9"), false};
    expect(gateway_session_next(&session, 0, 1000).kind == GatewaySessionEventKind::need_input,
           "gateway_session_handshake_need_input");

    const ConstSlice<u8> response =
        byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                     "Set-Cookie: __cf_bm=bot; HttpOnly; SameSite=None; Secure; Path=/; Domain=discord.gg; "
                     "Max-Age=1800\r\n\r\n");
    bool handshake_prefixes_wait = true;
    GatewaySessionEvent opened{};
    for (u32 index = 0; index < response.count; ++index) {
        if (!gateway_session_push(&session, ConstSlice<u8>{response.data + index, 1})) {
            handshake_prefixes_wait = false;
            break;
        }
        opened = gateway_session_next(&session, 0, 1000);
        if (index + 1 < response.count && opened.kind != GatewaySessionEventKind::need_input) {
            handshake_prefixes_wait = false;
            break;
        }
    }
    expect(handshake_prefixes_wait && opened.kind == GatewaySessionEventKind::opened && session.opened &&
               session.lifecycle.phase == GatewayLifecyclePhase::awaiting_hello,
           "gateway_session_fragmented_opening");
    u8 gateway_cookie[kCookieHeaderMaximumBytes];
    const u32 gateway_cookie_bytes =
        cookie_jar_header(&gateway_cookies, session.cookie_request, clock_unix_milliseconds(),
                          Slice<u8>{gateway_cookie, static_cast<u32>(sizeof(gateway_cookie))});
    expect(gateway_cookies.count == 1 &&
               bytes_equal(ConstSlice<u8>{gateway_cookie, gateway_cookie_bytes}, byte_literal("__cf_bm=bot")),
           "gateway_session_opening_stores_cookies");
    reset_value(&gateway_cookies);

    const ConstSlice<u8> hello_payload = byte_literal(R"({"op":10,"d":{"heartbeat_interval":45000}})");
    u8 frames[512]{};
    u32 frames_bytes =
        write_server_frame(WebSocketOpcode::text, false, ConstSlice<u8>{hello_payload.data, 12},
                           Slice<u8>{frames, static_cast<u32>(sizeof(frames))});
    frames_bytes += write_server_frame(
        WebSocketOpcode::ping, true, byte_literal("p"),
        Slice<u8>{frames + frames_bytes, static_cast<u32>(sizeof(frames)) - frames_bytes});
    frames_bytes += write_server_frame(
        WebSocketOpcode::continuation, true,
        ConstSlice<u8>{hello_payload.data + 12, hello_payload.count - 12},
        Slice<u8>{frames + frames_bytes, static_cast<u32>(sizeof(frames)) - frames_bytes});
    expect(gateway_session_push(&session, ConstSlice<u8>{frames, frames_bytes}),
           "gateway_session_push_fragmented_hello");
    const GatewaySessionEvent ping = gateway_session_next(&session, 0, 1000);
    expect(ping.kind == GatewaySessionEventKind::ping && ping.control_payload.count == 1 &&
               ping.control_payload[0] == 'p',
           "gateway_session_interleaved_ping");
    const GatewaySessionEvent hello = gateway_session_next(&session, 0, 1000);
    expect(hello.kind == GatewaySessionEventKind::hello && hello.hello.heartbeat_interval_ms == 45000 &&
               hello.lifecycle_step.action == GatewayLifecycleAction::send_identify,
           "gateway_session_fragmented_hello");

    u8 frame[1024]{};
    u32 frame_bytes = write_server_frame(WebSocketOpcode::text, true, byte_literal(R"({"op":11,"d":null})"),
                                         Slice<u8>{frame, static_cast<u32>(sizeof(frame))});
    expect(gateway_session_push(&session, ConstSlice<u8>{frame, frame_bytes}) &&
               gateway_session_next(&session, 0, 1000).kind ==
                   GatewaySessionEventKind::heartbeat_acknowledged,
           "gateway_session_heartbeat_ack");

    const ConstSlice<u8> ready_payload = byte_literal(
        R"({"op":0,"d":{"v":10,"user":{"id":"1"},"guilds":[],"session_id":"session","resume_gateway_url":"wss://gateway.discord.gg","application":{}},"s":7,"t":"READY"})");
    frame_bytes = write_server_frame(WebSocketOpcode::text, true, ready_payload,
                                     Slice<u8>{frame, static_cast<u32>(sizeof(frame))});
    expect(gateway_session_push(&session, ConstSlice<u8>{frame, frame_bytes}),
           "gateway_session_push_ready");
    const GatewaySessionEvent ready = gateway_session_next(&session, 0, 1000);
    expect(ready.kind == GatewaySessionEventKind::ready && ready.ready.api_version == 10 &&
               session.lifecycle.phase == GatewayLifecyclePhase::ready &&
               session.lifecycle.last_sequence == 7 && session.lifecycle.resume_available,
           "gateway_session_ready");

    frame_bytes = write_server_frame(WebSocketOpcode::text, true, byte_literal(R"({"op":7,"d":null})"),
                                     Slice<u8>{frame, static_cast<u32>(sizeof(frame))});
    expect(gateway_session_push(&session, ConstSlice<u8>{frame, frame_bytes}),
           "gateway_session_push_reconnect");
    const GatewaySessionEvent reconnect = gateway_session_next(&session, 0, 1000);
    expect(reconnect.kind == GatewaySessionEventKind::reconnect_requested &&
               reconnect.lifecycle_step.action == GatewayLifecycleAction::reconnect_resume,
           "gateway_session_reconnect_resume");

    const u8 close_payload[] = {0x0F, 0xA9};
    frame_bytes = write_server_frame(WebSocketOpcode::close, true, ConstSlice<u8>{close_payload, 2},
                                     Slice<u8>{frame, static_cast<u32>(sizeof(frame))});
    expect(gateway_session_push(&session, ConstSlice<u8>{frame, frame_bytes}),
           "gateway_session_push_close");
    const GatewaySessionEvent closed = gateway_session_next(&session, 0, 1000);
    expect(closed.kind == GatewaySessionEventKind::closed && closed.close_code == 4009 &&
               closed.lifecycle_step.action == GatewayLifecycleAction::reconnect_identify,
           "gateway_session_close_policy");

    // zstd-stream sessions: tests/protocol/gateway/etf_session.cpp covers framing and errors.
    u8 compressed_input[8192]{};
    u8 compressed_accumulation[4096]{};
    u32 compressed_scratch[512]{};
    static ZstdStreamDecoder compressed_stream{};
    static u8 compressed_stream_storage[kZstdMessageSlackBytes + 64 * 1024];
    GatewaySession compressed_session{};

    // A compressed payload larger than the initial compressed storage and the
    // input queue: binary frames stream into an owner spill (one large frame,
    // or fragments around a ping) and the session returns to its own storage.
    {
        static u8 spill_storage[24 * 1024];
        static u8 large_payload[16 * 1024];
        static u8 large_frames[20 * 1024];
        static u8 large_output[16 * 1024];
        struct Spill {
            u32 grows, releases, committed;
        };
        static Spill spill{};
        const auto grow = [](void* context, u32 required, Slice<u8>* backed) noexcept {
            auto& owner = *static_cast<Spill*>(context);
            if (!required) {
                ++owner.releases;
                owner.committed = 0;
                return true;
            }
            if (required > sizeof(spill_storage)) return false;
            ++owner.grows;
            owner.committed = required;
            *backed = {spill_storage, required};
            return true;
        };
        // {"op":10,"d":{"heartbeat_interval":41250},"pad":"aaaa..."} as ETF in raw zstd blocks.
        u8 json[10 * 1024];
        u32 json_bytes = 0;
        const auto put = [&](const char* text) noexcept { while (*text) json[json_bytes++] = static_cast<u8>(*text++); };
        put("{\"op\":10,\"d\":{\"heartbeat_interval\":41250},\"pad\":\"");
        while (json_bytes < 9000) json[json_bytes++] = 'a';
        put("\"}");
        static u8 term[10 * 1024];
        const EtfResult packed = etf_from_json({reinterpret_cast<const char*>(json), json_bytes}, {term, sizeof(term)});
        constexpr u8 frame_header[]{0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x30};
        u32 payload_bytes = sizeof(frame_header);
        copy_bytes(large_payload, frame_header, sizeof(frame_header));
        for (u32 offset = 0; packed.ok() && offset < packed.bytes;) {
            const u32 block = minimum(packed.bytes - offset, 4000u);
            const u32 header = block << 3; // raw, not the frame's last
            large_payload[payload_bytes++] = static_cast<u8>(header);
            large_payload[payload_bytes++] = static_cast<u8>(header >> 8);
            large_payload[payload_bytes++] = static_cast<u8>(header >> 16);
            copy_bytes(large_payload + payload_bytes, term + offset, block);
            payload_bytes += block;
            offset += block;
        }

        bool streamed = true;
        for (u32 layout = 0; layout < 2 && streamed; ++layout) {
            spill = {};
            SecureZeroMemory(compressed_accumulation, sizeof(compressed_accumulation));
            streamed = gateway_session_initialize(
                           &compressed_session, Slice<u8>{compressed_input, static_cast<u32>(sizeof(compressed_input))},
                           Slice<u8>{large_output, static_cast<u32>(sizeof(large_output))},
                           Slice<u32>{compressed_scratch,
                                      static_cast<u32>(sizeof(compressed_scratch) / sizeof(compressed_scratch[0]))},
                           key, 4096, 4096, false, false, 0) &&
                       zstd_stream_initialize(&compressed_stream,
                                              {compressed_stream_storage, sizeof(compressed_stream_storage)},
                                              sizeof(compressed_stream_storage), nullptr, nullptr) &&
                       gateway_session_enable_zstd_etf(
                           &compressed_session, &compressed_stream,
                           Slice<u8>{compressed_accumulation, static_cast<u32>(sizeof(compressed_accumulation))}) &&
                       gateway_session_enable_compressed_growth(&compressed_session, sizeof(spill_storage), grow,
                                                                &spill) &&
                       gateway_session_push(&compressed_session, response) &&
                       gateway_session_next(&compressed_session, 0, 2'000).kind == GatewaySessionEventKind::opened;
            u32 wire_bytes = 0;
            if (layout == 0) {
                wire_bytes = write_server_frame(WebSocketOpcode::binary, true, ConstSlice<u8>{large_payload, payload_bytes},
                                            Slice<u8>{large_frames, static_cast<u32>(sizeof(large_frames))});
            } else {
                const u32 split = 3000;
                constexpr u8 ping_payload[]{'h', 'i'};
                wire_bytes = write_server_frame(WebSocketOpcode::binary, false, ConstSlice<u8>{large_payload, split},
                                            Slice<u8>{large_frames, static_cast<u32>(sizeof(large_frames))});
                wire_bytes += write_server_frame(WebSocketOpcode::ping, true, ConstSlice<u8>{ping_payload, 2},
                                             Slice<u8>{large_frames + wire_bytes, static_cast<u32>(sizeof(large_frames)) - wire_bytes});
                wire_bytes += write_server_frame(WebSocketOpcode::continuation, true,
                                             ConstSlice<u8>{large_payload + split, payload_bytes - split},
                                             Slice<u8>{large_frames + wire_bytes, static_cast<u32>(sizeof(large_frames)) - wire_bytes});
            }
            u32 pings = 0, hellos = 0, errors = 0;
            for (u32 offset = 0; offset < wire_bytes && streamed;) {
                const u32 piece = minimum(wire_bytes - offset, 1000u);
                streamed = gateway_session_push(&compressed_session, ConstSlice<u8>{large_frames + offset, piece});
                offset += piece;
                for (;;) {
                    const GatewaySessionEvent next = gateway_session_next(&compressed_session, 0, 2'001);
                    if (next.kind == GatewaySessionEventKind::need_input) break;
                    pings += next.kind == GatewaySessionEventKind::ping;
                    hellos += next.kind == GatewaySessionEventKind::hello &&
                              next.hello.heartbeat_interval_ms == 41'250;
                    errors += next.kind == GatewaySessionEventKind::error;
                    if (next.kind == GatewaySessionEventKind::error) break;
                }
            }
            streamed = streamed && wire_bytes > sizeof(compressed_input) && pings == layout && hellos == 1 && !errors &&
                       spill.grows && spill.releases == 1 && !spill.committed &&
                       compressed_session.compressed_bytes == 0 &&
                       compressed_session.compressed_storage.data == compressed_accumulation;
        }
        expect(packed.ok() && streamed, "gateway_session_compressed_payload_streams_beyond_input_and_spills");
        zstd_stream_clear(&compressed_stream);
    }

    GatewaySession bad_handshake{};
    u8 bad_input[512]{};
    u8 bad_message[128]{};
    u32 bad_scratch[64]{};
    expect(gateway_session_initialize(
               &bad_handshake, Slice<u8>{bad_input, static_cast<u32>(sizeof(bad_input))},
               Slice<u8>{bad_message, static_cast<u32>(sizeof(bad_message))},
               Slice<u32>{bad_scratch, static_cast<u32>(sizeof(bad_scratch) / sizeof(bad_scratch[0]))}, key,
               128, 128, false, false, 0),
           "gateway_session_bad_handshake_initialize");
    const ConstSlice<u8> rejected_response =
        byte_literal("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    expect(gateway_session_push(&bad_handshake, rejected_response),
           "gateway_session_push_rejected_handshake");
    const GatewaySessionEvent rejected = gateway_session_next(&bad_handshake, 0, 0);
    expect(rejected.kind == GatewaySessionEventKind::error &&
               rejected.error == GatewaySessionError::handshake &&
               rejected.handshake_error == WebSocketHandshakeError::invalid_status,
           "gateway_session_rejected_handshake");
}

} // namespace maicord

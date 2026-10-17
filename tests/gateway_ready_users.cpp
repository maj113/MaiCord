// Also built with MAICORD_READY_USERS_BENCHMARK=1 as the manual MaiCordReadyUsersBenchmark
// (cmake/benchmarks.cmake).
// Exercise READY's shipping private batching path without opening a connection.
#include "../src/app/gateway_client.cpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/protocol/writer.hpp"
#include "fixtures/gateway_growth.hpp"
#include "fixtures/oversized_guild.hpp"

namespace maicord {
namespace ready_probe {
GatewayClientRuntime runtime{};
GatewayClient client{};
GatewayUserRuntime growth_user{};
u8 json[128 * 1024], output[128 * 1024];
char rich[2048];
constexpr auto basic = utf8_literal("{\"id\":\"123\",\"username\":\"sample\",\"avatar\":null}");
constexpr auto escaped = utf8_literal("{\"id\":\"123\",\"username\":\"a\\u0062\\ud83d\\ude00\",\"global_name\":\"Name\\u00e9\",\"premium_type\":2}");

Utf8View rich_user() noexcept {
    ProtocolWriter w{{reinterpret_cast<u8*>(rich), sizeof(rich)}, 0};
    protocol_writer_append_literal(&w, "{\"id\":\"123\",\"username\":\"");
    for (u32 i = 0; i < 128; ++i) protocol_writer_append_byte(&w, 'u');
    protocol_writer_append_literal(&w, "\",\"global_name\":\"");
    for (u32 i = 0; i < 128; ++i) protocol_writer_append_byte(&w, 'n');
    protocol_writer_append_literal(&w, "\",\"bot\":true,\"public_flags\":65536,\"display_name_styles\":{\"font_id\":3,\"effect_id\":4,\"colors\":[16711680,255]},\"collectibles\":{\"nameplate\":{\"sku_id\":\"456\",\"asset\":\"");
    for (u32 i = 0; i < 128; ++i) protocol_writer_append_byte(&w, 'p');
    protocol_writer_append_literal(&w, "\"}},\"avatar_decoration_data\":{\"asset\":\"");
    for (u32 i = 0; i < 64; ++i) protocol_writer_append_byte(&w, 'd');
    protocol_writer_append_literal(&w, "\"},\"avatar\":\"00000000000000000000000000000000\",\"primary_guild\":{\"identity_guild_id\":\"789\",\"identity_enabled\":true,\"tag\":\"TEST\",\"badge\":\"00000000000000000000000000000000\"}}");
    return {rich, w.used};
}
JsonValue array(Utf8View user, u32 count) noexcept {
    ProtocolWriter w{{json, sizeof(json)}, 0};
    protocol_writer_append_byte(&w, '[');
    for (u32 i = 0; i < count; ++i) {
        if (i) protocol_writer_append_byte(&w, ',');
        if (!protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(user.data), user.bytes)) return {};
    }
    protocol_writer_append_byte(&w, ']');
    return {{reinterpret_cast<const char*>(json), w.used}, 0, JsonValueKind::array};
}
bool drain(u32 expected, bool rich_record) noexcept {
    u32 count = 0;
    GatewayClientEvent event{};
    for (;;) {
        const auto popped = spsc_mailbox_pop(&runtime.events, &event, {output, sizeof(output)});
        if (popped.error == SpscMailboxError::empty) break;
        if (!popped.ok() || event.dispatch_kind != GatewayClientDispatchKind::user_snapshot ||
            !event.state.record_count || event.state.record_count > 64 || event.sequence != 9 ||
            event.timestamp_ms != 10 || event.dispatch_payload_bytes != popped.payload_bytes) return false;
        u32 cursor = 0;
        for (u32 i = 0; i < event.state.record_count; ++i) {
            GatewayClientUserMutation user{};
            Utf8View name{}, avatar{}, username{};
            ConstSlice<u8> appearance{};
            if (!gateway_client_user_record({output, popped.payload_bytes}, &cursor, &user,
                    &name, &avatar, &username, &appearance) || user.id != 123) return false;
            if (rich_record) {
                UserAppearance decoded{};
                if (name.bytes != 128 || username.bytes != 128 || avatar.bytes != 32 ||
                    !user_appearance_read(appearance, user.id, 0, &decoded) ||
                    decoded.nameplate.bytes != 128 || decoded.decoration.bytes != 64 ||
                    decoded.name.font_id != 3 || decoded.name.effect_id != 4 ||
                    decoded.name.colors[0] != 16711680 || decoded.primary_guild_id != 789 || !decoded.bot)
                    return false;
                for (u32 j = 0; j < 128; ++j)
                    if (name.data[j] != 'n' || username.data[j] != 'u' || decoded.nameplate.data[j] != 'p') return false;
                for (u32 j = 0; j < 64; ++j) if (decoded.decoration.data[j] != 'd') return false;
            } else {
                const bool separate = (user.flags & 4) != 0;
                constexpr u8 unicode_name[]{'N','a','m','e',0xc3,0xa9};
                constexpr u8 unicode_user[]{'a','b',0xf0,0x9f,0x98,0x80};
                if (name.bytes != 6 || username.bytes != 6) return false;
                for (u32 j = 0; j < 6; ++j)
                    if (static_cast<u8>(name.data[j]) != (separate ? unicode_name[j] : static_cast<u8>("sample"[j])) ||
                        static_cast<u8>(username.data[j]) != (separate ? unicode_user[j] : static_cast<u8>("sample"[j]))) return false;
            }
            ++count;
        }
        if (cursor != popped.payload_bytes) return false;
    }
    return count == expected && !spsc_mailbox_payload_used(runtime.events);
}
bool batch(JsonValue records, u32 count, bool rich_record = false) noexcept {
    // Standalone batching has no outer envelope scan; prepare its fixture's
    // pinned array prefix just as the production session does before routing.
    auto scan = json_scan_structurals_avx1(records.source, main_index_view(&runtime));
    while (scan.error == JsonScanError::output_overflow && runtime.structural_scratch.capacity < kStructuralCount) {
        Slice<u32> backed{};
        if (!grow_main_index(&runtime, runtime.structural_scratch.capacity * 2, &backed)) return false;
        scan = json_scan_structurals_avx1(records.source, backed);
    }
    if (!scan.ok()) return false;
    const auto routed = publish_ready_state(&client, &runtime, records, ReadyRecordKind::user, 9, 10);
    return routed.route.ok() && !routed.publish_failed && drain(count, rich_record);
}
bool initialize() noexcept {
    if (!stable_storage_initialize(&runtime.structural_scratch, kStructuralCount,
                                  kInitialStructuralCount, sizeof(u32), alignof(u32))) return false;
    if (!stable_storage_initialize(&runtime.ready_record_index, kStructuralCount,
        kInitialStructuralCount, sizeof(u32), alignof(u32))) {
        release_main_index(&runtime);
        return false;
    }
    client.notification_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime.mailbox_space = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime.mailbox_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (client.notification_event && runtime.mailbox_space && runtime.mailbox_cancel) return true;
    if (runtime.mailbox_cancel) CloseHandle(runtime.mailbox_cancel);
    if (runtime.mailbox_space) CloseHandle(runtime.mailbox_space);
    if (client.notification_event) CloseHandle(client.notification_event);
    runtime.mailbox_cancel = runtime.mailbox_space = client.notification_event = nullptr;
    release_main_index(&runtime);
    stable_storage_release(&runtime.ready_record_index);
    return false;
}
void release() noexcept {
    CloseHandle(runtime.mailbox_cancel);CloseHandle(runtime.mailbox_space);CloseHandle(client.notification_event);
    runtime.mailbox_cancel = runtime.mailbox_space = client.notification_event = nullptr;
    release_main_index(&runtime);
    if (runtime.ready_record_index.arena.base)
        SecureZeroMemory(runtime.ready_record_index.arena.base, runtime.ready_record_index.arena.committed);
    stable_storage_release(&runtime.ready_record_index);
}
bool projection_packet(bool malformed) noexcept {
    ProtocolWriter writer{{json, sizeof(json)}, 0};
    if (!protocol_writer_append_literal(&writer,
        "{\"op\":0,\"t\":\"READY\",\"s\":88,\"d\":{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"self\"},"
        "\"guilds\":[],\"session_id\":\"worker-projection\",\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"users\":[")) return false;
    for (u32 i = 0; i < 65; ++i) {
        if (i && !protocol_writer_append_byte(&writer, ',')) return false;
        if (!protocol_writer_append_literal(&writer, "{\"id\":\"") ||
            !protocol_writer_append_u64(&writer, i + 2) ||
            !protocol_writer_append_literal(&writer, "\",\"username\":\"peer\"}")) return false;
    }
    if (!protocol_writer_append_literal(&writer, "],\"read_state\":{\"entries\":[{\"id\":\"90\"}")) return false;
    if (malformed && !protocol_writer_append_literal(&writer, ",{\"id\":\"90\"}")) return false;
    if (!protocol_writer_append_literal(&writer, "]},\"dense_probe\":[")) return false;
    for (u32 i = 0; i < 4096; ++i) {
        if (i && !protocol_writer_append_byte(&writer, ',')) return false;
        if (!protocol_writer_append_byte(&writer, '0')) return false;
    }
    if (!protocol_writer_append_literal(&writer, "]}}") || writer.used > 65535) return false;
    // Complete unmasked server text frame; no socket or app window is opened.
    output[0] = 0x81; output[1] = 126;
    output[2] = static_cast<u8>(writer.used >> 8); output[3] = static_cast<u8>(writer.used);
    copy_bytes(output + 4, json, writer.used);
    const u8 key[kWebSocketClientKeyBytes]{};
    release_main_index(&runtime);
    if (!stable_storage_initialize(&runtime.structural_scratch, kStructuralCount,
        kInitialStructuralCount, sizeof(u32), alignof(u32))) return false;
    if (!gateway_session_initialize(&runtime.session, {runtime.session_input, kSessionInputBytes},
        {runtime.handshake_output, kMessageBytes}, main_index_view(&runtime),
        {key, sizeof(key)}, kSessionInputBytes - 14, kMessageBytes, false, false, 0)) return false;
    runtime.session.grow_index = grow_main_index;
    runtime.session.grow_index_context = &runtime;
    runtime.session.opened = true;
    runtime.session.lifecycle.phase = GatewayLifecyclePhase::awaiting_session;
    runtime.identity = GatewayClientIdentity::normal_user;
    runtime.authenticated = true;
    runtime.stopping = false;
    runtime.self_user_id = runtime.resume_user_id = 77;
    runtime.resume = {};
    runtime.resume.sequence = 7;
    runtime.voice_snapshot.user_id = 77;
    runtime.failure_stage = GatewayClientFailureStage::none;
    return gateway_session_push(&runtime.session, {output, writer.used + 4});
}
void worker_projection(void (*check)(bool, const char*) noexcept) noexcept {
    client.state_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!client.state_event) { check(false, "READY worker projection state event"); return; }
    ReadyProjectionBinding binding{&client, &runtime, AccountRequirement::none};
    runtime.mailbox_deadline_ms = GetTickCount64();
    check(!ready_projection_work(&binding, GatewayReadyProjectionPhase::preflight) &&
        !ready_projection_work(&binding, GatewayReadyProjectionPhase::publication),
        "shipping READY guard applies the same absolute deadline to preflight and publication");
    check(projection_packet(true), "READY malformed-tail worker fixture initializes");
    const u64 before_failure = GetTickCount64();
    const auto failed = process_session(&client, &runtime, before_failure);
    GatewayClientEvent event{};
    bool reported = false, scheduled = false, prefix = false;
    for (u32 i = 0; i < 16 && spsc_mailbox_pop(&runtime.events, &event, {output, sizeof(output)}).ok(); ++i) {
        reported = reported || (event.kind == GatewayClientEventKind::reconnecting &&
                                event.error == GatewayClientError::session &&
                                event.failure_stage == GatewayClientFailureStage::ready_route);
        scheduled = scheduled || (event.kind == GatewayClientEventKind::reconnecting &&
                                  event.error == GatewayClientError::none);
        prefix = prefix || event.kind == GatewayClientEventKind::ready ||
                 event.kind == GatewayClientEventKind::dispatch;
    }
    // A READY that cannot be projected is never a logout: no READY prefix is
    // published, the categorical reason is reported, and a fresh IDENTIFY is
    // scheduled after the READY-failure floor instead of ending the worker.
    check(failed == PlaintextDrive::reconnecting && reported && scheduled && !prefix &&
        runtime.self_user_id == 77 && !runtime.resume.valid &&
        runtime.reconnecting && runtime.ready_failures == 1 &&
        runtime.reconnect_deadline_ms >= before_failure + gateway_ready_failure_delay(1),
        "shipping READY worker reports malformed final reads and re-identifies later without publishing a prefix");
    runtime.reconnecting = false;
    runtime.reconnect_deadline_ms = 0;
    runtime.ready_failures = 0;
    gateway_reconnect_reset(&runtime.reconnect_backoff);
    check(projection_packet(false), "READY canceled worker fixture initializes");
    SetEvent(runtime.mailbox_cancel);
    const auto cancelled_checkpoint = runtime.resume;
    const auto cancelled = process_session(&client, &runtime, GetTickCount64());
    const auto cancelled_empty = spsc_mailbox_pop(&runtime.events, &event, {output, sizeof(output)});
    check(cancelled == PlaintextDrive::failed && cancelled_empty.error == SpscMailboxError::empty &&
        runtime.self_user_id == 77 && runtime.voice_snapshot.user_id == 77 &&
        bytes_equal(&cancelled_checkpoint, &runtime.resume, sizeof(cancelled_checkpoint)),
        "shipping READY cancellation during preflight leaves old producer checkpoint and owners untouched");
    ResetEvent(runtime.mailbox_cancel);
    check(projection_packet(false), "READY accepted worker fixture initializes");
    const auto accepted = process_session(&client, &runtime, GetTickCount64());
    u32 completions = 0, batches = 0, users = 0;
    bool ordered = true, saw_complete = false;
    for (;;) {
        const auto popped = spsc_mailbox_pop(&runtime.events, &event, {output, sizeof(output)});
        if (popped.error == SpscMailboxError::empty) break;
        if (!popped.ok()) { ordered = false; break; }
        ordered = ordered && !saw_complete && event.sequence == 88 && event.has_sequence;
        if (event.dispatch_kind == GatewayClientDispatchKind::ready_state_complete) {
            ++completions; saw_complete = true;
        }
        if (event.dispatch_kind == GatewayClientDispatchKind::user_snapshot) {
            ++batches; users += event.state.record_count;
        }
    }
    check(accepted == PlaintextDrive::receive && ordered && completions == 1 && batches == 3 && users == 66 &&
        runtime.structural_scratch.capacity == 8192 && runtime.session.structural_scratch.count == 8192 &&
        runtime.resume.valid && runtime.resume.sequence == 88 && runtime.resume_user_id == 1 &&
        runtime.self_user_id == 1 && runtime.resume.session_id_bytes == 17 &&
        load_phase(&client) == GatewayClientPhase::ready,
        "shipping READY checkpoints fresh sequence only after ordered complete projection enters the mailbox");
    CloseHandle(client.state_event); client.state_event = nullptr;
}

// The worker's oversized guild record: routed into its reservation, handed to
// the consumer outside the ring, and decommitted by the consumer's pop.
u8 oversized_json[1024 * 1024], oversized_output[kGatewayClientGuildRecordCapacity];
void oversized_record(void (*check)(bool, const char*) noexcept) noexcept {
    namespace fixture = oversized_guild_fixture;
    constexpr u64 guild = 88'000'000;
    client.runtime = &runtime;
    runtime.owner = &client;
    runtime.self_user_id = 1;
    const bool reserved = reserved_bytes_initialize(&runtime.large_record, kGatewayClientGuildRecordCapacity);
    const Utf8View source = fixture::write({oversized_json, sizeof(oversized_json)}, guild, 1);
    GatewayEnvelope envelope{};
    envelope.data = {source, 0, JsonValueKind::object};
    envelope.event_name = {utf8_literal("GUILD_CREATE"), 0, false};
    envelope.sequence = 12;
    envelope.opcode = 0;
    envelope.has_sequence = envelope.has_event_name = true;
    const Slice<char> scratch{reinterpret_cast<char*>(runtime.handshake_output + kMessageBytes),
                              kHandshakeBytes - kMessageBytes};
    Slice<u32> index{};
    runtime.mailbox_deadline_ms = 0;
    const bool indexed = grow_main_index(&runtime, 1u << 18, &index);
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    runtime.mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
    const auto routed = indexed ? gateway_client_route_dispatch_large(envelope, index, 64, scratch, {}, 1,
                                                                      {acquire_large_record, &runtime}, &event, &payload)
                                : GatewayClientDispatchRouteResult{};
    const u32 record_bytes = static_cast<u32>(payload.count);
    check(reserved && indexed && source.bytes && routed.ok() && event.state.record_count == fixture::kChannels &&
              record_bytes > kGatewayClientDispatchPayloadCapacity &&
              payload.data == runtime.large_record.base && runtime.large_record.committed == 256 * 1024 &&
              !runtime.large_record_lease,
          "oversized guild: worker commits only the 256 KiB the record needs");
    event.timestamp_ms = 13;
    const u32 ring_before = spsc_mailbox_payload_used(runtime.events);
    check(publish_event_payload(&client, &runtime, event, payload) && runtime.large_record_lease == 1 &&
              spsc_mailbox_payload_used(runtime.events) == ring_before && spsc_mailbox_size(runtime.events) == 1,
          "oversized guild: the record is handed over outside the 128 KiB ring");
    settle_large_record(&runtime);
    runtime.mailbox_deadline_ms = GetTickCount64();
    Slice<char> busy{};
    check(runtime.large_record.committed == 256 * 1024 && !acquire_large_record(&runtime, 256 * 1024, &busy),
          "oversized guild: the worker cannot reuse its storage before the consumer took the record");
    GatewayClientEvent popped_event{};
    const auto small = gateway_client_pop_event_payload(&client, &popped_event, {output, sizeof(output)});
    check(small.error == GatewayClientPopError::output_too_small && small.payload_bytes == record_bytes &&
              runtime.large_record_lease == 1 && spsc_mailbox_size(runtime.events) == 1,
          "oversized guild: a small consumer buffer leaves the record queued with its size");
    const auto popped =
        gateway_client_pop_event_payload(&client, &popped_event, {oversized_output, sizeof(oversized_output)});
    u32 cursor = 0, position = 0, channels = 0;
    GatewayClientStateMutation channel{};
    Utf8View name{};
    ConstSlice<u8> overwrites{};
    while (popped.ok() && gateway_client_event_next_guild_channel(popped_event, {oversized_output, popped.payload_bytes},
                                                                   &cursor, &position, &channel, &name, &overwrites))
        channels += channel.overwrite_count == fixture::kOverwrites;
    MEMORY_BASIC_INFORMATION region{};
    check(popped.ok() && popped.payload_bytes == record_bytes && popped_event.state.id == guild &&
              channels == fixture::kChannels && !runtime.large_record_lease && !runtime.large_record.committed &&
              spsc_mailbox_size(runtime.events) == 0 &&
              VirtualQuery(runtime.large_record.base, &region, sizeof(region)) && region.State == MEM_RESERVE,
          "oversized guild: the consumer copies the whole record and decommits the worker storage");
    // A discarded oversized record releases the storage the same way.
    runtime.mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
    event = {};
    payload = {};
    const bool discarded =
        gateway_client_route_dispatch_large(envelope, index, 64, scratch, {}, 1, {acquire_large_record, &runtime},
                                            &event, &payload)
            .ok() &&
        publish_event_payload(&client, &runtime, event, payload) && gateway_client_pop_event(&client, &popped_event);
    check(discarded && !runtime.large_record_lease && !runtime.large_record.committed,
          "oversized guild: discarding the record decommits the worker storage");
    // The ordinary scratch reports the overflow instead of dropping parts.
    GatewayClientEvent limited{};
    ConstSlice<u8> limited_payload{};
    const auto old = gateway_client_route_dispatch(envelope, index, 64, scratch, {}, 1, &limited, &limited_payload);
    check(!old.ok() && old.payload_overflow && old.error == GatewayClientDispatchRouteError::payload_capacity,
          "oversized guild: the ordinary scratch reports an overflow the caller can retry");
    runtime.mailbox_deadline_ms = 0;
    reserved_bytes_release(&runtime.large_record);
    runtime.owner = nullptr;
    client.runtime = nullptr;
}

void failure_receipt(void (*check)(bool, const char*) noexcept) noexcept {
    client.state_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!client.state_event) { check(false, "Gateway terminal receipt state event"); return; }
    for (u32 full = 0; full < 2; ++full) {
        runtime.stopping = false;
        runtime.failed = false;
        runtime.connection.socket = INVALID_SOCKET;
        runtime.failure_stage = GatewayClientFailureStage::session_event;
        runtime.session_error = GatewaySessionError::compression;
        runtime.compression_error = ZlibInflateError::output_capacity;
        InterlockedExchange(&client.error, static_cast<LONG>(GatewayClientError::none));
        InterlockedExchange(&client.phase, static_cast<LONG>(GatewayClientPhase::ready));
        ResetEvent(client.state_event);
        ResetEvent(client.notification_event);
        ResetEvent(runtime.mailbox_cancel);
        GatewayClientEvent queued{};
        queued.kind = GatewayClientEventKind::heartbeat_acknowledged;
        bool filled = true;
        if (full) for (u32 i = 0; i < kGatewayClientEventCapacity; ++i)
            filled = spsc_mailbox_push(&runtime.events, queued, {}).ok() && filled;
        const u32 dropped = spsc_mailbox_load_u32(&runtime.events.dropped);
        fail_and_drain(&client, &runtime, GatewayClientError::session, 0);
        GatewayClientEvent receipt{};
        bool matching = true;
        const u32 expected = full ? kGatewayClientEventCapacity : 1;
        for (u32 i = 0; i < expected; ++i) {
            const auto popped = spsc_mailbox_pop(&runtime.events, &receipt, {output, sizeof(output)});
            matching = matching && popped.ok() && !popped.payload_bytes && (full ?
                receipt.kind == GatewayClientEventKind::heartbeat_acknowledged :
                receipt.kind == GatewayClientEventKind::failed && receipt.failure_stage == GatewayClientFailureStage::session_event &&
                receipt.session_error == GatewaySessionError::compression && receipt.compression_error == ZlibInflateError::output_capacity);
        }
        check(filled && matching && load_phase(&client) == GatewayClientPhase::failed &&
            load_error(&client) == GatewayClientError::session && WaitForSingleObject(client.state_event, 0) == WAIT_OBJECT_0 &&
            WaitForSingleObject(client.notification_event, 0) == WAIT_OBJECT_0 &&
            spsc_mailbox_load_u32(&runtime.events.dropped) == dropped + full &&
            spsc_mailbox_pop(&runtime.events, &receipt, {output, sizeof(output)}).error == SpscMailboxError::empty,
            "Gateway terminal status retains categorical receipt or full FIFO and wakes consumer after failure publication");
    }
    runtime.stopping = runtime.failed = false;
    ResetEvent(runtime.mailbox_cancel);
    CloseHandle(client.state_event);
    client.state_event = nullptr;
}

u32 index_growth_calls{};
bool counted_growth(void* context, u32 required, Slice<u32>* backed) noexcept {
    ++index_growth_calls;
    return grow_main_index(context, required, backed);
}
bool denied_growth(void*, u32, Slice<u32>*) noexcept {
    ++index_growth_calls;
    return false;
}
bool wrong_base_growth(void* context, u32 required, Slice<u32>* backed) noexcept {
    ++index_growth_calls;
    if (!grow_main_index(context, required, backed)) return false;
    ++backed->data;
    return true;
}

u32 dense_packet(u32 values, bool malformed, bool compressed) noexcept {
    ProtocolWriter writer{{json, sizeof(json)}, 0};
    if (!protocol_writer_append_literal(&writer, "{\"op\":0,\"t\":\"DENSE_PROBE\",\"s\":99,\"d\":[")) return 0;
    for (u32 i = 0; i < values; ++i) {
        if (i && !protocol_writer_append_byte(&writer, ',')) return 0;
        if (!protocol_writer_append_byte(&writer, '0')) return 0;
    }
    if (malformed ? !protocol_writer_append_literal(&writer, "]?}") :
                    !protocol_writer_append_literal(&writer, "]}")) return 0;
    u32 bytes = writer.used;
    if (compressed) {
        if (bytes > 65535) return 0;
        // One RFC1950 stored block plus Z_SYNC_FLUSH, with no final block.
        output[4] = 0x78; output[5] = 0x01; output[6] = 0;
        output[7] = static_cast<u8>(bytes); output[8] = static_cast<u8>(bytes >> 8);
        output[9] = static_cast<u8>(~bytes); output[10] = static_cast<u8>((~bytes) >> 8);
        copy_bytes(output + 11, json, bytes);
        const u8 flush[]{0, 0, 0, 0xff, 0xff};
        copy_bytes(output + 11 + bytes, flush, sizeof(flush));
        bytes += 12;
    } else {
        const u32 header = bytes > 65535 ? 10 : 4;
        copy_bytes(output + header, json, bytes);
    }
    output[0] = compressed ? 0x82 : 0x81;
    if (bytes > 65535) {
        output[1] = 127;
        for (u32 i = 0; i < 8; ++i) output[2 + i] = static_cast<u8>(static_cast<u64>(bytes) >> ((7 - i) * 8));
        return bytes + 10;
    }
    output[1] = 126;
    output[2] = static_cast<u8>(bytes >> 8); output[3] = static_cast<u8>(bytes);
    return bytes + 4;
}

bool index_session(GatewaySessionGrowIndex grow = counted_growth,
                   ZlibStreamInflater* inflater = nullptr) noexcept {
    const u8 key[kWebSocketClientKeyBytes]{};
    if (!gateway_session_initialize(&runtime.session, {runtime.session_input, kSessionInputBytes},
        {runtime.handshake_output, kMessageBytes}, main_index_view(&runtime), {key, sizeof(key)},
        kSessionInputBytes - 14, kMessageBytes, true, true, 7)) return false;
    runtime.session.grow_index = grow;
    runtime.session.grow_index_context = &runtime;
    if (inflater && !gateway_session_enable_zlib_stream(&runtime.session, inflater,
        {runtime.handshake_output + kMessageBytes, kHandshakeBytes - kMessageBytes})) return false;
    runtime.session.opened = true;
    runtime.session.lifecycle.phase = GatewayLifecyclePhase::ready;
    runtime.mailbox_deadline_ms = 0;
    index_growth_calls = 0;
    return true;
}

bool fresh_index() noexcept {
    release_main_index(&runtime);
    return stable_storage_initialize(&runtime.structural_scratch, kStructuralCount,
        kInitialStructuralCount, sizeof(u32), alignof(u32));
}

void release_message() noexcept {
    if (runtime.decoded_message.arena.base)
        SecureZeroMemory(runtime.decoded_message.arena.base, runtime.decoded_message.arena.committed);
    stable_storage_release(&runtime.decoded_message);
}

bool growing_session(u32 reserve = kGatewaySessionMaximumDecodedMessageBytes) noexcept {
    release_message();
    runtime.stopping = false;
    runtime.mailbox_deadline_ms = 0;
    runtime.identity = GatewayClientIdentity::normal_user;
    runtime.user = &growth_user;
    const u8 key[kWebSocketClientKeyBytes]{};
    return fresh_index() && stable_storage_initialize(&runtime.decoded_message, reserve,
        kInitialDecodedMessageBytes, sizeof(u8), alignof(u8)) && initialize_session(&runtime, key);
}

bool compressed_packet(ConstSlice<u8> bytes, u32 first = 0) noexcept {
    if (!bytes.data || !bytes.count || bytes.count > sizeof(output) - 4) return false;
    // Separate server binary frames exercise accumulation before Z_SYNC_FLUSH.
    const u32 part = first ? first : bytes.count;
    if (part > bytes.count) return false;
    output[0] = 0x82;
    const u32 header = part <= 125 ? 2 : 4;
    if (header == 2) output[1] = static_cast<u8>(part);
    else {
        output[1] = 126;
        output[2] = static_cast<u8>(part >> 8);
        output[3] = static_cast<u8>(part);
    }
    copy_bytes(output + header, bytes.data, part);
    return gateway_session_push(&runtime.session, {output, part + header});
}

void decoded_storage(void (*check)(bool, const char*) noexcept) noexcept {
    check(growing_session(), "Gateway growing native session initializes actual shipping adapter");
    if (!runtime.decoded_message.arena.base) return;
    auto* base = runtime.decoded_message.arena.base;
    runtime.session.opened = true;
    runtime.session.lifecycle.phase = GatewayLifecyclePhase::awaiting_hello;
    check(compressed_packet({kGatewayGrowthHello, sizeof(kGatewayGrowthHello)}), "Gateway compressed HELLO accepted");
    auto received = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(received.kind == GatewaySessionEventKind::hello && runtime.decoded_message.arena.committed == 4096 &&
        runtime.structural_scratch.arena.committed == 4096, "Gateway HELLO retains two one-page startup prefixes");
    runtime.session.lifecycle.phase = GatewayLifecyclePhase::awaiting_session;
    check(compressed_packet({kGatewayGrowthReady, sizeof(kGatewayGrowthReady)}, 17), "Gateway first READY compressed chunk accepted");
    check(gateway_session_next(&runtime.session, 0, GetTickCount64()).kind == GatewaySessionEventKind::need_input &&
        runtime.session.compressed_bytes == 17, "Gateway partial flush waits without decoding or growing output");
    check(compressed_packet({kGatewayGrowthReady + 17, sizeof(kGatewayGrowthReady) - 17}), "Gateway remaining READY compressed chunk accepted");
    received = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(received.kind == GatewaySessionEventKind::ready && received.envelope.data.source.bytes > 128 * 1024 &&
        runtime.decoded_message.arena.base == base && runtime.decoded_message.capacity == 512 * 1024 &&
        runtime.structural_scratch.capacity == 128 * 1024 && !growth_user.inflater.failed && !runtime.session.compressed_bytes,
        "Gateway actual compressed READY expands beyond 128 KiB and scans large dense tail once with stable banks");
    // This bank's reservation is bounded to 8 MiB by growing_session.
    const u32 committed = static_cast<u32>(runtime.decoded_message.arena.committed);
    check(compressed_packet({kGatewayGrowthFollow, sizeof(kGatewayGrowthFollow)}), "Gateway dictionary continuation frame accepted");
    received = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(received.kind == GatewaySessionEventKind::dispatch && received.envelope.sequence == 12 &&
        runtime.decoded_message.arena.committed == committed && !growth_user.inflater.failed,
        "Gateway dictionary continuation preserves flush history and message backing high-water mark");
    for (u32 mode = 0; mode < 3; ++mode) {
        check(growing_session(mode == 2 ? 4096 : kGatewaySessionMaximumDecodedMessageBytes), "Gateway failed growth owner initializes");
        runtime.session.opened = true;
        runtime.session.lifecycle.phase = GatewayLifecyclePhase::ready;
        runtime.session.lifecycle.has_sequence = true;
        runtime.session.lifecycle.last_sequence = 7;
        check(compressed_packet({kGatewayGrowthBound, sizeof(kGatewayGrowthBound)}), "Gateway growth boundary failure frame accepted");
        if (mode == 0) SetEvent(runtime.mailbox_cancel);
        if (mode == 1) runtime.mailbox_deadline_ms = GetTickCount64();
        received = gateway_session_next(&runtime.session, 0, GetTickCount64());
        ResetEvent(runtime.mailbox_cancel);
        runtime.mailbox_deadline_ms = 0;
        check(received.kind == GatewaySessionEventKind::error && received.error == GatewaySessionError::compression &&
            received.compression_error == ZlibInflateError::output_capacity && growth_user.inflater.failed &&
            runtime.decoded_message.arena.committed == 4096 && runtime.session.lifecycle.last_sequence == 7,
            "Gateway cancellation deadline or denied reserve fails at write boundary without dispatch or checkpoint mutation");
    }
    for (u32 excess = 0; excess < 2; ++excess) {
        check(growing_session(), "Gateway structural maximum native session initializes");
        runtime.session.opened = true;
        runtime.session.lifecycle.phase = GatewayLifecyclePhase::ready;
        runtime.session.lifecycle.has_sequence = true;
        runtime.session.lifecycle.last_sequence = 7;
        const ConstSlice<u8> bytes = excess ? ConstSlice<u8>{kGatewayGrowthExcessIndex, sizeof(kGatewayGrowthExcessIndex)} :
            ConstSlice<u8>{kGatewayGrowthExactIndex, sizeof(kGatewayGrowthExactIndex)};
        check(compressed_packet(bytes), "Gateway compressed structural boundary frame accepted within wire bound");
        received = gateway_session_next(&runtime.session, 0, GetTickCount64());
        // Past 1M offsets the index doubles once more instead of failing: a
        // large account's READY reaches this well within the 8 MiB message bound.
        const u32 capacity = excess ? 2 * 1024 * 1024 : 1024 * 1024;
        check(runtime.structural_scratch.capacity == capacity &&
            runtime.structural_scratch.arena.committed == capacity * sizeof(u32) && !growth_user.inflater.failed &&
            received.kind == GatewaySessionEventKind::dispatch && runtime.session.lifecycle.last_sequence == 99,
            "Gateway 1M and 1M+1 offset frames both dispatch, committing only the index each needs");
    }
    clear_user_transport(&runtime);
    bool wiped = true;
    for (u32 i = 0; wiped && i < runtime.decoded_message.arena.committed; ++i)
        wiped = runtime.decoded_message.arena.base[i] == 0;
    check(wiped && !growth_user.inflater.initialized, "Gateway reconnect clears committed decoded bytes and persistent dictionary");
    release_message();
    runtime.user = nullptr;
    check(fresh_index(), "Gateway decoded fixtures restore index baseline");
}

void allocation_cleanup(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayClient owned{};
    owned.runtime = VirtualAlloc(nullptr, kGatewayUserClientRuntimeBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!owned.runtime) { check(false, "Gateway cleanup fixture runtime allocates"); return; }
    auto* storage = static_cast<GatewayClientRuntime*>(owned.runtime);
    storage->allocation_bytes = kGatewayUserClientRuntimeBytes;
    const bool initialized = stable_storage_initialize(&storage->structural_scratch,
        kStructuralCount, kInitialStructuralCount, sizeof(u32), alignof(u32)) &&
        stable_storage_initialize(&storage->decoded_message, kGatewaySessionMaximumDecodedMessageBytes,
            kInitialDecodedMessageBytes, sizeof(u8), alignof(u8)) &&
        stable_storage_initialize(&storage->ready_record_index, kStructuralCount,
            kInitialStructuralCount, sizeof(u32), alignof(u32));
    const auto* reserved = storage->structural_scratch.arena.base;
    const auto* message_reserved = storage->decoded_message.arena.base;
    const auto* record_reserved = storage->ready_record_index.arena.base;
    MEMORY_BASIC_INFORMATION owner_info{}, index_info{}, message_info{}, record_info{};
    const bool measured_owner = VirtualQuery(owned.runtime, &owner_info, sizeof(owner_info)) == sizeof(owner_info);
    const bool measured_index = initialized && VirtualQuery(reserved, &index_info, sizeof(index_info)) == sizeof(index_info);
    const bool measured_message = initialized && VirtualQuery(message_reserved, &message_info, sizeof(message_info)) == sizeof(message_info);
    const bool measured_record = initialized && VirtualQuery(record_reserved, &record_info, sizeof(record_info)) == sizeof(record_info);
    check(measured_owner && measured_index && measured_message && measured_record && owner_info.State == MEM_COMMIT &&
        index_info.State == MEM_COMMIT && message_info.State == MEM_COMMIT && message_info.RegionSize == kInitialDecodedMessageBytes &&
        record_info.State == MEM_COMMIT && record_info.RegionSize == 4096 &&
        storage->decoded_message.arena.reserved == kGatewaySessionMaximumDecodedMessageBytes &&
        storage->ready_record_index.arena.reserved == kStructuralCount * sizeof(u32),
        "Gateway user owner and three one-page message/outer/record prefixes commit without eager maximum backing");
    release_runtime_allocation(&owned);
    check(initialized && VirtualQuery(owned.runtime, &owner_info, sizeof(owner_info)) &&
        owner_info.State == MEM_FREE && VirtualQuery(reserved, &index_info, sizeof(index_info)) &&
        index_info.State == MEM_FREE && VirtualQuery(message_reserved, &message_info, sizeof(message_info)) &&
        message_info.State == MEM_FREE && VirtualQuery(record_reserved, &record_info, sizeof(record_info)) && record_info.State == MEM_FREE,
        "Gateway common teardown releases separate message/outer/record reservations and runtime after owner initialization");
    owned.runtime = VirtualAlloc(nullptr, sizeof(GatewayClientRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!owned.runtime) { check(false, "Gateway partial cleanup fixture runtime allocates"); return; }
    storage = static_cast<GatewayClientRuntime*>(owned.runtime);
    storage->allocation_bytes = static_cast<u32>(sizeof(GatewayClientRuntime));
    const bool refused = !stable_storage_initialize(&storage->structural_scratch,
        kInitialStructuralCount, kStructuralCount, sizeof(u32), alignof(u32));
    release_runtime_allocation(&owned);
    check(refused && VirtualQuery(owned.runtime, &owner_info, sizeof(owner_info)) && owner_info.State == MEM_FREE,
        "Gateway common teardown accepts an uninitialized index after a partial-start failure");
    owned.runtime = VirtualAlloc(nullptr, sizeof(GatewayClientRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!owned.runtime) { check(false, "Gateway partial decoded owner runtime allocates"); return; }
    storage = static_cast<GatewayClientRuntime*>(owned.runtime);
    storage->allocation_bytes = static_cast<u32>(sizeof(GatewayClientRuntime));
    const bool index_live = stable_storage_initialize(&storage->structural_scratch,
        kStructuralCount, kInitialStructuralCount, sizeof(u32), alignof(u32));
    reserved = storage->structural_scratch.arena.base;
    const bool decoded_refused = !stable_storage_initialize(&storage->decoded_message,
        1, kInitialDecodedMessageBytes, sizeof(u8), alignof(u8));
    release_runtime_allocation(&owned);
    check(index_live && decoded_refused && VirtualQuery(reserved, &index_info, sizeof(index_info)) &&
        index_info.State == MEM_FREE && VirtualQuery(owned.runtime, &owner_info, sizeof(owner_info)) && owner_info.State == MEM_FREE,
        "Gateway decoded-bank initialization failure releases the already-live index and runtime owner");
    owned.runtime = VirtualAlloc(nullptr, sizeof(GatewayClientRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!owned.runtime) { check(false, "Gateway partial READY record owner runtime allocates"); return; }
    storage = static_cast<GatewayClientRuntime*>(owned.runtime);
    storage->allocation_bytes = static_cast<u32>(sizeof(GatewayClientRuntime));
    const bool prior_banks = stable_storage_initialize(&storage->structural_scratch,
        kStructuralCount, kInitialStructuralCount, sizeof(u32), alignof(u32)) &&
        stable_storage_initialize(&storage->decoded_message, kGatewaySessionMaximumDecodedMessageBytes,
            kInitialDecodedMessageBytes, sizeof(u8), alignof(u8));
    reserved = storage->structural_scratch.arena.base;
    message_reserved = storage->decoded_message.arena.base;
    const bool record_refused = !stable_storage_initialize(&storage->ready_record_index,
        1, kInitialStructuralCount, sizeof(u32), alignof(u32));
    release_runtime_allocation(&owned);
    check(prior_banks && record_refused && VirtualQuery(reserved, &index_info, sizeof(index_info)) &&
        index_info.State == MEM_FREE && VirtualQuery(message_reserved, &message_info, sizeof(message_info)) &&
        message_info.State == MEM_FREE && VirtualQuery(owned.runtime, &owner_info, sizeof(owner_info)) && owner_info.State == MEM_FREE,
        "Gateway READY record-bank initialization failure releases both prior banks and the runtime owner");
}

void index_storage(void (*check)(bool, const char*) noexcept) noexcept {
    check(runtime.structural_scratch.capacity == 1024 && runtime.structural_scratch.arena.committed == 4096 &&
        runtime.structural_scratch.arena.reserved == kGatewaySessionMaximumStructuralCount * sizeof(u32),
        "Gateway main index reserves one offset per decoded message byte with one initial committed page");
    u32 bytes = dense_packet(4096, false, false);
    check(bytes && index_session(), "Gateway grow/rescan fixture initializes");
    const auto* base = main_index_view(&runtime).data;
    check(gateway_session_push(&runtime.session, {output, bytes}), "Gateway dense frame is accepted once");
    const auto dense = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(dense.kind == GatewaySessionEventKind::dispatch && dense.envelope.sequence == 99 &&
        runtime.session.lifecycle.last_sequence == 99 && index_growth_calls == 3 &&
        main_index_view(&runtime).data == base && runtime.structural_scratch.capacity == 8192 &&
        runtime.structural_scratch.arena.committed == 32768 &&
        gateway_session_next(&runtime.session, 0, GetTickCount64()).kind == GatewaySessionEventKind::need_input,
        "Gateway envelope grows only its stable index and dispatches the consumed frame once");
    const auto committed = runtime.structural_scratch.arena.committed;
    check(index_session() && runtime.session.structural_scratch.count == 8192 &&
        runtime.structural_scratch.arena.committed == committed,
        "Gateway reconnect borrows the existing committed high-water prefix");
    check(fresh_index() && index_session(nullptr), "Gateway fixed-slice fixture initializes");
    bytes = dense_packet(4096, false, false);
    check(gateway_session_push(&runtime.session, {output, bytes}), "Gateway fixed slice receives dense frame");
    auto failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(failed.kind == GatewaySessionEventKind::error && failed.gateway_error == GatewayDecodeError::scan_failed &&
        runtime.structural_scratch.capacity == 1024 && runtime.session.lifecycle.last_sequence == 7 && !index_growth_calls,
        "Gateway callers without the adapter retain fixed-capacity errors and their checkpoint");
    runtime.json_scan_error = JsonScanError::none;
    check(failed.scan_error == JsonScanError::output_overflow &&
        fail_session_event(&runtime, failed) == PlaintextDrive::failed &&
        runtime.json_scan_error == JsonScanError::output_overflow,
        "Gateway envelope scan failures report the scan error to the client's diagnostics");
    check(index_session(denied_growth) && gateway_session_push(&runtime.session, {output, bytes}),
        "Gateway failed reserve fixture initializes");
    failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(failed.kind == GatewaySessionEventKind::error && index_growth_calls == 1 &&
        runtime.session.lifecycle.last_sequence == 7 && runtime.structural_scratch.capacity == 1024,
        "Gateway owner growth failure does not dispatch or mutate lifecycle sequence");
    check(index_session() && gateway_session_push(&runtime.session, {output, bytes}), "Gateway cancel growth fixture initializes");
    SetEvent(runtime.mailbox_cancel);
    failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    ResetEvent(runtime.mailbox_cancel);
    check(failed.kind == GatewaySessionEventKind::error && index_growth_calls == 1 &&
        runtime.session.lifecycle.last_sequence == 7 && runtime.structural_scratch.capacity == 1024,
        "Gateway cancellation rejects growth before lifecycle dispatch");
    check(index_session() && gateway_session_push(&runtime.session, {output, bytes}), "Gateway expired growth fixture initializes");
    runtime.mailbox_deadline_ms = GetTickCount64();
    failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    runtime.mailbox_deadline_ms = 0;
    check(failed.kind == GatewaySessionEventKind::error && index_growth_calls == 1 &&
        runtime.session.lifecycle.last_sequence == 7 && runtime.structural_scratch.capacity == 1024,
        "Gateway growth retains one absolute owner deadline before dispatch");
    check(index_session(wrong_base_growth) && gateway_session_push(&runtime.session, {output, bytes}),
        "Gateway invalid adapter fixture initializes");
    failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(failed.kind == GatewaySessionEventKind::error && index_growth_calls == 1 &&
        runtime.session.structural_scratch.data == main_index_view(&runtime).data &&
        runtime.session.lifecycle.last_sequence == 7,
        "Gateway rejects an adapter that moves the pinned index base");
    check(fresh_index() && index_session(), "Gateway malformed tail fixture initializes");
    bytes = dense_packet(4096, true, false);
    check(gateway_session_push(&runtime.session, {output, bytes}), "Gateway malformed dense frame accepted");
    failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(failed.kind == GatewaySessionEventKind::error && failed.gateway_error == GatewayDecodeError::invalid_json &&
        index_growth_calls == 3 && runtime.session.lifecycle.last_sequence == 7,
        "Gateway bounded growth reveals a malformed tail without dispatching its prefix");
    check(fresh_index() && index_session(), "Gateway expanded structural policy fixture initializes");
    bytes = dense_packet(40000, false, false);
    check(bytes && gateway_session_push(&runtime.session, {output, bytes}), "Gateway larger dense frame accepted within unchanged wire bound");
    const auto larger = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(larger.kind == GatewaySessionEventKind::dispatch && index_growth_calls == 6 &&
        runtime.structural_scratch.capacity == 65536 && runtime.structural_scratch.arena.committed == 256 * 1024 &&
        runtime.session.lifecycle.last_sequence == 99,
        "Gateway demand index accepts more than the old 32768-offset bound without eager maximum commit");
    check(fresh_index() && index_session(), "Gateway lexical error fixture initializes");
    constexpr auto control_json = utf8_literal("{\"op\":0,\"d\":\"\x01\"}");
    output[0] = 0x81; output[1] = static_cast<u8>(control_json.bytes);
    copy_bytes(output + 2, control_json.data, control_json.bytes);
    check(gateway_session_push(&runtime.session, {output, control_json.bytes + 2}), "Gateway lexical error frame accepted");
    failed = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(failed.kind == GatewaySessionEventKind::error && !index_growth_calls &&
        runtime.structural_scratch.capacity == 1024 && runtime.session.lifecycle.last_sequence == 7,
        "Gateway non-capacity scan errors do not request growth");
    static ZlibStreamInflater inflater{};
    check(fresh_index() && index_session(counted_growth, &inflater), "Gateway persistent inflater fixture initializes");
    bytes = dense_packet(4096, false, true);
    const u32 json_bytes = bytes - 16;
    check(bytes && gateway_session_push(&runtime.session, {output, bytes}), "Gateway one stored flush is accepted");
    const auto inflated = gateway_session_next(&runtime.session, 0, GetTickCount64());
    check(inflated.kind == GatewaySessionEventKind::dispatch && index_growth_calls == 3 &&
        !inflater.failed && inflater.window_bytes == json_bytes && inflater.window_cursor == json_bytes &&
        !runtime.session.compressed_bytes &&
        gateway_session_next(&runtime.session, 0, GetTickCount64()).kind == GatewaySessionEventKind::need_input,
        "Gateway index growth rescans immutable inflated bytes without replaying the persistent flush");
    zlib_stream_clear(&inflater);
    check(fresh_index(), "Gateway demand index fixture returns to one-page baseline");
}
}
void test_gateway_ready_users(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace ready_probe;
    if (!initialize()) { check(false, "ready user probe event"); return; }
    index_storage(check);
    decoded_storage(check);
    allocation_cleanup(check);
    const u32 counts[]{0, 1, 63, 64, 65, 129};
    for (u32 count : counts) check(batch(array(basic, count), count), "ready users preserve batch boundaries and record framing");
    check(batch(array(escaped, 65), 65), "ready users preserve escaped identity across batches");
    check(batch(array(rich_user(), 64), 64, true), "ready rich users retain maximum names and serialized appearance");
    constexpr auto invalid = utf8_literal("{\"id\":\"123\",\"username\":\"first\",\"username\":\"duplicate\"}");
    char bytes[2048];
    GatewayPayloadWriter writer{{bytes, sizeof(bytes)}, 7};
    auto result = gateway_append_user({invalid, 0, JsonValueKind::object}, main_index_view(&runtime), 64, &writer);
    check(!result.ok() && writer.used == 7, "user parse errors do not append partial records");
    writer = {{bytes, 8}, 0};
    result = gateway_append_user({basic, 0, JsonValueKind::object}, main_index_view(&runtime), 64, &writer);
    check(result.error == GatewayClientDispatchRouteError::payload_capacity && writer.used == 0,
          "user output capacity failure is atomic");
    writer = {{bytes, sizeof(bytes)}, 0};
    result = gateway_append_user({utf8_literal("\"123\""), 0, JsonValueKind::string}, {}, 64, &writer, true);
    GatewayClientUserMutation id_only{};
    Utf8View name{}, avatar{}, username{}; ConstSlice<u8> appearance{}; u32 cursor = 0;
    check(result.ok() && gateway_client_user_record({reinterpret_cast<const u8*>(bytes), writer.used},
          &cursor, &id_only, &name, &avatar, &username, &appearance) && id_only.id == 123 &&
          !name.bytes && !username.bytes && !appearance.count && cursor == writer.used,
          "id-only users do not read unused appearance storage");
    worker_projection(check);
    oversized_record(check);
    failure_receipt(check);
    release();
    check(!runtime.structural_scratch.arena.base && !runtime.structural_scratch.capacity &&
        !runtime.structural_scratch.reserved_capacity, "Gateway index teardown releases and clears the owner descriptor");
}

#if defined(MAICORD_READY_USERS_BENCHMARK)
bool benchmark_ready_users() noexcept {
    using namespace ready_probe;
    if (!initialize()) return false;
    const Clock clock = clock_initialize();
    const Utf8View fixtures[]{basic, escaped, rich_user()};
    for (u32 fixture = 0; fixture < 3; ++fixture) {
        const JsonValue value{fixtures[fixture], 0, JsonValueKind::object};
        constexpr u32 iterations = 200'000;
        u64 checksum = 0;
        u32 record_bytes = 0;
        const i64 begin = clock_now();
        for (u32 i = 0; i < iterations; ++i) {
            GatewayPayloadWriter writer{{reinterpret_cast<char*>(output), sizeof(output)}, 0};
            if (!gateway_append_user(value, main_index_view(&runtime), 64, &writer).ok()) return false;
            checksum += writer.used + output[0];
            record_bytes = writer.used;
        }
        const u64 elapsed = clock_microseconds(clock, begin, clock_now());
        u64 digest = 14695981039346656037ull;
        for (u32 i = 0; i < record_bytes; ++i) digest = (digest ^ output[i]) * 1099511628211ull;
        TraceRecord line("ready_user_decode");
        line.field("fixture", fixture); line.field("calls", iterations);
        line.field("us", elapsed); line.field("checksum", checksum); line.field("digest", digest);
        DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
    }
    const JsonValue records = array(basic, 64);
    constexpr u32 iterations = 5000;
    const i64 begin = clock_now();
    for (u32 i = 0; i < iterations; ++i) if (!batch(records, 64)) return false;
    TraceRecord line("ready_user_batch");
    line.field("calls", iterations); line.field("users", 64);
    line.field("us", clock_microseconds(clock, begin, clock_now()));
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
    release(); return true;
}
extern "C" void maicord_ready_users_entry() noexcept { ExitProcess(benchmark_ready_users() ? 0 : 1); }
#endif
} // namespace maicord

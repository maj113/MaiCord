#include "../src/app/gateway_ready_projection.hpp"
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/reserved_bytes.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/protocol/gateway_runtime.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace ready_projection_probe {
u32 arrays[32768], records[16384];
char payload[128 * 1024], header[4096];
u8 source[128 * 1024];
struct Probe {
    GatewayReadyProjectionPhase phase;
    u32 preflight_checks, publication_checks, admitted, users, batches, complete;
    u32 stop_preflight, stop_publication, fail_admission = ~u32{0};
    bool valid = true;
    u32 notification_complete, notification_incomplete;
    GatewayClientDispatchKind order[32];
    u32 batch_counts[8];
    u32 guild_creates;
    u8 content_region_flags;
    GatewayClientUserMutation self_content;
    GatewayClientStateMutation huge;
    u64 digest = 14695981039346656037ull;
};
bool work(void* opaque, GatewayReadyProjectionPhase phase) noexcept {
    auto& probe = *static_cast<Probe*>(opaque); probe.phase = phase;
    auto& count = phase == GatewayReadyProjectionPhase::preflight ? probe.preflight_checks : probe.publication_checks;
    const u32 stop = phase == GatewayReadyProjectionPhase::preflight ? probe.stop_preflight : probe.stop_publication;
    ++count; return !stop || count < stop;
}
bool publish(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> bytes) noexcept {
    auto& probe = *static_cast<Probe*>(opaque);
    probe.valid = probe.valid && probe.phase == GatewayReadyProjectionPhase::publication &&
        event.sequence == 88 && event.has_sequence && event.timestamp_ms == 99 &&
        event.dispatch_payload_bytes == bytes.count;
    if (probe.admitted == probe.fail_admission) return false;
    probe.digest = (probe.digest ^ static_cast<u8>(event.dispatch_kind) ^ (u64{event.state.record_count} << 8)) *
                   1099511628211ull;
    for (u32 i = 0; i < bytes.count; ++i) probe.digest = (probe.digest ^ bytes[i]) * 1099511628211ull;
    if (probe.admitted < 32) probe.order[probe.admitted] = event.dispatch_kind;
    ++probe.admitted;
    if (event.kind == GatewayClientEventKind::ready)
        probe.content_region_flags = event.state.account_content_flags;
    if (event.dispatch_kind == GatewayClientDispatchKind::ready_state_complete) ++probe.complete;
    if (event.kind == GatewayClientEventKind::dispatch && event.dispatch_kind == GatewayClientDispatchKind::guild_create) {
        ++probe.guild_creates;
        if (event.state.id == 500000) probe.huge = event.state;
    }
    if(event.dispatch_kind==GatewayClientDispatchKind::notification_snapshot_complete){++probe.notification_complete;
        if(event.notification.flags&gateway_notification_incomplete)++probe.notification_incomplete;}
    if(event.dispatch_kind==GatewayClientDispatchKind::guild_create&&event.notification_payload_bytes){
        GatewayClientEvent normal{};ConstSlice<u8>body{};
        probe.valid=probe.valid&&gateway_client_notification_trim(event,bytes,&normal,&body)&&
            body.count+event.notification_payload_bytes==bytes.count&&normal.notification_payload_bytes==0&&normal.state.id==event.state.id;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::user_snapshot) {
        u32 cursor = 0;
        for (u32 i = 0; i < event.state.record_count; ++i) {
            GatewayClientUserMutation mutation{}; Utf8View name{}, avatar{};
            probe.valid = gateway_client_user_record(bytes, &cursor, &mutation, &name, &avatar) && probe.valid;
            if (mutation.id == 1) probe.self_content = mutation;
            ++probe.users;
        }
        probe.valid = probe.valid && cursor == bytes.count && event.state.record_count <= 64;
        if (probe.batches < 8) probe.batch_counts[probe.batches] = event.state.record_count;
        ++probe.batches;
    }
    return true;
}
GatewayReadyProjectionScratch scratch() noexcept {
    return {{arrays, 32768}, {records, 16384}, {payload, sizeof(payload)}, {header, sizeof(header)}};
}
bool decode(Utf8View input, GatewayReady* ready) noexcept {
    return gateway_decode_ready({input, 0, JsonValueKind::object}, {arrays, 32768}, 64, ready).ok();
}
u64 hash(Utf8View input) noexcept {
    u64 value = 14695981039346656037ull;
    for (u32 i = 0; i < input.bytes; ++i) value = (value ^ static_cast<u8>(input.data[i])) * 1099511628211ull;
    return value;
}
Utf8View fixture(u32 users, Utf8View tail = utf8_literal(""), bool bad_last_user = false) noexcept {
    ProtocolWriter writer{{source, sizeof(source)}, 0};
    if (!protocol_writer_append_literal(&writer,
        "{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"self\"},\"session_id\":\"projection\","
        "\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"guilds\":[{\"id\":\"8\",\"unavailable\":true}],\"users\":[")) return {};
    for (u32 i = 0; i < users; ++i) {
        if (i && !protocol_writer_append_byte(&writer, ',')) return {};
        if (!protocol_writer_append_literal(&writer, "{\"id\":\"") ||
            !protocol_writer_append_u64(&writer, i + 2) ||
            !protocol_writer_append_literal(&writer, "\",\"username\":\"peer\"")) return {};
        if (bad_last_user && i + 1 == users &&
            !protocol_writer_append_literal(&writer, ",\"username\":\"duplicate\"")) return {};
        if (!protocol_writer_append_byte(&writer, '}')) return {};
    }
    if (!protocol_writer_append_literal(&writer, "],\"read_state\":{\"entries\":[{\"id\":\"90\",\"last_message_id\":\"99\",\"mention_count\":3}]},\"user_guild_settings\":{\"entries\":[],\"partial\":false,\"version\":1},\"notification_settings\":{\"flags\":0},\"user_settings_proto\":\"\"") ||
        !protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(tail.data), tail.bytes) ||
        !protocol_writer_append_byte(&writer, '}')) return {};
    return {reinterpret_cast<const char*>(source), writer.used};
}
GatewayReadyProjectionResult project(Utf8View input, Probe* probe) noexcept {
    GatewayReady ready{};
    if (!decode(input, &ready)) {
        GatewayReadyProjectionResult failed{};
        failed.route.error = GatewayClientDispatchRouteError::decode;
        return failed;
    }
    return gateway_ready_project(ready, scratch(), 88, 99, publish, work, probe);
}
struct RecordIndex {
    StableStorage storage;
    u32 calls;
    u32 reject_call;
    bool move_base;
};
bool grow_records(void* context, u32 required, Slice<u32>* backed) noexcept {
    auto& index = *static_cast<RecordIndex*>(context);
    if (++index.calls == index.reject_call || !stable_storage_grow(&index.storage, required)) return false;
    *backed = {stable_storage_data<u32>(index.storage), index.storage.capacity};
    if (index.move_base) ++backed->data;
    return true;
}
Utf8View dense_guild(bool malformed) noexcept {
    ProtocolWriter writer{{source, sizeof(source)}, 0};
    if (!protocol_writer_append_literal(&writer,
        "{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"self\"},\"session_id\":\"projection\","
        "\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"guilds\":[{\"id\":\"8\",\"unavailable\":true,\"dense_probe\":[")) return {};
    for (u32 i = 0; i < 25000; ++i) {
        if (i && !protocol_writer_append_byte(&writer, ',')) return {};
        if (!protocol_writer_append_byte(&writer, '0')) return {};
    }
    if (malformed) {
        if (!protocol_writer_append_literal(&writer, "],\"id\":\"9\"}]}")) return {};
    } else if (!protocol_writer_append_literal(&writer, "]}]}")) return {};
    return {reinterpret_cast<const char*>(source), writer.used};
}
void nested_record_growth(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayReady ready{};
    const auto input = dense_guild(false);
    check(decode(input, &ready), "READY dense nested guild fits the existing outer index and source byte bound");
    const auto immutable = hash(input);
    Probe fixed{};
    const auto failure = gateway_ready_project(ready, scratch(), 88, 99, publish, work, &fixed);
    check(!failure.ok() && !fixed.admitted && failure.failure_stage == GatewayClientFailureStage::ready_route &&
        failure.route.event_error == GatewayEventDecodeError::scan_failed &&
        failure.route.scan_error == JsonScanError::output_overflow && failure.route.read_error == JsonReadError::visitor_rejected,
        "READY fixed caller reproduces nested 16384-offset overflow before publication");
    for (u32 mode = 0; mode < 5; ++mode) {
        check(decode(dense_guild(mode == 3), &ready), "READY nested typed fixture resets immutable source per case");
        RecordIndex index{};
        check(stable_storage_initialize(&index.storage, kGatewaySessionMaximumStructuralCount,
            1024, sizeof(u32), alignof(u32)), "READY nested stable index reserves bounded 4 MiB with 4 KiB initial backing");
        if (!index.storage.arena.base) return;
        auto growing = scratch();
        growing.records = {stable_storage_data<u32>(index.storage), index.storage.capacity};
        growing.grow_records = grow_records;
        growing.grow_records_context = &index;
        auto* base = growing.records.data;
        index.reject_call = mode == 1 ? 1 : 0;
        index.move_base = mode == 2;
        Probe probe{};
        if (mode == 4) probe.stop_preflight = 2;
        const auto result = gateway_ready_project(ready, growing, 88, 99, publish, work, &probe);
        check(mode == 3 ? result.ok() && result.degraded_guilds == 1 && probe.complete == 1 && probe.valid &&
                !probe.guild_creates && result.degraded_route.event_error == GatewayEventDecodeError::duplicate_field :
            mode ? !result.ok() && !probe.admitted && !probe.complete &&
            (mode != 4 || result.interrupted && !index.calls && index.storage.arena.committed == 4096) :
            result.ok() && probe.complete == 1 && probe.valid && index.calls == 5 &&
            index.storage.capacity == 32768 && index.storage.arena.committed == 128 * 1024 &&
            stable_storage_data<u32>(index.storage) == base && hash(input) == immutable,
            "READY nested index grows only on failed typed scans; denial and moved backing publish no prefix; "
            "an unreadable guild is left unavailable while READY completes");
        if (!mode) {
            TraceRecord memory("gateway_ready_record_high_water");
            memory.field("reserved_bytes", index.storage.arena.reserved);
            memory.field("initial_committed_bytes", 4096);
            memory.field("committed_bytes", index.storage.arena.committed);
            memory.field("commit_count", index.storage.arena.commit_count);
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), memory.text, memory.used, &written, nullptr);
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
        }
        SecureZeroMemory(index.storage.arena.base, index.storage.arena.committed);
        stable_storage_release(&index.storage);
    }
}
Utf8View dense_channels(bool duplicate_last) noexcept {
    ProtocolWriter writer{{source, sizeof(source)}, 0};
    if (!protocol_writer_append_literal(&writer,
        "{\"v\":9,\"user\":{\"id\":\"1\",\"username\":\"self\"},\"session_id\":\"projection\","
        "\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"guilds\":[{\"id\":\"8\",\"name\":\"guild\","
        "\"owner_id\":\"1\",\"roles\":[],\"channels\":[")) return {};
    for (u32 channel = 0; channel < 2; ++channel) {
        if (channel && !protocol_writer_append_byte(&writer, ',')) return {};
        if (!protocol_writer_append_literal(&writer, "{\"id\":\"") ||
            !protocol_writer_append_u64(&writer, channel + 20) ||
            !protocol_writer_append_literal(&writer, "\",\"type\":0,\"permission_overwrites\":[")) return {};
        for (u32 i = 0; i < 200; ++i) {
            if (i && !protocol_writer_append_byte(&writer, ',')) return {};
            const u32 id = duplicate_last && channel == 1 && i == 199 ? 100 : i + 100;
            if (!protocol_writer_append_literal(&writer, "{\"id\":\"") ||
                !protocol_writer_append_u64(&writer, id) ||
                !protocol_writer_append_literal(&writer, "\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}")) return {};
        }
        // A field after the large array catches accidental parent-index writes.
        if (!protocol_writer_append_literal(&writer, "],\"name\":\"tail\"}")) return {};
    }
    if (!protocol_writer_append_literal(&writer, "]}]}")) return {};
    return {reinterpret_cast<const char*>(source), writer.used};
}
void indexed_channel_records(void (*check)(bool, const char*) noexcept) noexcept {
    for (u32 malformed = 0; malformed < 2; ++malformed) {
        const auto input = dense_channels(malformed != 0);
        const u64 immutable = hash(input);
        GatewayReady ready{};
        check(decode(input, &ready), "READY channels with200 overwrites each decode within existing outer and guild index bounds");
        Probe probe{};
        const auto projected = gateway_ready_project(ready, scratch(), 88, 99, publish, work, &probe);
        if (malformed) {
            // The official client keys overwrites by target: a repeated target
            // replaces the earlier entry instead of failing the guild.
            check(projected.ok() && probe.complete == 1 && probe.guild_creates == 1 && !projected.degraded_guilds &&
                hash(input) == immutable,
                "READY duplicate trailing overwrite keeps the guild with the last entry for that target");
            continue;
        }
        check(projected.ok() && probe.complete == 1 && probe.valid && hash(input) == immutable,
            "READY indexed nested channel parsing exceeds old512-offset scratch without new allocations or destructive publication");
        GatewayEnvelope envelope{};
        // The fixture contains one guild. Borrow it through the already-pinned
        // array index, then route its exact production record once more to
        // verify all copied permission records through public accessors.
        JsonValue guild{};
        const auto scan = json_scan_structurals_avx1(ready.guilds.source, {arrays, 32768});
        const auto read = json_visit_root_array(ready.guilds.source, {arrays, scan.count}, 64,
            +[](void* opaque, u32 index, JsonValue value) noexcept {
                if (index) return false;
                *static_cast<JsonValue*>(opaque) = value;
                return true;
            }, &guild);
        envelope.data = guild;
        envelope.event_name = {utf8_literal("READY_GUILD"), 0, false};
        envelope.opcode = 0; envelope.has_event_name = true;
        GatewayClientEvent event{}; ConstSlice<u8> copied{};
        const auto routed = gateway_client_route_dispatch(envelope, {records, 16384}, 64,
            {payload, sizeof(payload)}, {}, 1, &event, &copied);
        u32 cursor = 0, index = 0, overwrites = 0;
        bool exact = scan.ok() && read.ok() && routed.ok() && event.state.record_count == 2 && event.state.overwrite_count == 400;
        for (u32 channel = 0; exact && channel < 2; ++channel) {
            GatewayClientStateMutation mutation{}; Utf8View name{}; ConstSlice<u8> permissions{};
            exact = gateway_client_event_next_guild_channel(event, copied, &cursor, &index, &mutation, &name, &permissions) &&
                mutation.id == channel + 20 && mutation.guild_id == 8 && mutation.overwrite_count == 200 &&
                name.bytes == 4 && bytes_equal(name.data, "tail", 4);
            for (u32 i = 0; exact && i < mutation.overwrite_count; ++i) {
                GatewayClientPermissionOverwriteMutation overwrite{};
                exact = gateway_client_permission_overwrite_at(permissions, mutation.overwrite_count, i, &overwrite) &&
                    overwrite.id == i + 100 && overwrite.type == 0 && overwrite.allow == 1024 && !overwrite.deny;
                ++overwrites;
            }
        }
        check(exact && overwrites == 400 && hash(input) == immutable,
            "READY indexed channels preserve every overwrite and fields after large nested arrays across sibling records");
    }
}
// A READY shaped like a huge public community (~18M members) at the server's own
// per-guild limits: 500 channels of every guild type with eight overwrites
// each, 251 roles, 500 emoji, 60 stickers, 48 soundboard sounds, a recurring
// scheduled event, active threads, unknown features/levels and large counts.
u8 huge_source[1024 * 1024];
u32 huge_arrays[1u << 18], huge_records[1u << 18];
char huge_payload[kGatewayClientDispatchPayloadCapacity];
struct Json {
    ProtocolWriter writer;
    bool ok = true;
    Json& s(const char* text) noexcept { ok = ok && protocol_writer_append_c_string(&writer, text); return *this; }
    Json& n(u64 value) noexcept { ok = ok && protocol_writer_append_u64(&writer, value); return *this; }
    Json& q(u64 value) noexcept { return s("\"").n(value).s("\""); }
};
constexpr u64 kHugeGuild = 500000;
Utf8View huge_ready() noexcept {
    Json j{{{huge_source, sizeof(huge_source)}, 0}};
    j.s(R"({"v":9,"user":{"id":"1","username":"self"},"session_id":"projection",)"
        R"("resume_gateway_url":"wss://gateway.discord.gg","guilds":[{"id":)").q(kHugeGuild)
     .s(R"(,"name":"Huge Community","owner_id":"77","member_count":18000000,"approximate_member_count":18000000,)"
        R"("approximate_presence_count":3200000,"max_members":25000000,"premium_subscription_count":31337,)"
        R"("premium_tier":3,"large":true,"lazy":true,"verification_level":9,"explicit_content_filter":2,)"
        R"("default_message_notifications":1,"mfa_level":1,"nsfw_level":0,"afk_timeout":300,)"
        R"("vanity_url_code":"huge","incidents_data":null,"clan":null,"profile":null,"inventory_settings":null,)"
        R"("premium_progress_bar_enabled":true,"latest_onboarding_question_id":"123","activity_instances":[],)"
        R"("application_command_counts":{"1":12},"hub_type":null,"safety_alerts_channel_id":null,)"
        R"("max_stage_video_channel_users":300,"home_header":null,"description":")");
    for (u32 i = 0; i < 30; ++i) j.s("A very long community description sentence. ");
    j.s(R"(","features":["COMMUNITY","NEWS","DISCOVERABLE","PARTNERED","VERIFIED","MORE_EMOJI","GUILD_ONBOARDING",)"
        R"("GUILD_SERVER_GUIDE","SOUNDBOARD","FUTURE_FEATURE_2027",{"unexpected":true},42],"roles":[)");
    for (u32 i = 0; i < 251; ++i) {
        if (i) j.s(",");
        j.s(R"({"id":)").q(i ? kHugeGuild + 1000 + i : kHugeGuild).s(R"(,"name":"role )").n(i)
         .s(R"(","permissions":"2251799813685247","position":)").n(i).s(R"(,"color":)").n(i * 977)
         .s(R"(,"colors":{"primary_color":)").n(i * 977);
        // Role 1 is a gradient and role 2 holographic (enhanced role styles).
        if (i == 1) j.s(R"(,"secondary_color":16711680,"tertiary_color":null},)");
        else if (i == 2) j.s(R"(,"secondary_color":0,"tertiary_color":255},)");
        else j.s(R"(,"secondary_color":null,"tertiary_color":null},)");
        j.s(R"("hoist":false,"managed":false,"mentionable":false,"flags":0,"icon":null,"unicode_emoji":null,"tags":{}})");
    }
    j.s(R"(],"emojis":[)");
    for (u32 i = 0; i < 500; ++i) {
        if (i) j.s(",");
        j.s(R"({"id":)").q(kHugeGuild + 10000 + i).s(R"(,"name":"emoji_)").n(i)
         .s(i % 2 ? R"(","roles":[],"require_colons":true,"managed":false,"animated":true,"available":true})"
                  : R"(","roles":[],"require_colons":true,"managed":false,"animated":false,"available":true})");
    }
    j.s(R"(],"stickers":[)");
    for (u32 i = 0; i < 60; ++i) {
        if (i) j.s(",");
        j.s(R"({"id":)").q(kHugeGuild + 20000 + i)
         .s(R"(,"name":"sticker","tags":"x","type":2,"format_type":1,"available":true,"guild_id":)").q(kHugeGuild).s("}");
    }
    j.s(R"(],"soundboard_sounds":[)");
    for (u32 i = 0; i < 48; ++i) {
        if (i) j.s(",");
        j.s(R"({"sound_id":)").q(kHugeGuild + 30000 + i)
         .s(R"(,"name":"sound","volume":0.85,"emoji_id":null,"emoji_name":"x","available":true,"guild_id":)").q(kHugeGuild).s("}");
    }
    j.s(R"(],"guild_scheduled_events":[{"id":"9001","guild_id":)").q(kHugeGuild)
     .s(R"(,"channel_id":)").q(kHugeGuild + 100003)
     .s(R"(,"name":"Weekly stage","scheduled_start_time":"2026-10-10T18:00:00.000000+00:00","scheduled_end_time":null,)"
        R"("privacy_level":2,"status":1,"entity_type":1,"entity_metadata":null,"user_count":120000,)"
        R"("recurrence_rule":{"start":"2026-10-10T18:00:00.000000+00:00","end":null,"frequency":2,"interval":1,)"
        R"("by_weekday":[5],"by_n_weekday":null,"by_month":null,"by_month_day":null,"by_year_day":null,"count":null}}],)"
        R"("stage_instances":[],"voice_states":[],"threads":[)");
    for (u32 i = 0; i < 2; ++i) {
        if (i) j.s(",");
        j.s(R"({"id":)").q(kHugeGuild + 200000 + i).s(R"(,"type":11,"parent_id":)").q(kHugeGuild + 100001)
         .s(R"(,"owner_id":"2","name":"thread","message_count":4,"member_count":2,"rate_limit_per_user":0,)"
            R"("thread_metadata":{"archived":false,"auto_archive_duration":1440,"locked":false,)"
            R"("archive_timestamp":"2026-10-01T00:00:00.000000+00:00"},"flags":0})");
    }
    j.s(R"(],"channels":[)");
    constexpr u8 types[] = {0, 2, 5, 13, 15, 16, 0, 2, 0};
    for (u32 i = 0; i < 500; ++i) {
        if (i) j.s(",");
        const bool category = i % 10 == 0;
        const u8 type = category ? u8{4} : types[i % 9];
        j.s(R"({"version":1700000000000,"type":)").n(type).s(R"(,"topic":"Channel topic text","position":)").n(i)
         .s(R"(,"rate_limit_per_user":0,"nsfw":false,"name":"channel-)").n(i).s(R"(","id":)").q(kHugeGuild + 100000 + i)
         .s(R"(,"flags":0,"icon_emoji":{"id":null,"name":"x"},"theme_color":null,"last_message_id":)").q(kHugeGuild + 900000 + i);
        if (!category) j.s(R"(,"parent_id":)").q(kHugeGuild + 100000 + i / 10 * 10);
        if (type == 2 || type == 13) j.s(R"(,"bitrate":64000,"user_limit":0,"rtc_region":null,"voice_background_display":null)");
        if (type == 15 || type == 16)
            j.s(R"(,"available_tags":[{"id":"1","name":"help","moderated":false,"emoji_id":null,"emoji_name":"x"}],)"
                R"("default_reaction_emoji":{"emoji_id":null,"emoji_name":"x"},"default_sort_order":null,"default_forum_layout":0)");
        j.s(R"(,"permission_overwrites":[)");
        for (u32 o = 0; o < 8; ++o) {
            if (o) j.s(",");
            j.s(R"({"type":0,"id":)").q(o ? kHugeGuild + 1000 + o : kHugeGuild)
             .s(o ? R"(,"deny":"0","allow":"1049600"})" : R"(,"deny":"1024","allow":"0"})");
        }
        j.s("]}");
    }
    j.s(R"(]},{"id":"8","unavailable":true}]})");
    return j.ok ? Utf8View{reinterpret_cast<const char*>(huge_source), j.writer.used} : Utf8View{};
}
// Oversized guild records go to storage committed on demand, as in the worker.
ReservedBytes huge_large{};
u32 huge_acquires = 0;
bool acquire_huge(void*, u32 bytes, Slice<char>* storage) noexcept {
    ++huge_acquires;
    if (!reserved_bytes_commit(&huge_large, bytes)) return false;
    *storage = {reinterpret_cast<char*>(huge_large.base), huge_large.committed};
    return true;
}
GatewayReadyProjectionScratch huge_scratch(u32 payload_bytes, bool large = false) noexcept {
    GatewayReadyProjectionScratch scratch{{huge_arrays, 1u << 18}, {huge_records, 1u << 18}, {huge_payload, payload_bytes},
        {header, sizeof(header)}};
    if (large) scratch.large = {acquire_huge, nullptr};
    return scratch;
}
void huge_community_guild(void (*check)(bool, const char*) noexcept) noexcept {
    const auto input = huge_ready();
    GatewayReady ready{};
    check(input.bytes && gateway_decode_ready({input, 0, JsonValueKind::object}, {huge_arrays, 1u << 18}, 64, &ready).ok(),
        "huge community READY fixture decodes");
    // Route the guild alone with the former 128 KiB record capacity: this is
    // the owner's failure (route_detail=6 channel list, event_decode_error=2
    // invalid_data, json_read_error=17 visitor_rejected at an overwrite).
    JsonValue guild{};
    const auto scan = json_scan_structurals_avx1(ready.guilds.source, {huge_arrays, 1u << 18});
    const auto read = json_visit_root_array(ready.guilds.source, {huge_arrays, scan.count}, 64,
        +[](void* opaque, u32 index, JsonValue value) noexcept {
            if (!index) *static_cast<JsonValue*>(opaque) = value;
            return true;
        }, &guild);
    GatewayEnvelope envelope{};
    envelope.data = guild;
    envelope.event_name = {utf8_literal("READY_GUILD"), 0, false};
    envelope.opcode = 0; envelope.has_event_name = true;
    GatewayClientEvent event{}; ConstSlice<u8> copied{};
    const auto old_capacity = gateway_client_route_dispatch(envelope, {huge_records, 1u << 18}, 64,
        {huge_payload, 128 * 1024}, {}, 1, &event, &copied);
    // The record that crosses the limit may be a channel header or one of its
    // overwrites (then json_read_error=17 visitor_rejected at that overwrite).
    check(scan.ok() && read.ok() && !old_capacity.ok() && old_capacity.detail == 6 &&
        old_capacity.error == GatewayClientDispatchRouteError::payload_capacity &&
        old_capacity.event_error == GatewayEventDecodeError::invalid_data &&
        (old_capacity.read_error == JsonReadError::none ||
         (old_capacity.read_error == JsonReadError::visitor_rejected && old_capacity.error_offset != 0)),
        "huge community guild reproduces the 128 KiB channel-list overflow codes (detail 6, 2)");
    check(old_capacity.payload_overflow, "the channel-list overflow is reported as retryable with more room");
    const bool reserved = reserved_bytes_initialize(&huge_large, kGatewayClientGuildRecordCapacity);
    const auto full = gateway_client_route_dispatch_large(envelope, {huge_records, 1u << 18}, 64,
        {huge_payload, kGatewayClientDispatchPayloadCapacity}, {}, 1, {acquire_huge, nullptr}, &event, &copied);
    check(reserved && full.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
        event.state.id == kHugeGuild && event.state.record_count == 502 && event.state.overwrite_count == 4000 &&
        event.state.role_count == 251 && event.state.emojis_known && event.state.emoji_count == 500 &&
        copied.count > 128 * 1024 && copied.data == huge_large.base && huge_acquires == 1 &&
        huge_large.committed == 256 * 1024,
        "huge community guild fits one typed record at server limits, committing only 256 KiB of 1 MiB");
    GatewayClientRoleMutation flat{}, gradient{}, holographic{};
    const ConstSlice<u8> huge_view{copied.data, copied.count};
    check(full.ok() && gateway_client_event_guild_role(event, huge_view, 0, &flat) &&
              gateway_client_event_guild_role(event, huge_view, 1, &gradient) &&
              gateway_client_event_guild_role(event, huge_view, 2, &holographic) && !flat.secondary_color &&
              !flat.tertiary_color && gradient.color == 977 &&
              gradient.secondary_color == (0xFF0000u | kRoleColorPresent) && !gradient.tertiary_color &&
              holographic.secondary_color == kRoleColorPresent &&
              holographic.tertiary_color == (0x0000FFu | kRoleColorPresent),
          "enhanced role colours keep secondary and tertiary colours; null stays a flat colour, black stays present");
    reserved_bytes_decommit(&huge_large);
    // An ordinary guild never touches the large storage.
    huge_acquires = 0;
    const auto small_text = utf8_literal(R"({"id":"9","name":"small","owner_id":"1","roles":[],"channels":[]})");
    GatewayEnvelope small = envelope;
    small.data = {small_text, 0, JsonValueKind::object};
    check(gateway_client_route_dispatch_large(small, {huge_records, 1u << 18}, 64,
              {huge_payload, kGatewayClientDispatchPayloadCapacity}, {}, 1, {acquire_huge, nullptr}, &event, &copied).ok() &&
          !huge_acquires && !huge_large.committed && copied.data == reinterpret_cast<const u8*>(huge_payload),
        "an ordinary guild record stays in the ordinary scratch");

    // Projection with the former capacity degrades only that guild.
    Probe degraded{};
    const auto limited = gateway_ready_project(ready, huge_scratch(128 * 1024), 88, 99, publish, work, &degraded);
    check(limited.ok() && degraded.complete == 1 && degraded.valid && degraded.guild_creates == 1 &&
        limited.degraded_guilds == 1 && limited.degraded_route.detail == 6 &&
        limited.degraded_route.event_error == GatewayEventDecodeError::invalid_data &&
        (limited.degraded_route.read_error == JsonReadError::none ||
         limited.degraded_route.read_error == JsonReadError::visitor_rejected),
        "an oversized guild is left unavailable with its reason while READY and the session continue");
    Probe projected{};
    const auto result = gateway_ready_project(ready, huge_scratch(kGatewayClientDispatchPayloadCapacity, true), 88, 99,
        publish, work, &projected);
    check(result.ok() && !result.degraded_guilds && projected.complete == 1 && projected.valid &&
        projected.guild_creates == 2 && projected.huge.record_count == 502 && projected.huge.role_count == 251 &&
        huge_large.peak == 256 * 1024,
        "huge community READY projects every guild through the on-demand oversized record storage");
    reserved_bytes_release(&huge_large);
}
// Individually malformed or unknown-shaped records cost only themselves.
void tolerant_guild_records(void (*check)(bool, const char*) noexcept) noexcept {
    const auto text = utf8_literal(R"({"id":"8","name":"guild","owner_id":"99","roles":[],)"
        R"("verification_level":9,"nsfw_level":7,"afk_timeout":-5,"features":[1,"COMMUNITY"],"emojis":[{"id":"5"}],)"
        R"("channels":[)"
        R"({"id":"30","type":0,"name":"kept","position":-1,"flags":4294967297,"permission_overwrites":[)"
        R"({"id":"8","type":0,"allow":"0","deny":"1024"},{"id":"8","type":0,"allow":"0","deny":"2048"},)"
        R"({"id":"9","type":2,"allow":"1","deny":"0"},{"id":"10","type":1}]},)"
        R"({"id":"31","type":1,"name":"private type"},)"
        R"({"id":"32","type":0,"name":"member on text","member":{"id":"32","user_id":"1","flags":0}},)"
        R"({"id":"33","type":0,"name":"thread metadata on text","thread_metadata":{"archived":true}},)"
        R"({"id":"34","name":"no type"},)"
        R"({"id":"35","type":0,"guild_id":"999","name":"foreign"}]})");
    GatewayEnvelope envelope{};
    envelope.data = {text, 0, JsonValueKind::object};
    envelope.event_name = {utf8_literal("READY_GUILD"), 0, false};
    envelope.opcode = 0; envelope.has_event_name = true;
    GatewayClientEvent event{}; ConstSlice<u8> copied{};
    const auto routed = gateway_client_route_dispatch(envelope, {records, 16384}, 64, {payload, sizeof(payload)}, {}, 1,
        &event, &copied);
    u32 cursor = 0, index = 0;
    GatewayClientStateMutation first{}, second{}; Utf8View name{}; ConstSlice<u8> permissions{}, none{};
    GatewayClientPermissionOverwriteMutation role{}, member{};
    const bool exact = routed.ok() && event.state.record_count == 2 && event.state.overwrite_count == 2 &&
        !event.state.emojis_known &&
        gateway_client_event_next_guild_channel(event, copied, &cursor, &index, &first, &name, &permissions) &&
        first.id == 30 && first.overwrite_count == 2 && !(first.fields & gateway_client_state_field_position) &&
        gateway_client_permission_overwrite_at(permissions, 2, 0, &role) && role.id == 8 && role.deny == 2048 &&
        gateway_client_permission_overwrite_at(permissions, 2, 1, &member) && member.id == 10 && member.type == 1 &&
        !member.allow && !member.deny &&
        gateway_client_event_next_guild_channel(event, copied, &cursor, &index, &second, &name, &none) &&
        second.id == 32 && !second.thread_self_joined_known &&
        (second.fields & gateway_client_state_field_permission_overwrites) && !second.overwrite_count;
    check(exact, "unknown enum levels, odd feature entries and bad channel or overwrite records are skipped individually");
    GatewayClientEvent placeholder{};
    check(gateway_client_route_guild_placeholder({utf8_literal(R"({"id":"77","name":5})"), 0, JsonValueKind::object},
              {records, 16384}, 64, &placeholder) &&
          placeholder.dispatch_kind == GatewayClientDispatchKind::guild_create && placeholder.state.id == 77 &&
          placeholder.state.unavailable && !placeholder.dispatch_payload_bytes &&
          !gateway_client_route_guild_placeholder({utf8_literal(R"({"id":"77","id":"78"})"), 0, JsonValueKind::object},
              {records, 16384}, 64, &placeholder),
        "an unprojectable joined guild becomes an unavailable entry only when its id is unambiguous");
    check(gateway_ready_failure_delay(0) == 0 && gateway_ready_failure_delay(1) == 5'000 &&
          gateway_ready_failure_delay(2) == 10'000 && gateway_ready_failure_delay(8) == 600'000 &&
          gateway_ready_failure_delay(1000) == 600'000,
        "repeated READY failures back off to a 10 minute floor (bounded daily IDENTIFY count)");
}
bool channel_state_equal(const GatewayClientStateMutation& a, const GatewayClientStateMutation& b) noexcept {
#define SAME(field) (a.field == b.field)
    return SAME(id) && SAME(guild_id) && SAME(parent_id) && SAME(owner_id) && SAME(permissions) &&
        SAME(last_message_id) && SAME(member_list_id) && SAME(position) && SAME(flags) && SAME(color) &&
        SAME(record_count) && SAME(role_count) && SAME(member_role_count) && SAME(overwrite_count) &&
        SAME(emoji_count) && SAME(emoji_role_count) && SAME(name_bytes) && SAME(fields) && SAME(type) &&
        SAME(unavailable) && SAME(hoist) && SAME(managed) && SAME(icon_bytes) && SAME(emojis_known) &&
        SAME(member_list_id_known) && SAME(recipient_count) && SAME(recipients_known) &&
        SAME(private_nicknames_known) && SAME(upload_policy.floor_bytes) && SAME(upload_policy.nitro_bytes) &&
        SAME(upload_policy.guild_250_disabled) && SAME(upload_policy_known) && SAME(upload_features) &&
        SAME(upload_features_known) && SAME(thread_archived) && SAME(thread_archived_known) &&
        SAME(thread_locked) && SAME(thread_locked_known) && SAME(thread_self_joined) &&
        SAME(thread_self_joined_known) && SAME(read_state_partial) && SAME(content_policy_known) &&
        SAME(content_policy) && SAME(guild_banner) && SAME(rate_limit_per_user) && SAME(system_channel_id) &&
        SAME(afk_channel_id) && SAME(afk_timeout) && SAME(self_communication_disabled_until) &&
        SAME(guild_settings_known) && SAME(mfa_level) && SAME(verification_level) &&
        SAME(default_message_notifications) && SAME(explicit_content_filter) && SAME(role_flags) && SAME(self_member);
#undef SAME
}
// Fills the stack the next call will reuse, so a payload byte copied from an
// uninitialized local (struct padding included) differs between the two routes.
__declspec(noinline) void poison_stack(u8 pattern) noexcept {
    volatile u8 bytes[64 * 1024];
    for (u32 i = 0; i < sizeof(bytes); ++i) bytes[i] = pattern;
}
void channel_nickname_projection(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayClientEvent event{};
    ConstSlice<u8> output{};
    const auto route = [&](Utf8View text, u32 capacity = sizeof(payload)) noexcept {
        event = {};
        output = {};
        return gateway_client_route_channel_create({text, 0, JsonValueKind::object}, {records, 16384},
            64, {payload, capacity}, 1, &event, &output);
    };
    const auto text = utf8_literal(R"({"id":"20","type":3,"nicks":[{"id":"2","nick":"Fr\u00e9nd"},{"id":"3","nick":""}]})");
    u32 cursor = 0;
    GatewayClientUserMutation nickname{};
    Utf8View name{}, avatar{};
    check(route(text).ok() && event.state.private_nicknames_known && event.state.record_count == 1 &&
        gateway_client_user_record(output, &cursor, &nickname, &name, &avatar) && nickname.id == 2 &&
        utf8_equal(name, utf8_literal("Frénd")) && !avatar.bytes && cursor == output.count,
        "REST channel normalization converts wire nick objects to the existing nickname payload");
    check(route(utf8_literal(R"({"id":"20","type":3})")).ok() && !event.state.private_nicknames_known &&
        !event.state.record_count && !output.count &&
        route(utf8_literal(R"({"id":"20","type":3,"nicks":[]})")).ok() && event.state.private_nicknames_known &&
        !event.state.record_count && !output.count &&
        route(utf8_literal(R"({"id":"20","type":3,"nicks":null})")).ok() && event.state.private_nicknames_known &&
        !event.state.record_count && !output.count,
        "REST nickname normalization distinguishes an absent snapshot from empty and null snapshots");
    check(route(text, 1).error == GatewayClientDispatchRouteError::payload_capacity && !output.count,
        "nickname payload capacity failures publish no partial normalized output");
    for (u32 mode = 0; mode < 2; ++mode) {
        ProtocolWriter writer{{source, sizeof(source)}, 0};
        bool built = protocol_writer_append_literal(&writer, "{\"id\":\"20\",\"type\":3,\"nicks\":[");
        const u32 count = 256 + mode;
        for (u32 i = 0; i < count; ++i) {
            if (i) built = protocol_writer_append_byte(&writer, ',') && built;
            built = protocol_writer_append_literal(&writer, "{\"id\":\"") && built;
            built = protocol_writer_append_u64(&writer, 1000 + i) && built;
            built = protocol_writer_append_literal(&writer, "\",\"nick\":\"") && built;
            if (!mode) built = protocol_writer_append_byte(&writer, 'n') && built;
            built = protocol_writer_append_literal(&writer, "\"}") && built;
        }
        built = protocol_writer_append_literal(&writer, "]}") && built;
        const auto result = route({reinterpret_cast<const char*>(source), writer.used});
        check(built && (mode ? result.error == GatewayClientDispatchRouteError::payload_capacity && !output.count :
            result.ok() && event.state.record_count == 256 && event.state.private_nicknames_known),
            "nickname arrays accept 256 records and bound even empty nicknames at 257 entries");
    }
    for (u32 length = 128; length <= 129; ++length) {
        ProtocolWriter writer{{source, sizeof(source)}, 0};
        bool built = protocol_writer_append_literal(&writer, "{\"id\":\"20\",\"type\":3,\"nicks\":[{\"id\":\"2\",\"nick\":\"");
        for (u32 i = 0; i < length; ++i) built = protocol_writer_append_byte(&writer, 'n') && built;
        built = protocol_writer_append_literal(&writer, "\"}]}") && built;
        const auto result = route({reinterpret_cast<const char*>(source), writer.used});
        check(built && (length == 128 ? result.ok() && event.state.record_count == 1 :
            result.error == GatewayClientDispatchRouteError::decode && !output.count),
            "wire nickname decoding enforces the model's 128-byte UTF-8 storage bound");
    }
}
void channel_create_projection(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr Utf8View cases[] = {
        utf8_literal(R"({"id":"20","guild_id":"8","type":0,"name":"A\u00e9","position":3,"flags":16,"last_message_id":"99","rate_limit_per_user":5,"nsfw":true,"permission_overwrites":[{"id":"8","type":0,"allow":"1024","deny":"0"},{"id":"1","type":1,"allow":"0","deny":"2048"}]})"),
        utf8_literal(R"({"id":"20","guild_id":"8","parent_id":"10","type":10,"name":"news","thread_metadata":{"archived":false,"locked":true},"member":{"id":"20","user_id":"1","join_timestamp":"2026-10-05T12:00:00Z","flags":0}})"),
        utf8_literal(R"({"id":"20","guild_id":"8","parent_id":"10","type":11,"name":"public","thread_metadata":{"archived":true,"locked":false},"member":null})"),
        utf8_literal(R"({"id":"20","guild_id":"8","parent_id":"10","type":12,"name":"private","thread_metadata":null})"),
        utf8_literal(R"({"id":"20","guild_id":"8","parent_id":"10","type":11,"member":{"id":"20","user_id":"2","join_timestamp":"2026-10-05T12:00:00Z","flags":0}})"),
        utf8_literal(R"({"id":"20","guild_id":"8","parent_id":"10","type":11,"member":{"id":false}})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":0,"member":{"id":"20","user_id":"1","flags":0}})"),
        utf8_literal(R"({"id":"20","type":1,"name":null,"recipients":[{"id":"2","username":"other"}]})"),
        utf8_literal(R"({"id":"20","type":3,"name":"group","recipient_ids":["2","3"],"nicks":[{"id":"2","nick":"friend"}],"is_message_request":true})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":0,"name":"channel","permission_overwrites":[{"id":"8","type":0,"allow":"0","deny":"1"},{"id":"8","type":0,"allow":"0","deny":"2"}]})"),
        utf8_literal(R"({"guild_id":"8","type":0})"),
        utf8_literal(R"({"id":"20","guild_id":"8"})"),
        utf8_literal(R"({"id":"20","type":0})"),
        utf8_literal(R"({"id":"20","id":"21","guild_id":"8","type":0})"),
        utf8_literal(R"({"id":false,"guild_id":"8","type":0})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":0,"name":null})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":0,"thread_metadata":{"archived":true}})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":1})"),
        utf8_literal(R"({"id":"20","type":1,"permission_overwrites":[]})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":0,"name":"channel","permission_overwrites":[{"id":"8","type":7}]})"),
        utf8_literal(R"({"id":"20","guild_id":"8","type":0)"),
    };
    GatewayClientEvent seed{};
    seed.kind = GatewayClientEventKind::dispatch;
    seed.dispatch_kind = GatewayClientDispatchKind::guild_update;
    seed.timestamp_ms = 111; seed.sequence = 222; seed.has_sequence = true;
    seed.heartbeat_interval_ms = 333; seed.native_error = 444; seed.dispatch_payload_bytes = 555;
    seed.close_code = 666; seed.route_error_offset = 777; seed.route_detail = 888; seed.event_name_bytes = 9;
    seed.state.id = 999; seed.state.name_bytes = 13; seed.state.overwrite_count = 17;
    char expected_storage[4096], actual_storage[4096];
    const u8 sentinel = 0xA5;
    u32 successes = 0, failures = 0, overflows = 0, partial_failures = 0, joined = 0;
    for (const Utf8View text : cases) {
        for (u32 mode = 0; mode < 6; ++mode) {
            const u32 capacity = mode == 1 ? 2 : mode == 2 ? 0 : 4096;
            const u32 structural_count = mode == 3 ? 1 : 16384;
            const u32 depth = mode == 4 ? 1 : 64;
            const u64 self = mode == 5 ? 0 : 1;
            GatewayEnvelope envelope{};
            envelope.event_name = {utf8_literal("CHANNEL_CREATE"), 0, false};
            envelope.has_event_name = true;
            envelope.data = {text, 0, JsonValueKind::object};
            GatewayClientEvent expected = seed, actual = seed;
            ConstSlice<u8> expected_payload{&sentinel, 1}, actual_payload{&sentinel, 1};
            bool handled = false;
            for (u32 i = 0; i < sizeof(expected_storage); ++i) {
                expected_storage[i] = static_cast<char>(0xCD);
                actual_storage[i] = 0x5A;
            }
            poison_stack(0xCD);
            const auto reference = gateway_client_route_state_dispatch(envelope, {records, structural_count}, depth,
                {expected_storage, capacity}, self, &expected, &expected_payload, &handled);
            poison_stack(0x5A);
            const auto result = gateway_client_route_channel_create(envelope.data, {records, structural_count}, depth,
                {actual_storage, capacity}, self, &actual, &actual_payload);
            check(handled && reference.error == result.error && reference.event_error == result.event_error &&
                reference.scan_error == result.scan_error && reference.read_error == result.read_error &&
                reference.error_offset == result.error_offset && reference.detail == result.detail &&
                reference.payload_overflow == result.payload_overflow &&
                channel_state_equal(expected.state, actual.state) && expected.dispatch_kind == actual.dispatch_kind,
                "channel receipt matches general route errors, partial mutation, counters and dispatch kind");
            check(actual.timestamp_ms == seed.timestamp_ms && actual.sequence == seed.sequence &&
                actual.has_sequence == seed.has_sequence && actual.kind == seed.kind &&
                actual.heartbeat_interval_ms == seed.heartbeat_interval_ms && actual.native_error == seed.native_error &&
                actual.dispatch_payload_bytes == seed.dispatch_payload_bytes && actual.close_code == seed.close_code &&
                actual.route_error_offset == seed.route_error_offset && actual.route_detail == seed.route_detail &&
                actual.event_name_bytes == seed.event_name_bytes,
                "channel receipt preserves event metadata and caller-owned payload count");
            if (result.ok()) {
                ++successes;
                joined += actual.state.thread_self_joined_known && actual.state.thread_self_joined;
                check(expected_payload.count == actual_payload.count &&
                    bytes_equal(expected_payload.data, actual_payload.data, actual_payload.count),
                    "channel receipt preserves exact channel and private-channel payload bytes");
            } else {
                ++failures;
                overflows += result.payload_overflow;
                partial_failures += actual.state.id != seed.state.id;
                check(expected_payload.data == &sentinel && expected_payload.count == 1 &&
                    actual_payload.data == &sentinel && actual_payload.count == 1,
                    "failed channel receipt leaves payload output untouched");
            }
        }
    }
    check(successes && failures && overflows && partial_failures && joined,
        "channel receipt cases cover success, errors, overflow, partial state and self-member hydration");
    GatewayClientEvent event = seed;
    ConstSlice<u8> output{&sentinel, 1};
    const JsonValue data{cases[0], 0, JsonValueKind::object};
    check(gateway_client_route_channel_create(data, {records, 16384}, 64, {actual_storage, 4096}, 1,
              nullptr, &output).error == GatewayClientDispatchRouteError::invalid_input &&
        output.data == &sentinel && output.count == 1 &&
        gateway_client_route_channel_create(data, {records, 16384}, 64, {actual_storage, 4096}, 1,
              &event, nullptr).error == GatewayClientDispatchRouteError::invalid_input &&
        channel_state_equal(event.state, seed.state),
        "channel receipt rejects null outputs before it changes caller state");
}
struct SelfMemberProbe {
    ChatStateStore store;
    GuildRecord guilds[2];
    ChannelRecord channels[3];
    RoleRecord roles[4];
    PermissionOverwriteRecord overwrites[2];
    u16 guild_order[2], channel_order[3];
    u8 names[2048];
    u64 guild_ids[2];
    u32 guild_events, member_events, published, notification_guilds;
    bool applied;
};
SelfMemberProbe self_members;
bool reset_self_members() noexcept {
    clear_bytes(&self_members, sizeof(self_members));
    auto& p = self_members;
    p.applied = chat_state_store_initialize(&p.store, {p.guilds, 2}, {p.guild_order, 2}, {p.channels, 3},
                                            {p.channel_order, 3}, {p.names, sizeof(p.names)}) &&
                chat_state_store_bind_permissions(&p.store, {p.roles, 4}, {p.overwrites, 2});
    return p.applied;
}
bool publish_self_members(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> bytes) noexcept {
    auto& p = *static_cast<SelfMemberProbe*>(opaque);
    ++p.published;
    if (event.kind == GatewayClientEventKind::ready ||
        event.dispatch_kind == GatewayClientDispatchKind::guild_create ||
        event.dispatch_kind == GatewayClientDispatchKind::guild_member_update) {
        GatewayClientEvent ordinary = event;
        ConstSlice<u8> body = bytes;
        if (event.dispatch_kind == GatewayClientDispatchKind::guild_create && event.state.id == 8) {
            GatewayClientNotificationSidecar side{};
            ConstSlice<u8> rows{};
            GatewayClientNotificationThreadRecord sensitive{};
            p.applied =
                p.applied && event.notification_payload_bytes &&
                gateway_client_notification_trim(event, bytes, &ordinary, &body) &&
                !ordinary.notification_payload_bytes && ordinary.dispatch_payload_bytes == body.count &&
                body.count + event.notification_payload_bytes == bytes.count &&
                gateway_client_notification_sidecar(
                    {bytes.data + body.count, event.notification_payload_bytes}, &side, &rows) &&
                side.guild_id == 8 && (side.flags & gateway_notification_guild_default_known) &&
                (side.flags & gateway_notification_guild_default_mentions) &&
                (side.flags & gateway_notification_channel_age_baseline_complete) &&
                (side.known_gates & notification_gate_age_access) &&
                !(side.gates & notification_gate_age_access) && side.thread_count == 1 &&
                gateway_client_notification_thread_at(rows, 0, &sensitive) &&
                sensitive.flags == gateway_notification_channel_metadata &&
                sensitive.thread.thread_id == 801 && (sensitive.gates & notification_gate_age_access);
            ++p.notification_guilds;
        }
        p.applied =
            p.applied && chat_state_apply_gateway_event(&p.store, ordinary, body, {p.guild_ids, 2}).ok();
        p.guild_events += event.dispatch_kind == GatewayClientDispatchKind::guild_create;
        p.member_events += event.dispatch_kind == GatewayClientDispatchKind::guild_member_update;
    }
    return p.applied;
}
Utf8View self_member_ready(Utf8View merged, Utf8View tail = utf8_literal("")) noexcept {
    ProtocolWriter writer{{source, sizeof(source)}, 0};
    const bool ok =
        protocol_writer_append_literal(
            &writer,
            R"({"v":9,"user":{"id":"1","username":"self"},"session_id":"members",)"
            R"("resume_gateway_url":"wss://gateway.discord.gg","guilds":[)"
            R"({"id":"8","name":"First","owner_id":"99","default_message_notifications":1,"nsfw_level":0,"roles":[{"id":"8","permissions":"0"},{"id":"80","permissions":"3072"}],)"
            R"("channels":[{"id":"800","type":0,"name":"allowed","permission_overwrites":[]},)"
            R"({"id":"801","type":0,"name":"member-deny","nsfw":true,"permission_overwrites":[{"id":"1","type":1,"allow":"0","deny":"2048"}]}]},)"
            R"({"id":"9","name":"Second","owner_id":"99","roles":[{"id":"9","permissions":"3072"},{"id":"90","permissions":"0"}],)"
            R"("channels":[{"id":"900","type":0,"name":"role-deny","permission_overwrites":[{"id":"90","type":0,"allow":"0","deny":"1024"}]}]}],)"
            R"("merged_members":)") &&
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(merged.data), merged.bytes) &&
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(tail.data), tail.bytes) &&
        protocol_writer_append_literal(
            &writer,
            R"(,"user_guild_settings":{"entries":[],"partial":false},"notification_settings":{"flags":0},"user_settings_proto":""})");
    return ok ? Utf8View{reinterpret_cast<const char*>(source), writer.used} : Utf8View{};
}
GatewayReadyProjectionResult project_self_members(Utf8View merged) noexcept {
    GatewayReady ready{};
    if (!decode(self_member_ready(merged), &ready)) {
        GatewayReadyProjectionResult failed{};
        failed.route.error = GatewayClientDispatchRouteError::decode;
        return failed;
    }
    return gateway_ready_project(ready, scratch(), 88, 99, publish_self_members, nullptr, &self_members);
}
void ready_self_member_roles(void (*check)(bool, const char*) noexcept) noexcept {
    check(reset_self_members(), "READY self members initialize existing bounded permission stores");
    const auto projected = project_self_members(utf8_literal(
        R"([[{"user_id":"1","roles":["80"]}],[{"user_id":"2"},{"user_id":"1","roles":["90"]}]])"));
    auto& p = self_members;
    const auto* first = chat_state_store_find_guild(p.store, 8);
    const auto* second = chat_state_store_find_guild(p.store, 9);
    const auto* allowed = chat_state_store_find_channel(p.store, 800);
    const auto* member_denied = chat_state_store_find_channel(p.store, 801);
    const auto* role_denied = chat_state_store_find_channel(p.store, 900);
    const auto* first_role = chat_state_store_find_role(p.store, 8, 80);
    const auto* second_role = chat_state_store_find_role(p.store, 9, 90);
    check(projected.ok() && !projected.degraded_guilds && p.applied && p.guild_events == 2 &&
              p.notification_guilds == 1 && first && second && first->member_roles_known &&
              second->member_roles_known && first_role && first_role->self_assigned && second_role &&
              second_role->self_assigned && allowed && allowed->permissions_resolved &&
              allowed->effective_permissions == (kPermissionViewChannel | kPermissionSendMessages) &&
              member_denied && member_denied->effective_permissions == kPermissionViewChannel &&
              role_denied && role_denied->permissions_resolved &&
              !(role_denied->effective_permissions & kPermissionViewChannel),
          "READY merged-member projection retains defaults and sparse age metadata, applies the trimmed "
          "body, "
          "and honors role and member permissions");
    check(reset_self_members(), "READY missing-role fixture resets");
    const auto missing = project_self_members(
        utf8_literal(R"([[{"user_id":"1","flags":128}],[{"user_id":"2","roles":[]}]])"));
    first = chat_state_store_find_guild(p.store, 8);
    second = chat_state_store_find_guild(p.store, 9);
    allowed = chat_state_store_find_channel(p.store, 800);
    check(missing.ok() && p.applied && first && second && !first->member_roles_known &&
              !second->member_roles_known &&
              (first->settings.self_member & guild_self_member_quarantined) && allowed &&
              !allowed->permissions_resolved,
          "READY self without roles retains its observed flags while another member's empty roles never "
          "authorize self permissions");
    check(reset_self_members(), "READY empty-role fixture resets");
    const auto empty = project_self_members(utf8_literal(R"([[{"user":{"id":"1"},"roles":[]}],[]])"));
    first = chat_state_store_find_guild(p.store, 8);
    first_role = chat_state_store_find_role(p.store, 8, 80);
    allowed = chat_state_store_find_channel(p.store, 800);
    check(empty.ok() && p.applied && first && first->member_roles_known && first->permissions_resolved &&
              first_role && !first_role->self_assigned && allowed && allowed->permissions_resolved &&
              !allowed->effective_permissions,
          "READY an explicit empty self-role array is authoritative and grants none of the guild's named "
          "roles");
    check(reset_self_members(), "READY malformed-role fixture resets");
    const auto malformed = project_self_members(
        utf8_literal(R"([[{"user_id":"1","roles":["80",false]}],[{"user_id":"1","roles":["90"]}]])"));
    first = chat_state_store_find_guild(p.store, 8);
    second = chat_state_store_find_guild(p.store, 9);
    check(malformed.ok() && malformed.degraded_guilds == 1 && p.applied && first && first->unavailable &&
              !first->member_roles_known && second && second->member_roles_known && p.guild_events == 1,
          "READY a malformed self-role tail degrades only that guild and never publishes its valid role "
          "prefix");
    check(reset_self_members(), "READY misaligned-member fixture resets");
    const auto misaligned = project_self_members(utf8_literal(R"([[{"user_id":"1","roles":["80"]}]])"));
    first = chat_state_store_find_guild(p.store, 8);
    second = chat_state_store_find_guild(p.store, 9);
    check(misaligned.ok() && misaligned.degraded_guilds == 2 && p.applied && !p.guild_events && first &&
              second && first->unavailable && second->unavailable && !first->member_roles_known &&
              !second->member_roles_known,
          "READY a short parallel member array cannot bind partial or shifted role authority to any guild");
    check(reset_self_members(), "READY duplicate-member fixture resets");
    GatewayReady duplicate{};
    const auto duplicated = self_member_ready(utf8_literal(R"([[{"user_id":"1","roles":["80"]}],[]])"),
                                              utf8_literal(R"(,"merged_members":[])"));
    const bool duplicate_decoded = decode(duplicated, &duplicate);
    const auto duplicate_projected =
        gateway_ready_project(duplicate, scratch(), 88, 99, publish_self_members, nullptr, &p);
    first = chat_state_store_find_guild(p.store, 8);
    second = chat_state_store_find_guild(p.store, 9);
    check(duplicate_decoded && duplicate.merged_members.kind == JsonValueKind::null_value &&
              duplicate_projected.ok() && duplicate_projected.degraded_guilds == 2 && p.applied &&
              !p.guild_events && first && second && first->unavailable && second->unavailable &&
              !first->member_roles_known && !second->member_roles_known,
          "READY duplicate merged-member authority degrades the optional guild snapshots without failing "
          "the session or assigning roles");
}
void supplemental_self_member_roles(void (*check)(bool, const char*) noexcept) noexcept {
    check(reset_self_members(), "supplemental members reset the permission fixture");
    const auto seed = project_self_members(
        utf8_literal(R"([[{"user_id":"1","roles":["80"]}],[{"user_id":"1","roles":["90"]}]])"));
    auto& p = self_members;
    const auto supplement = [&](Utf8View text, u32 capacity = sizeof(payload)) noexcept {
        return gateway_ready_publish_supplemental_members(
            {text, 0, JsonValueKind::object}, 1, {arrays, 32768}, {records, 16384}, {payload, capacity}, 89,
            100, publish_self_members, &p);
    };
    const auto metadata = supplement(utf8_literal(
        R"({"guilds":[{"id":"8"},{"id":"9"}],"merged_members":[[{"user_id":"2","roles":[]}],[{"user_id":"1","flags":128}]]})"));
    const auto* first_role = chat_state_store_find_role(p.store, 8, 80);
    const auto* second_role = chat_state_store_find_role(p.store, 9, 90);
    const auto* second = chat_state_store_find_guild(p.store, 9);
    check(seed.ok() && metadata.ok() && p.applied && p.member_events == 1 && first_role &&
              first_role->self_assigned && second_role && second_role->self_assigned && second &&
              second->member_roles_known && guild_self_quarantined(second->settings),
          "supplemental other-member rows never clear self roles and self flags without roles preserve "
          "prior assignments");
    const auto cleared = supplement(utf8_literal(
        R"({"guilds":[{"id":"8"},{"id":"9"}],"merged_members":[[{"user_id":"1","roles":[]}],[{"user_id":"1","roles":["90",false]}]]})"));
    const auto* first = chat_state_store_find_guild(p.store, 8);
    const auto* channel = chat_state_store_find_channel(p.store, 800);
    check(cleared.ok() && p.applied && p.member_events == 2 && first && first->member_roles_known &&
              first_role && !first_role->self_assigned && channel && channel->permissions_resolved &&
              !channel->effective_permissions && second_role && second_role->self_assigned && second &&
              guild_self_quarantined(second->settings),
          "supplemental explicit empty self roles revoke authority while a malformed sibling row publishes "
          "no partial role prefix");
    const u32 published = p.published;
    const u32 revision = channel ? channel->revision : 0;
    const auto capacity = supplement(
        utf8_literal(R"({"guilds":[{"id":"8"}],"merged_members":[[{"user_id":"1","roles":["80"]}]]})"), 1);
    const auto mismatch = supplement(utf8_literal(
        R"({"guilds":[{"id":"8"},{"id":"9"}],"merged_members":[[{"user_id":"1","roles":["80"]}]]})"));
    check(capacity.ok() && mismatch.ok() && p.published == published && p.member_events == 2 &&
              first_role && !first_role->self_assigned && channel && channel->revision == revision,
          "supplemental role capacity failure and positional count mismatch publish nothing and preserve "
          "previously applied membership");
}
void account_content_metadata(void (*check)(bool, const char*) noexcept) noexcept {
    const Utf8View tails[]{
        utf8_literal(""), utf8_literal(R"(,"regional_feature_config":null)"),
        utf8_literal(R"(,"regional_feature_config":{})"),
        utf8_literal(R"(,"regional_feature_config":{"teen_by_default_settings":1,"age_gated_features":2})"),
        utf8_literal(R"(,"regional_feature_config":{"teen_by_default_settings":2,"age_gated_features":1})"),
        utf8_literal(R"(,"regional_feature_config":{"teen_by_default_settings":1,"teen_by_default_settings":0})"),
        utf8_literal(R"(,"regional_feature_config":{"age_gated_features":"2"})"),
        utf8_literal(R"(,"regional_feature_config":{"teen_by_default_settings":1},"regional_feature_config":{})"),
        utf8_literal(R"(,"linked_users":[{"link_status":2,"link_type":1}])"),
        utf8_literal(R"(,"linked_users":[{"link_status":2,"link_type":2},{"link_status":99,"link_type":1}])"),
        utf8_literal(R"(,"linked_users":[])"), utf8_literal(R"(,"linked_users":null)")};
    for (u32 i = 0; i < 12; ++i) {
        Probe probe{};
        const auto result = project(fixture(0, tails[i]), &probe);
        check(result.ok() && probe.valid && probe.complete == 1 &&
            probe.content_region_flags == (i == 3 ? 3 : i == 8 ? 4 : 0),
            "READY content policy: only documented regional bits normalize and absent or malformed ancillary data cannot retain stale flags");
    }
    constexpr auto own = utf8_literal(R"({"v":9,"user":{"id":"1","nsfw_allowed":false,"age_verification_status":3,"linked_users":[]},"guilds":[],"session_id":"projection","resume_gateway_url":"wss://gateway.discord.gg","regional_feature_config":{"teen_by_default_settings":1,"age_gated_features":2},"linked_users":[{"link_status":2,"link_type":1}]})");
    Probe probe{};
    check(project(own, &probe).ok() && probe.valid && probe.self_content.id == 1 &&
        probe.self_content.flags == 96 && probe.self_content.age_verification_status == 3 &&
        probe.self_content.content_fields == 1 && probe.content_region_flags == 7,
        "READY content policy: own eligibility and age proof stay independent while canonical top-level parent state wins");
    GatewayReady borrowed{};
    check(decode(own, &borrowed), "READY content policy: borrowed regional fixture decodes");
    borrowed.regional_feature_config.source = {payload, 2};
    Probe alias{};
    const auto rejected = gateway_ready_project(borrowed, scratch(), 88, 99, publish, work, &alias);
    check(!rejected.ok() && !alias.admitted,
        "READY content policy: borrowed regional policy cannot alias mutable projection scratch");
    check(decode(own, &borrowed), "READY content policy: linked alias fixture restores immutable source");
    borrowed.linked_users.source = {payload, 2};
    check(!gateway_ready_project(borrowed, scratch(), 88, 99, publish, work, &alias).ok() && !alias.admitted,
        "READY content policy: borrowed linked users cannot alias mutable projection scratch");
}
} // namespace ready_projection_probe

void test_gateway_ready_projection(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace ready_projection_probe;
    account_content_metadata(check);
    ready_self_member_roles(check);
    supplemental_self_member_roles(check);
    nested_record_growth(check);
    indexed_channel_records(check);
    huge_community_guild(check);
    tolerant_guild_records(check);
    channel_nickname_projection(check);
    channel_create_projection(check);
    const auto input = fixture(129);
    const u64 immutable = hash(input);
    Probe valid{};
    const auto success = project(input, &valid);
    check(success.ok() && success.phase == GatewayReadyProjectionPhase::publication && valid.valid &&
        valid.preflight_checks && valid.publication_checks && valid.users == 130 && valid.batches == 4 &&
        valid.batch_counts[0] == 1 && valid.batch_counts[1] == 64 && valid.batch_counts[2] == 64 &&
        valid.batch_counts[3] == 1 && valid.complete == 1 && hash(input) == immutable,
        "READY preflight is callback-free and immutable before valid users publish in existing 64-record batches");
    // An empty user_settings_proto still publishes: the official client loads
    // every setting at its default.
    check(valid.admitted == 15 && valid.order[5] == GatewayClientDispatchKind::notification_snapshot_begin &&
        valid.order[6] == GatewayClientDispatchKind::notification_global &&valid.order[7]==GatewayClientDispatchKind::notification_snapshot_complete&&
        valid.order[8] == GatewayClientDispatchKind::guild_folders &&
        valid.order[9] == GatewayClientDispatchKind::user_settings &&
        valid.order[10] == GatewayClientDispatchKind::guild_create &&
        valid.order[11] == GatewayClientDispatchKind::read_state_snapshot &&
        valid.order[12] == GatewayClientDispatchKind::relationship_snapshot_begin &&
        valid.order[13] == GatewayClientDispatchKind::relationship_snapshot_complete &&
        valid.order[14] == GatewayClientDispatchKind::ready_state_complete &&valid.notification_complete==1&&!valid.notification_incomplete,
        "READY projection keeps header users guilds reads relationships and completion order");

    // Publication replayed from the preflight's staged records matches
    // routing READY again: the same descriptors, bytes, order and guards.
    ReservedBytes staging{};
    check(reserved_bytes_initialize(&staging, 1024 * 1024), "READY staging reservation");
    GatewayReady staged_ready{};
    check(decode(input, &staged_ready), "READY staged fixture decodes");
    auto staged_scratch = scratch();
    staged_scratch.staging = &staging;
    Probe staged{};
    const auto replayed = gateway_ready_project(staged_ready, staged_scratch, 88, 99, publish, work, &staged);
    bool same_order = staged.admitted == valid.admitted;
    for (u32 i = 0; i < 32 && i < valid.admitted && same_order; ++i) same_order = staged.order[i] == valid.order[i];
    check(replayed.ok() && staged.valid && same_order && staged.digest == valid.digest && staged.users == valid.users &&
        staged.batches == valid.batches && staged.complete == 1 && staged.publication_checks >= staged.admitted &&
        staged.preflight_checks == valid.preflight_checks && staging.peak && !staging.committed &&
        hash(input) == immutable,
        "READY publication replays staged preflight records byte-for-byte and decommits the staging pages");
    Probe staged_stop{}; staged_stop.stop_publication = 6;
    const auto staged_partial = gateway_ready_project(staged_ready, staged_scratch, 88, 99, publish, work, &staged_stop);
    check(staged_partial.interrupted && staged_partial.publication_started && !staged_partial.completed &&
        staged_stop.admitted == 5 && !staged_stop.complete &&
        staged_partial.failure_stage == GatewayClientFailureStage::ready_publish && !staging.committed,
        "staged READY publication stops at its guard with the admitted prefix and releases staging");
    reserved_bytes_release(&staging);

    Probe late_user{};
    const auto user_failure = project(fixture(65, {}, true), &late_user);
    check(!user_failure.ok() && !user_failure.interrupted && !user_failure.completed &&
        user_failure.phase == GatewayReadyProjectionPhase::preflight && !late_user.admitted &&
        user_failure.failure_stage == GatewayClientFailureStage::ready_user_route,
        "malformed user after a complete candidate batch publishes no READY or user prefix");
    const Utf8View malformed_tails[]{
        utf8_literal(",\"private_channels\":[{\"id\":\"90\",\"type\":1,\"recipients\":[{\"id\":\"2\",\"username\":\"peer\"}]},{\"id\":\"91\",\"type\":99}]"),
        utf8_literal(",\"relationships\":[{\"id\":\"2\",\"type\":1},{\"id\":\"3\",\"type\":99}]"),
        utf8_literal(",\"relationships\":[{\"id\":\"2\",\"type\":1}],\"merged_presences\":{\"friends\":[{\"user_id\":\"2\",\"status\":\"online\"},{\"user_id\":\"3\"}]}"),
    };
    for (auto tail : malformed_tails) {
        Probe rejected{};
        const auto failure = project(fixture(65, tail), &rejected);
        check(!failure.ok() && !failure.completed && failure.phase == GatewayReadyProjectionPhase::preflight &&
            !rejected.admitted && !rejected.complete,
            "malformed final private relationship or presence child publishes no semantic READY prefix");
    }
    const auto bad_guild = utf8_literal(R"({"v":9,"user":{"id":"1","username":"self"},"session_id":"projection","resume_gateway_url":"wss://gateway.discord.gg","guilds":[{"id":"8","unavailable":true},{"id":"9","unavailable":false}]})");
    Probe guild{};
    const auto guild_failure = project(bad_guild, &guild);
    check(guild_failure.ok() && guild.complete == 1 && guild.guild_creates == 1 && guild_failure.degraded_guilds == 1 &&
        guild_failure.degraded_route.event_error == GatewayEventDecodeError::missing_required_field &&
        (guild_failure.degraded_route.detail & 0xFFu) == 2,
        "malformed final guild is left unavailable with its categorical reason; READY and other guilds complete");
    const auto bad_reads = utf8_literal(R"({"v":9,"user":{"id":"1","username":"self"},"session_id":"projection","resume_gateway_url":"wss://gateway.discord.gg","guilds":[],"read_state":{"entries":[{"id":"9"},{"id":"9"}]}})");
    Probe reads{};
    const auto read_failure = project(bad_reads, &reads);
    check(!read_failure.ok() && !reads.admitted && read_failure.phase == GatewayReadyProjectionPhase::preflight,
        "duplicate trailing read-state authority rejects the entire READY before publication");

    Probe cancel{}; cancel.stop_preflight = 10;
    const auto cancelled = project(fixture(129), &cancel);
    check(cancelled.interrupted && !cancelled.completed &&
        cancelled.phase == GatewayReadyProjectionPhase::preflight && !cancel.admitted,
        "preflight cancellation or expired deadline stops between records without owner publication");
    Probe before_begin{}; before_begin.stop_publication = 1;
    const auto before = project(fixture(1), &before_begin);
    check(before.interrupted && !before.completed && !before.publication_started &&
        before.phase == GatewayReadyProjectionPhase::publication &&
        !before_begin.admitted && before.failure_stage == GatewayClientFailureStage::ready_publish,
        "guard can reject publication after successful preflight before first READY event");
    Probe admission{}; admission.fail_admission = 3;
    const auto interrupted = project(fixture(129), &admission);
    check(interrupted.interrupted && interrupted.publication_started && !interrupted.completed &&
        admission.admitted == 3 && !admission.complete &&
        interrupted.phase == GatewayReadyProjectionPhase::publication &&
        interrupted.failure_stage == GatewayClientFailureStage::ready_publish,
        "post-begin callback failure reports incomplete admitted prefix without a completion checkpoint");
    Probe during{}; during.stop_publication = 12;
    const auto partial = project(fixture(129), &during);
    check(partial.interrupted && !partial.completed && during.admitted && !during.complete,
        "publication cancellation retains its admitted prefix and cannot claim transaction rollback");

    GatewayReady decoded{};
    const auto small = fixture(1);
    check(decode(small, &decoded), "READY bounded-scratch fixture decodes");
    Probe capacity{};
    auto tiny = scratch(); tiny.payload.count = 1;
    const auto exhausted = gateway_ready_project(decoded, tiny, 88, 99, publish, work, &capacity);
    check(!exhausted.ok() && !capacity.admitted && exhausted.phase == GatewayReadyProjectionPhase::preflight,
        "current typed output exhaustion is discovered before destructive READY publication");
    auto alias = scratch(); alias.header = {reinterpret_cast<char*>(source), sizeof(header)};
    const u64 before_alias = hash(small);
    Probe aliased{};
    check(!gateway_ready_project(decoded, alias, 88, 99, publish, work, &aliased).ok() &&
        !aliased.admitted && hash(small) == before_alias,
        "source and mutable scratch overlap is rejected before changing immutable READY bytes");
    auto overlap = scratch(); overlap.records = {arrays, 16384};
    Probe overlapping{};
    check(!gateway_ready_project(decoded, overlap, 88, 99, publish, work, &overlapping).ok() && !overlapping.admitted,
        "outer READY and per-record indices require separate live scratch ownership");
    Probe incomplete_policy{};
    const auto policy_result=project(utf8_literal(R"({"v":9,"user":{"id":"1","username":"self"},"session_id":"projection","resume_gateway_url":"wss://gateway.discord.gg","guilds":[],"user_guild_settings":{"entries":[{"guild_id":"8"},{"guild_id":"8"}]},"notification_settings":{"flags":0},"user_settings_proto":""})"),&incomplete_policy);
    check(policy_result.ok()&&incomplete_policy.complete==1&&incomplete_policy.notification_complete==1&&incomplete_policy.notification_incomplete==1,
        "duplicate notification READY authority is typed incomplete without terminating otherwise valid Gateway");
    GatewayEnvelope envelope{};envelope.opcode=0;envelope.has_event_name=true;
    envelope.event_name={utf8_literal("USER_GUILD_SETTINGS_UPDATE"),0,false};
    const auto entry_text=utf8_literal(R"({"guild_id":"8","version":5,"channel_overrides":[{"channel_id":"20","message_notifications":1}]})");
    envelope.data={entry_text,0,JsonValueKind::object};GatewayClientEvent setting{};ConstSlice<u8>setting_payload{};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&setting,&setting_payload).ok()&&
        setting.dispatch_kind==GatewayClientDispatchKind::notification_settings_entry&&setting.notification.version==5,"live full notification entry projection");
    MessageNotificationGuildSetting guild_setting{};MessageNotificationOverride channel_setting{};
    check(gateway_client_notification_entry(setting,setting_payload,&guild_setting)&&gateway_client_notification_override_at(setting,setting_payload,0,&channel_setting)&&
        guild_setting.guild_id==8&&channel_setting.channel_id==20&&channel_setting.setting.level==MessageNotificationLevel::mentions,"copied settings record accessors");
    const auto message_text=utf8_literal(R"({"id":"99","channel_id":"20","type":19,"author":{"id":"2","username":"peer","public_flags":1048576,"bot":true},"content":"hi","timestamp":"2026-10-02T00:00:00Z","activity_instance":{},"application_id":"7"})");
    envelope.event_name={utf8_literal("MESSAGE_CREATE"),0,false};envelope.data={message_text,0,JsonValueKind::object};
    GatewayClientEvent message{};ConstSlice<u8>message_payload{};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&message,&message_payload).ok()&&
        message.message.type==19&&message.message.author_public_flags==1048576&&(message.message.fields&gateway_message_field_author_policy),"message policy metadata occupies existing header padding");
    GatewayMessageEvent decoded_message{};
    check(gateway_client_event_as_message(message,message_payload,&decoded_message)&&decoded_message.type==19&&decoded_message.author_public_flags==1048576&&
        decoded_message.author_bot&&decoded_message.activity_instance_present&&decoded_message.application_present,"message payload consumer retains policy metadata");
    check((message.message.fields&gateway_message_field_interaction_metadata)&&!(message.message.mention_flags&gateway_client_message_interaction_metadata_present),
        "ordinary full create has explicit known absent interaction metadata");
    u64 interaction_user=77;
    check(gateway_client_event_message_interaction_user(message,message_payload,&interaction_user)&&interaction_user==0,
        "activity instance without interaction returns a known absent interaction user");
    const auto self_activity=utf8_literal(R"({"id":"100","channel_id":"20","type":20,"author":{"id":"2","username":"peer"},"content":"hi","timestamp":"2026-10-02T00:00:00Z","activity_instance":{},"interaction":{"id":"900","type":2,"name":"play","user":{"id":"1","username":"self"}}})");
    envelope.data={self_activity,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&message,&message_payload).ok()&&
        gateway_client_event_message_interaction_user(message,message_payload,&interaction_user)&&interaction_user==1,
        "bounded scalar header identifies self activity even before off-channel rich storage admission");
    GatewayClientEvent malformed_message=message;malformed_message.message.rich_payload_bytes--;
    interaction_user=77;
    check(!gateway_client_event_message_interaction_user(malformed_message,message_payload,&interaction_user)&&interaction_user==77,
        "interaction scalar rejects inconsistent copied payload without publishing output");
    GatewayClientMessageRichHeader scalar_header{};scalar_header.interaction_id=900;scalar_header.interaction_user_id=1;scalar_header.interaction_type=2;
    u8 unaligned[sizeof(scalar_header)+1]{};copy_bytes(unaligned+1,&scalar_header,sizeof(scalar_header));
    GatewayClientEvent scalar_event{};scalar_event.kind=GatewayClientEventKind::dispatch;scalar_event.dispatch_kind=GatewayClientDispatchKind::message_create;
    scalar_event.dispatch_payload_bytes=sizeof(scalar_header);scalar_event.message.rich_payload_bytes=sizeof(scalar_header);scalar_event.message.fields=gateway_message_field_interaction;
    check(gateway_client_event_message_interaction_user(scalar_event,{unaligned+1,sizeof(scalar_header)},&interaction_user)&&interaction_user==1,
        "interaction scalar copies an unaligned mailbox header safely");
    scalar_header.reserved[0]=1;copy_bytes(unaligned+1,&scalar_header,sizeof(scalar_header));interaction_user=77;
    check(!gateway_client_event_message_interaction_user(scalar_event,{unaligned+1,sizeof(scalar_header)},&interaction_user)&&interaction_user==77,
        "interaction scalar rejects nonzero reserved header bytes");
    const auto metadata_message=utf8_literal(R"({"id":"101","channel_id":"20","type":20,"author":{"id":"2","username":"peer"},"content":"hi","timestamp":"2026-10-02T00:00:00Z","interaction_metadata":{"user":{"id":"3"}}})");
    envelope.data={metadata_message,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&message,&message_payload).ok()&&
        (message.message.fields&gateway_message_field_interaction_metadata)&&(message.message.mention_flags&gateway_client_message_interaction_metadata_present)&&
        gateway_client_event_as_message(message,message_payload,&decoded_message)&&decoded_message.interaction_metadata_present,
        "present unsupported modern interaction metadata stays typed distinct from legacy own activity");
    const auto metadata_patch=utf8_literal(R"({"id":"101","channel_id":"20","content":"patch"})");
    envelope.event_name={utf8_literal("MESSAGE_UPDATE"),0,false};envelope.data={metadata_patch,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&message,&message_payload).ok()&&
        !(message.message.fields&gateway_message_field_interaction_metadata),
        "omitted modern interaction metadata patch preserves missing field knowledge");
    const auto metadata_clear=utf8_literal(R"({"id":"101","channel_id":"20","interaction_metadata":null})");envelope.data={metadata_clear,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&message,&message_payload).ok()&&
        (message.message.fields&gateway_message_field_interaction_metadata)&&!(message.message.mention_flags&gateway_client_message_interaction_metadata_present),
        "explicit null modern interaction metadata patch clears presence without a fabricated user");
    const auto thread_text=utf8_literal(R"({"guild_id":"8","id":"30","added_members":[{"id":"30","user_id":"1","flags":4},{"id":"30","user_id":"3","flags":2}],"removed_member_ids":["4"]})");
    envelope.event_name={utf8_literal("THREAD_MEMBERS_UPDATE"),0,false};envelope.data={thread_text,0,JsonValueKind::object};
    GatewayClientEvent thread_event{};ConstSlice<u8>thread_payload{};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&thread_event,&thread_payload).ok()&&thread_event.notification_payload_bytes,
        "thread delta carries separate self-notification metadata");
    GatewayClientEvent ordinary{};ConstSlice<u8>ordinary_payload{};
    GatewayClientNotificationSidecar side{};ConstSlice<u8>side_rows{};GatewayClientNotificationThreadRecord self_thread{};
    check(gateway_client_notification_trim(thread_event,thread_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({thread_payload.data+ordinary_payload.count,thread_event.notification_payload_bytes},&side,&side_rows)&&
        side.thread_count==1&&gateway_client_notification_thread_at(side_rows,0,&self_thread)&&self_thread.thread.thread_id==30&&
        (self_thread.thread.setting.fields&notification_muted)&&!self_thread.thread.setting.muted,
        "self thread copied with proven full-member muted default and other users excluded");
    GatewayClientEvent legacy{};legacy.kind=GatewayClientEventKind::dispatch;ConstSlice<u8>legacy_payload{};bool handled=false;
    check(gateway_client_route_thread_member_dispatch(envelope,{records,16384},64,{header,sizeof(header)},&legacy,&legacy_payload,&handled).ok()&&handled&&
        ordinary_payload.count==legacy_payload.count&&bytes_equal(ordinary_payload.data,legacy_payload.data,legacy_payload.count)&&
        ordinary.thread_members.record_count==legacy.thread_members.record_count,
        "notification trimming preserves exact ordinary participant body and count");
    const auto guild_meta=utf8_literal(R"({"id":"8","name":"guild","owner_id":"99","roles":[],"default_message_notifications":1,"channels":[{"id":"40","type":18,"application_id":"7"}]})");
    envelope.event_name={utf8_literal("GUILD_UPDATE"),0,false};envelope.data={guild_meta,0,JsonValueKind::object};
    GatewayClientEvent guild_event{};ConstSlice<u8>guild_payload{};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.flags&gateway_notification_guild_default_mentions)&&gateway_client_notification_thread_at(side_rows,0,&self_thread)&&
        self_thread.flags==gateway_notification_channel_metadata&&self_thread.thread.thread_id==40&&(self_thread.gates&notification_gate_managed),
        "nested manageable guild channel metadata uses scoped row without self-thread admission");
    const auto age_guild=utf8_literal(R"({"id":"8","name":"age","owner_id":"99","roles":[],"channels":[{"id":"40","type":0},{"id":"41","type":0,"nsfw":true},{"id":"42","type":18,"application_id":"7"}]})");
    envelope.event_name={utf8_literal("READY_GUILD"),0,false};envelope.data={age_guild,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.flags&gateway_notification_channel_age_baseline_complete)&&(side.known_gates&notification_gate_age_access)&&
        !(side.gates&notification_gate_age_access)&&side.thread_count==2&&gateway_client_notification_thread_at(side_rows,0,&self_thread)&&
        self_thread.thread.thread_id==41&&self_thread.flags==gateway_notification_channel_metadata&&(self_thread.gates&notification_gate_age_access),
        "full guild validates clear age defaults and sparse sensitive or manageable rows only");
    const u32 ordinary_age_bytes=ordinary_payload.count;
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,ordinary_age_bytes+sizeof(GatewayClientNotificationSidecar)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.flags&gateway_notification_incomplete)&&!(side.flags&gateway_notification_channel_age_baseline_complete)&&side.thread_count==0&&side.known_gates==0,
        "sparse age row capacity failure publishes incomplete scope and no false known clear baseline");
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,ordinary_age_bytes},{},1,&guild_event,&guild_payload).ok()&&
        guild_event.notification_payload_bytes==kGatewayNotificationMetadataIncomplete&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        ordinary.notification_payload_bytes==0&&ordinary_payload.count==guild_payload.count,
        "header capacity failure marks copied policy incomplete while preserving valid ordinary body");
    const auto age_patch=utf8_literal(R"({"id":"8","name":"guild","owner_id":"99","roles":[],"nsfw_level":0})");
    envelope.event_name={utf8_literal("GUILD_UPDATE"),0,false};envelope.data={age_patch,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        !(side.flags&gateway_notification_channel_age_baseline_complete)&&!(side.known_gates&notification_gate_age_access),
        "sparse guild patch cannot invent complete channel age authority or clear other candidate");
    const auto sensitive_guild=utf8_literal(R"({"id":"8","name":"guild","owner_id":"99","roles":[],"owner_configured_content_level":3})");envelope.data={sensitive_guild,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.known_gates&notification_gate_age_access)&&(side.gates&notification_gate_age_access),
        "sensitive guild candidate suppresses without inferring consent or experiment choice");
    const auto malformed_age=utf8_literal(R"({"id":"8","name":"guild","owner_id":"99","roles":[],"channels":[{"id":"41","type":0,"nsfw":7}]})");envelope.data={malformed_age,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.flags&gateway_notification_incomplete)&&!(side.flags&gateway_notification_channel_age_baseline_complete)&&side.thread_count==0,
        "malformed channel age collection publishes scoped incomplete rather than false age baseline");
    const auto dm_clear=utf8_literal(R"({"id":"60","type":1,"recipients":[]})");
    envelope.event_name={utf8_literal("CHANNEL_CREATE"),0,false};envelope.data={dm_clear,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.known_gates&notification_gate_age_access)&&!(side.gates&notification_gate_age_access),
        "full private channel supplies known clear age default without inventing patch state");
    const auto age_channel_patch=utf8_literal(R"({"id":"41","guild_id":"8","type":0,"nsfw":true})");
    envelope.event_name={utf8_literal("CHANNEL_UPDATE"),0,false};envelope.data={age_channel_patch,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        (side.known_gates&notification_gate_age_access)&&(side.gates&notification_gate_age_access),
        "explicit channel sensitivity patch suppresses until native age consent authority exists");
    const auto restricted_update=utf8_literal(R"({"id":"1","restricted_schedule":{"rules":[{"enabled":true}]}})");
    envelope.event_name={utf8_literal("USER_UPDATE"),0,false};envelope.data={restricted_update,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        gateway_client_notification_trim(guild_event,guild_payload,&ordinary,&ordinary_payload)&&
        gateway_client_notification_sidecar({guild_payload.data+ordinary_payload.count,guild_event.notification_payload_bytes},&side,&side_rows)&&
        side.channel_id==0&&side.guild_id==0&&(side.flags&gateway_notification_incomplete)&&!(side.known_gates&notification_gate_restricted_hours),
        "unsupported self restricted schedule invalidates global knowledge rather than writing a user-ID channel scope");
    envelope.event_name={utf8_literal("USER_SETTINGS_PROTO_UPDATE"),0,false};
    const auto invalid_global=utf8_literal(R"({"partial":false,"settings":{"type":1,"proto":"AB=="}})");envelope.data={invalid_global,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        guild_event.dispatch_kind==GatewayClientDispatchKind::notification_global&&(guild_event.notification.flags&gateway_notification_incomplete)&&
        guild_event.dispatch_payload_bytes==0&&guild_payload.count==0,
        "invalid global proto invalidates only global authority without poisoning implicit guild defaults");
    const auto duplicate_global=utf8_literal(R"({"partial":true,"partial":false,"settings":{"type":1,"proto":""}})");envelope.data={duplicate_global,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        guild_event.dispatch_kind==GatewayClientDispatchKind::notification_global&&(guild_event.notification.flags&gateway_notification_incomplete)&&!guild_payload.count,
        "known global event with invalid JSON fields retains typed global incomplete classification");
    envelope.event_name={utf8_literal("NOTIFICATION_SETTINGS_UPDATE"),0,false};
    const auto invalid_account=utf8_literal(R"({"flags":"invalid"})");envelope.data={invalid_account,0,JsonValueKind::object};
    check(gateway_client_route_dispatch(envelope,{records,16384},64,{payload,sizeof(payload)},{},1,&guild_event,&guild_payload).ok()&&
        guild_event.dispatch_kind==GatewayClientDispatchKind::notification_global&&(guild_event.notification.flags&gateway_notification_incomplete)&&!guild_payload.count,
        "invalid account notification flags stay separate from guild or thread policy authority");
}
} // namespace maicord

// MaiCordPerfAccount.dll: a synthetic account module for MaiCordPerfProbe.
//
// Implements the account module ABI with no network access. A large
// deterministic READY is decoded and projected by the production routes
// (gateway_decode_ready + gateway_ready_project) on one worker; message history,
// member lists, member chunks and generated media frames are served from a
// second; typing/presence/message churn arrives on demand. Every REST
// submission gets a response so no client lane stays busy. All module memory
// comes from one reservation so the probe can report it separately.
#include "perf_account_internal.hpp"
#include "perf_shared.hpp"
#include "maicord/app/account/account_module_api.hpp"
#include "maicord/app/media/account_download.hpp"
#include "maicord/app/account/account_logout.hpp"
#include "maicord/app/media/account_upload.hpp"
#include "maicord/app/media/account_video_playback.hpp"
#include "maicord/app/account/login/captcha_bridge.hpp"
#include "maicord/app/chat/messages/event_scratch.hpp"
#include "maicord/app/voice/video_stream_decoders.hpp"
#include "maicord/protocol/messages/create.hpp"
#include "maicord/app/account/user_account_coordinator.hpp"
#include "../../src/app/gateway/ready_projection.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/account/account_module_version.hpp"
#include "maicord/protocol/messages/history.hpp"
#include "maicord/app/voice/socket.hpp"
#include "maicord/app/voice/udp_discovery.hpp"
#include "maicord/protocol/voice/participants.hpp"
#include "maicord/protocol/voice/video_signaling.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord::perf {
namespace {

// Event queue
struct QueuedEvent {
    GatewayClientEvent event;
    u64 offset;
    u32 bytes;
};
constexpr u32 kEventCapacity = 65'536;
constexpr u64 kEventPayloadBytes = 96ull * 1024 * 1024;

struct RestEntry {
    RestClientResponse response;
    u64 offset;
};
constexpr u32 kRestCapacity = 64;
constexpr u32 kRestBodyBytes = kAccountHistoryResponseBytes;

enum class WorkKind : u8 { none, history, rest_error, media, media_advance, member_sync, member_chunk, churn_tick };
struct Work {
    WorkKind kind;
    u64 a, b, c, d;
    u32 e, f;
    MediaAssetKey key;
    u64 request_id;
};
constexpr u32 kWorkCapacity = 8'192;

struct MediaSlot {
    AccountMediaFrame frame;
    u64 offset;
};
constexpr u32 kMediaSlots = 48;
constexpr u32 kMediaSlotBytes = 512 * 1024;

struct Scratch {
    char* json;
    u32* structurals;
    char* route;
};
constexpr u32 kScratchJsonBytes = 1024 * 1024;
constexpr u32 kScratchStructurals = 256 * 1024;
// The synthetic READY is fixed in size and well under 1M structurals. A local
// bound keeps the harness's committed memory, which counts in the measured
// process, independent of the Gateway's 8M-offset limit.
constexpr u32 kReadyStructurals = 1024 * 1024;

// The joined synthetic call. The publisher thread only counts pictures;
// textures and the device are created and released on the UI thread.
struct SyntheticCall {
    ID3D11Device* device;
    ID3D11Texture2D* texture; // Two NV12 slices, alternated per picture.
    ID3D11ShaderResourceView* luma[2];
    ID3D11ShaderResourceView* chroma[2];
    HANDLE thread, stop, frame_event; // frame_event: auto reset, set per picture.
    u64 guild_id, channel_id;
    volatile LONG fps, pictures;
    bool active, started;
};

struct Module {
    SRWLOCK lock;
    HANDLE gateway_event;   // manual reset: events pending
    HANDLE response_event;  // manual reset: REST or media results pending
    HANDLE work_event;      // auto reset: worker has work
    HANDLE stop_event;      // manual reset
    HANDLE ready_thread;
    HANDLE work_thread;
    HANDLE churn_thread;
    HANDLE churn_stop;
    QueuedEvent* events;
    u8* event_payload;
    u32 event_count, event_cursor;
    u64 event_used;
    RestEntry rest[kRestCapacity];
    u8* rest_bodies;
    u32 rest_head, rest_count;
    Work* work;
    u32 work_head, work_count;
    MediaSlot media[kMediaSlots];
    u8* media_pixels;
    u32 media_head, media_count;
    bool media_has_taken;
    // READY worker scratch.
    char* ready_json;
    u32* ready_structurals;
    u32* records;
    char* projection_payload;
    char* header;
    // Service and churn worker scratch (one set per thread).
    Scratch service;
    Scratch churn_scratch;
    u64 member_request_ids[100];
    volatile LONG ready_published;
    u64 sequence;
    PerfAccountControl churn;
    u64 churn_counter;
    PerfAccountCounters counters;
    SyntheticCall call;
    bool started;
};
Module* g_module = nullptr;

bool enqueue_event_locked(Module& m, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (m.event_count == kEventCapacity || payload.count > kEventPayloadBytes - m.event_used)
        return false;
    QueuedEvent& slot = m.events[m.event_count++];
    slot.event = event;
    slot.offset = m.event_used;
    slot.bytes = static_cast<u32>(payload.count);
    copy_bytes(m.event_payload + m.event_used, payload.data, static_cast<u32>(payload.count));
    m.event_used += payload.count;
    ++m.counters.gateway_events;
    m.counters.gateway_payload_bytes += payload.count;
    return true;
}

bool publish(void* context, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    Module& m = *static_cast<Module*>(context);
    AcquireSRWLockExclusive(&m.lock);
    // Producer backpressure, as the real mailbox: wait for the client to drain.
    while (!enqueue_event_locked(m, event, payload)) {
        if (m.event_cursor == 0 && m.event_count == kEventCapacity) {
            ReleaseSRWLockExclusive(&m.lock);
            return false;
        }
        ReleaseSRWLockExclusive(&m.lock);
        SetEvent(m.gateway_event);
        if (WaitForSingleObject(m.stop_event, 2) == WAIT_OBJECT_0)
            return false;
        AcquireSRWLockExclusive(&m.lock);
    }
    ReleaseSRWLockExclusive(&m.lock);
    SetEvent(m.gateway_event);
    return true;
}

bool route_and_publish(Module& m, Scratch& scratch, const char* name, Utf8View data) noexcept {
    GatewayEnvelope envelope{};
    envelope.opcode = 0;
    envelope.has_event_name = true;
    u32 name_bytes = 0;
    while (name[name_bytes])
        ++name_bytes;
    envelope.event_name = {{name, name_bytes}, 0, false};
    envelope.data = {data, 0, JsonValueKind::object};
    envelope.sequence = ++m.sequence;
    envelope.has_sequence = true;
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    u64 bulk[kGatewayBulkDeleteMaximumIds]{};
    static GatewayMessageDecodeScratch message_scratch;
    const auto routed = gateway_client_route_dispatch(
        envelope, {scratch.structurals, kScratchStructurals}, 64,
        {scratch.route, kGatewayClientDispatchPayloadCapacity}, {bulk, kGatewayBulkDeleteMaximumIds},
        kPerfSelfUserId, &event, &payload, &message_scratch);
    if (!routed.ok())
        return false;
    event.timestamp_ms = GetTickCount64();
    return publish(&m, event, payload);
}

// READY worker
DWORD WINAPI ready_thread(void* context) noexcept {
    Module& m = *static_cast<Module*>(context);
    const u32 bytes = build_ready(m.ready_json, 16 * 1024 * 1024);
    m.counters.ready_json_bytes = bytes;
    if (!bytes)
        return 1;
    GatewayReady ready{};
    if (!gateway_decode_ready({{m.ready_json, bytes}, 0, JsonValueKind::object},
                              {m.ready_structurals, kReadyStructurals}, 64, &ready)
             .ok())
        return 2;
    const u64 before = m.counters.gateway_events;
    const auto projected = gateway_ready_project(
        ready,
        {{m.ready_structurals, kReadyStructurals},
         {m.records, 512 * 1024},
         {m.projection_payload, kGatewayClientDispatchPayloadCapacity},
         {m.header, 4096}},
        ++m.sequence, GetTickCount64(), publish, nullptr, &m);
    if (!projected.ok())
        return 3;
    GatewayClientEvent complete{};
    complete.kind = GatewayClientEventKind::dispatch;
    complete.dispatch_kind = GatewayClientDispatchKind::supplemental_state_complete;
    complete.timestamp_ms = GetTickCount64();
    complete.sequence = ++m.sequence;
    complete.has_sequence = true;
    publish(&m, complete, {});
    m.counters.ready_events = static_cast<u32>(m.counters.gateway_events - before);
    InterlockedExchange(&m.ready_published, 1);
    return 0;
}

// Service worker
bool push_work(Module& m, const Work& work) noexcept {
    AcquireSRWLockExclusive(&m.lock);
    const bool ok = m.work_count < kWorkCapacity;
    if (ok) {
        m.work[(m.work_head + m.work_count) % kWorkCapacity] = work;
        ++m.work_count;
    }
    ReleaseSRWLockExclusive(&m.lock);
    if (ok)
        SetEvent(m.work_event);
    return ok;
}

bool push_rest(Module& m, u64 request_id, u16 status, ConstSlice<u8> body) noexcept {
    AcquireSRWLockExclusive(&m.lock);
    bool ok = m.rest_count < kRestCapacity && body.count <= kRestBodyBytes;
    if (ok) {
        const u32 slot = (m.rest_head + m.rest_count) % kRestCapacity;
        RestEntry& entry = m.rest[slot];
        entry = {};
        entry.offset = static_cast<u64>(slot) * kRestBodyBytes;
        copy_bytes(m.rest_bodies + entry.offset, body.data, static_cast<u32>(body.count));
        entry.response.request_id = request_id;
        entry.response.status_code = status;
        entry.response.body_bytes = static_cast<u32>(body.count);
        entry.response.content_type = RestContentType::json;
        entry.response.content_encoding = RestContentEncoding::identity;
        entry.response.body_kind = body.count == 0 ? RestBodyKind::empty
                                   : body.data[0] == '[' ? RestBodyKind::json_array
                                                          : RestBodyKind::json_object;
        entry.response.reused_connection = true;
        entry.response.elapsed_us = 1'000;
        ++m.rest_count;
        ++m.counters.rest_responses;
    }
    ReleaseSRWLockExclusive(&m.lock);
    if (ok)
        SetEvent(m.response_event);
    return ok;
}

void serve_history(Module& m, const Work& work) noexcept {
    const u64 channel = work.a;
    const u64 cursor = work.b;
    const u32 limit = minimum<u32>(work.e ? work.e : 50, 100);
    const auto position = static_cast<MessageHistoryPosition>(work.f);
    u32 newest = kMessagesPerChannel - 1;
    u32 oldest = 0;
    if (position == MessageHistoryPosition::before) {
        const u32 at = message_index_at(cursor);
        newest = at == 0 ? 0 : at - (message_id(channel, at) >= cursor ? 1 : 0);
        if (at == 0 && message_id(channel, 0) >= cursor) {
            push_rest(m, work.request_id, 200, {reinterpret_cast<const u8*>("[]"), 2});
            return;
        }
    } else if (position == MessageHistoryPosition::after) {
        const u32 at = message_index_at(cursor);
        oldest = at + 1;
        if (oldest >= kMessagesPerChannel) {
            push_rest(m, work.request_id, 200, {reinterpret_cast<const u8*>("[]"), 2});
            return;
        }
        newest = minimum<u32>(kMessagesPerChannel - 1, oldest + limit - 1);
    } else if (position == MessageHistoryPosition::around) {
        const u32 at = message_index_at(cursor);
        newest = minimum<u32>(kMessagesPerChannel - 1, at + limit / 2);
    }
    const u32 count = minimum<u32>(limit, newest - oldest + 1);
    u64 guild = 0;
    if (channel >= perf_guild_id(0) && channel < perf_guild_id(kPerfGuilds))
        guild = perf_guild_id(static_cast<u32>((channel - perf_guild_id(0)) / 10'000ull));
    Json j{m.service.json, kRestBodyBytes};
    j.s("[");
    u32 written = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 mark = j.used();
        j.c(i);
        append_message(j, guild, channel, newest - i, false);
        if (!j.ok) {
            j.ok = true;
            j.writer.used = mark;
            break;
        }
        ++written;
    }
    j.s("]");
    m.counters.history_messages += written;
    push_rest(m, work.request_id, 200, {reinterpret_cast<const u8*>(m.service.json), j.used()});
}

void serve_rest_error(Module& m, const Work& work) noexcept {
    constexpr char body[] = "{\"message\":\"Unknown Resource (perf probe)\",\"code\":10000}";
    push_rest(m, work.request_id, 404, {reinterpret_cast<const u8*>(body), sizeof(body) - 1});
}

u32 media_extent(MediaAssetKey key, u32 requested_width, u32 requested_height, u32* height) noexcept {
    u32 width = 128;
    *height = 128;
    switch (key.kind) {
    case MediaAssetKind::custom_emoji:
        width = *height = 96;
        break;
    case MediaAssetKind::sticker:
        width = *height = 160;
        break;
    case MediaAssetKind::attachment:
    case MediaAssetKind::embed:
        width = requested_width ? requested_width : 320;
        *height = requested_height ? requested_height : 240;
        break;
    default:
        break;
    }
    while (width * *height > kMediaSlotBytes / 4) {
        width = (width + 1) / 2;
        *height = (*height + 1) / 2;
    }
    return width;
}

bool media_animated(MediaAssetKey key) noexcept {
    return (key.flags & media_asset_flag_animated) != 0;
}

void serve_media(Module& m, const Work& work) noexcept {
    u32 height = 0;
    const u32 width = media_extent(work.key, work.e, work.f, &height);
    const u32 frame_index = work.kind == WorkKind::media_advance ? static_cast<u32>(work.c) : 0;
    const bool animated = media_animated(work.key);
    for (;;) {
        AcquireSRWLockExclusive(&m.lock);
        const bool room = m.media_count < kMediaSlots - 1;
        ReleaseSRWLockExclusive(&m.lock);
        if (room)
            break;
        if (WaitForSingleObject(m.stop_event, 1) == WAIT_OBJECT_0)
            return;
    }
    AcquireSRWLockExclusive(&m.lock);
    const u32 slot = (m.media_head + m.media_count) % kMediaSlots;
    ReleaseSRWLockExclusive(&m.lock);
    MediaSlot& media = m.media[slot];
    media = {};
    media.offset = static_cast<u64>(slot) * kMediaSlotBytes;
    u8* pixels = m.media_pixels + media.offset;
    // Deterministic gradient keyed by the asset; animations move a band.
    const u32 seed = static_cast<u32>(work.key.id * 2654435761u) ^ work.key.variant;
    for (u32 y = 0; y < height; ++y) {
        u8* row = pixels + static_cast<u64>(y) * width * 4;
        for (u32 x = 0; x < width; ++x) {
            const u32 band = animated && ((x + frame_index * 8) / 16) % 4 == 0 ? 80u : 0u;
            row[x * 4 + 0] = static_cast<u8>((seed >> 0) + x * 255 / width + band);
            row[x * 4 + 1] = static_cast<u8>((seed >> 8) + y * 255 / height);
            row[x * 4 + 2] = static_cast<u8>((seed >> 16) + band);
            row[x * 4 + 3] = 255;
        }
    }
    AccountMediaFrame& frame = media.frame;
    frame.key = work.key;
    frame.content_identity = work.key.id ^ (static_cast<u64>(work.key.variant) << 32);
    frame.bytes = {pixels, width * height * 4};
    frame.width = width;
    frame.height = height;
    frame.row_pitch = width * 4;
    frame.request_id = work.request_id;
    frame.source_message_id = work.a;
    frame.source_message_revision = static_cast<u32>(work.b);
    frame.cache_generation = 1;
    frame.source_kind = work.key.kind == MediaAssetKind::attachment || work.key.kind == MediaAssetKind::embed
                            ? AccountMediaSourceKind::message
                            : AccountMediaSourceKind::none;
    if (animated) {
        frame.kind = AccountMediaFrameKind::animated_webp_rgba8;
        frame.frame_index = frame_index % 12;
        frame.frame_count = 12;
        frame.play_count = 0;
        frame.duration_us = 80'000;
        frame.canvas_width = static_cast<u16>(width);
        frame.canvas_height = static_cast<u16>(height);
        frame.background_rgba = 0;
        frame.animated_webp_frame = {};
        frame.animated_webp_frame.payload_bytes = width * height * 4;
        frame.animated_webp_frame.duration_ms = 80;
        frame.animated_webp_frame.width = static_cast<u16>(width);
        frame.animated_webp_frame.height = static_cast<u16>(height);
        frame.animated_webp_frame.blend = AnimatedWebpBlend::source;
        frame.animated_webp_frame.dispose = AnimatedWebpDispose::none;
        frame.animated_webp_frame.has_alpha = false;
        frame.animated_webp_frame.codec = AnimatedWebpCodec::vp8l_lossless;
    } else {
        frame.kind = AccountMediaFrameKind::straight_rgba8;
        frame.frame_count = 1;
    }
    AcquireSRWLockExclusive(&m.lock);
    ++m.media_count;
    ++m.counters.media_frames;
    m.counters.media_pixel_bytes += frame.bytes.count;
    ReleaseSRWLockExclusive(&m.lock);
    SetEvent(m.response_event);
}

// Member list rows for Perf Lab: three hoisted roles, online, offline.
struct ListGroup {
    u64 id;
    u32 count;
    const char* name;
};
void list_groups(ListGroup* groups) noexcept {
    groups[0] = {perf_role_id(0, 1), 20, nullptr};
    groups[1] = {perf_role_id(0, 2), 80, nullptr};
    groups[2] = {perf_role_id(0, 3), 300, nullptr};
    groups[3] = {0, kPerfLabOnline - 400, "online"};
    groups[4] = {0, kPerfLabMembers - kPerfLabOnline, "offline"};
}
void append_group_id(Json& j, const ListGroup& group) noexcept {
    if (group.name)
        j.s("\"").s(group.name).s("\"");
    else
        j.id(group.id);
}

void serve_member_sync(Module& m, const Work& work) noexcept {
    const u64 guild = work.a;
    if (guild != perf_guild_id(0))
        return;
    ListGroup groups[5];
    list_groups(groups);
    const u32 first = work.e;
    const u32 last = minimum<u32>(work.f, first + 99);
    Json j{m.service.json, kScratchJsonBytes};
    j.s("{\"guild_id\":").id(guild).s(",\"id\":\"everyone\",\"member_count\":").n(kPerfLabMembers);
    j.s(",\"online_count\":").n(kPerfLabOnline).s(",\"groups\":[");
    for (u32 g = 0; g < 5; ++g) {
        j.c(g).s("{\"id\":");
        append_group_id(j, groups[g]);
        j.s(",\"count\":").n(groups[g].count).s("}");
    }
    j.s("],\"ops\":[{\"op\":\"SYNC\",\"range\":[").n(first).s(",").n(last).s("],\"items\":[");
    // Walk the virtual list: each group is a header row followed by members.
    u32 position = 0, member_ordinal = 0, written = 0;
    static const char* status[] = {"online", "idle", "dnd"};
    for (u32 g = 0; g < 5 && position <= last; ++g) {
        if (position >= first) {
            j.c(written++).s("{\"group\":{\"id\":");
            append_group_id(j, groups[g]);
            j.s(",\"count\":").n(groups[g].count).s("}}");
        }
        ++position;
        for (u32 i = 0; i < groups[g].count && position <= last; ++i, ++position, ++member_ordinal) {
            if (position < first)
                continue;
            const u32 user = 10'000 + member_ordinal;
            j.c(written++).s("{\"member\":{\"user\":");
            append_user(j, user);
            j.s(",\"roles\":[");
            if (g < 3)
                j.id(groups[g].id);
            j.s("],\"joined_at\":\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false,"
                "\"presence\":{\"user\":{\"id\":")
                .id(perf_user_id(user)).s("},\"status\":\"").s(g == 4 ? "offline" : status[member_ordinal % 3])
                .s("\",\"client_status\":{\"desktop\":\"online\"},");
            append_activity(j, user);
            j.s("}}}");
        }
    }
    j.s("]}]}");
    if (j.ok && route_and_publish(m, m.service, "GUILD_MEMBER_LIST_UPDATE", {m.service.json, j.used()}))
        ++m.counters.member_list_syncs;
}

void serve_member_chunk(Module& m, const Work& work) noexcept {
    // work.a guild, work.b nonce, work.e user count; the IDs are in member_request_ids.
    Json j{m.service.json, kScratchJsonBytes};
    j.s("{\"guild_id\":").id(work.a).s(",\"members\":[");
    const u32 count = work.e;
    const u64* users = m.member_request_ids;
    for (u32 i = 0; i < count && i < 100; ++i) {
        const u64 user = users[i] - perf_user_id(0);
        j.c(i).s("{\"user\":");
        append_user(j, static_cast<u32>(user % 100'000));
        j.s(",\"roles\":[").id(perf_role_id(static_cast<u32>((work.a - perf_guild_id(0)) / 10'000ull),
                                              static_cast<u32>(1 + user % 5)))
            .s("],\"joined_at\":\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false}");
    }
    j.s("],\"chunk_index\":0,\"chunk_count\":1,\"not_found\":[],\"nonce\":\"").n(work.b).s("\"}");
    if (j.ok)
        route_and_publish(m, m.service, "GUILD_MEMBERS_CHUNK", {m.service.json, j.used()});
}

void churn_message(Module& m, Scratch& scratch, u64 channel, u64 guild, u64 tick) noexcept;

DWORD WINAPI work_thread(void* context) noexcept {
    Module& m = *static_cast<Module*>(context);
    HANDLE handles[2] = {m.stop_event, m.work_event};
    for (;;) {
        Work work{};
        AcquireSRWLockExclusive(&m.lock);
        if (m.work_count) {
            work = m.work[m.work_head];
            m.work_head = (m.work_head + 1) % kWorkCapacity;
            --m.work_count;
        }
        ReleaseSRWLockExclusive(&m.lock);
        if (work.kind == WorkKind::none) {
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
                return 0;
            continue;
        }
        if (WaitForSingleObject(m.stop_event, 0) == WAIT_OBJECT_0)
            return 0;
        switch (work.kind) {
        case WorkKind::history:
            serve_history(m, work);
            break;
        case WorkKind::rest_error:
            serve_rest_error(m, work);
            break;
        case WorkKind::media:
        case WorkKind::media_advance:
            serve_media(m, work);
            break;
        case WorkKind::member_sync:
            serve_member_sync(m, work);
            break;
        case WorkKind::member_chunk:
            serve_member_chunk(m, work);
            break;
        case WorkKind::churn_tick:
            churn_message(m, m.service, work.a, work.b, work.c);
            break;
        default:
            break;
        }
    }
}

// Churn worker
void churn_typing(Module& m, Scratch& scratch, u64 tick) noexcept {
    char text[2048];
    Json j{text, sizeof(text)};
    const u32 user = static_cast<u32>(tick % 48);
    j.s("{\"channel_id\":").id(m.churn.channel_id).s(",\"guild_id\":").id(m.churn.guild_id).s(",\"user_id\":")
        .id(perf_user_id(user)).s(",\"timestamp\":1792994400,\"member\":{\"user\":");
    append_user(j, user);
    j.s(",\"roles\":[],\"joined_at\":\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false}}");
    if (j.ok)
        route_and_publish(m, scratch, "TYPING_START", {text, j.used()});
}
void churn_presence(Module& m, Scratch& scratch, u64 tick) noexcept {
    char text[2048];
    Json j{text, sizeof(text)};
    const u32 user = 10'000 + static_cast<u32>((tick * 7) % 400);
    static const char* status[] = {"online", "idle", "dnd", "online"};
    j.s("{\"user\":{\"id\":").id(perf_user_id(user)).s("},\"guild_id\":").id(m.churn.guild_id)
        .s(",\"status\":\"").s(status[tick % 4]).s("\",\"client_status\":{\"desktop\":\"online\"},");
    append_activity(j, static_cast<u32>(user + tick));
    j.s("}");
    if (j.ok)
        route_and_publish(m, scratch, "PRESENCE_UPDATE", {text, j.used()});
}
void churn_message(Module& m, Scratch& scratch, u64 channel, u64 guild, u64 tick) noexcept {
    // A new message after the newest history message (fresh ID per tick).
    char* text = scratch.json;
    Json j{text, kScratchJsonBytes};
    const u32 index = static_cast<u32>(tick % 12);
    // Reuse the history generator, then replace the ID with a strictly newer snowflake.
    append_message(j, guild, channel, kMessagesPerChannel - 12 + index, true);
    if (!j.ok)
        return;
    const u64 fresh =
        ((kLatestMessageMs + 1'000 + tick * 500 - kSnowflakeEpochMs) << 22) | (tick & 0x3FFFFF);
    char digits[24];
    u32 count = 0;
    for (u64 v = fresh; v; v /= 10)
        digits[count++] = static_cast<char>('0' + v % 10);
    // The message starts with {"id":"<20 digits>"; history IDs and fresh IDs have the same width.
    for (u32 i = 0; i < count && 7 + i < j.used(); ++i)
        text[7 + i] = digits[count - 1 - i];
    // Append guild_id and member: rewrite the closing brace.
    j.writer.used -= 1;
    j.s(",\"guild_id\":").id(guild).s(",\"member\":{\"roles\":[],\"joined_at\":"
        "\"2025-02-01T00:00:00+00:00\",\"nick\":null,\"mute\":false,\"deaf\":false}}");
    if (j.ok)
        route_and_publish(m, scratch, "MESSAGE_CREATE", {text, j.used()});
}

DWORD WINAPI churn_thread(void* context) noexcept {
    Module& m = *static_cast<Module*>(context);
    constexpr u32 kTickMs = 50 / kPerfScenarioSpeed;
    u64 typing_budget = 0, presence_budget = 0, message_budget = 0;
    u64 previous = GetTickCount64();
    while (WaitForSingleObject(m.churn_stop, kTickMs) == WAIT_TIMEOUT) {
        // Windows can round the wait up to its timer resolution. Accrue by
        // elapsed time so a short run still exercises the requested event rate.
        const u64 now = GetTickCount64(), elapsed = now - previous;
        previous = now;
        typing_budget += m.churn.typing_per_second * elapsed;
        presence_budget += m.churn.presence_per_second * elapsed;
        message_budget += m.churn.messages_per_second * elapsed;
        for (; typing_budget >= 1000; typing_budget -= 1000)
            churn_typing(m, m.churn_scratch, m.churn_counter++);
        for (; presence_budget >= 1000; presence_budget -= 1000)
            churn_presence(m, m.churn_scratch, m.churn_counter++);
        for (; message_budget >= 1000; message_budget -= 1000)
            churn_message(m, m.churn_scratch, m.churn.channel_id, m.churn.guild_id, m.churn_counter++);
    }
    return 0;
}

// Module API
AccountModuleStatus status_of(Module& m) noexcept {
    AccountModuleStatus status{};
    status.profile_id = 0x5045'5246'0000'0001ull;
    status.gateway_ready = 1;
    status.running = true;
    status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::running);
    (void)m;
    return status;
}

AccountModuleStartResult WINAPI module_start(const wchar_t*, const wchar_t*, u64, u64) noexcept {
    AccountModuleStartResult result{};
    if (g_module) {
        result.error = AccountModuleError::coordinator_start;
        return result;
    }
    auto* m = static_cast<Module*>(region_take(sizeof(Module)));
    if (!m) {
        result.error = AccountModuleError::memory;
        return result;
    }
    InitializeSRWLock(&m->lock);
    m->events = static_cast<QueuedEvent*>(region_take(sizeof(QueuedEvent) * u64{kEventCapacity}));
    m->event_payload = static_cast<u8*>(region_take(kEventPayloadBytes));
    m->rest_bodies = static_cast<u8*>(region_take(u64{kRestBodyBytes} * kRestCapacity));
    m->work = static_cast<Work*>(region_take(sizeof(Work) * u64{kWorkCapacity}));
    m->media_pixels = static_cast<u8*>(region_take(u64{kMediaSlotBytes} * kMediaSlots));
    m->ready_json = static_cast<char*>(region_take(16ull * 1024 * 1024));
    m->ready_structurals = static_cast<u32*>(region_take(sizeof(u32) * u64{kReadyStructurals}));
    m->records = static_cast<u32*>(region_take(sizeof(u32) * 512ull * 1024));
    m->projection_payload = static_cast<char*>(region_take(kGatewayClientDispatchPayloadCapacity));
    m->header = static_cast<char*>(region_take(8192));
    Scratch* scratches[2] = {&m->service, &m->churn_scratch};
    for (Scratch* scratch : scratches) {
        scratch->json = static_cast<char*>(region_take(kScratchJsonBytes));
        scratch->structurals = static_cast<u32*>(region_take(sizeof(u32) * u64{kScratchStructurals}));
        scratch->route = static_cast<char*>(region_take(kGatewayClientDispatchPayloadCapacity));
    }
    m->gateway_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->response_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->churn_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m->events || !m->event_payload || !m->rest_bodies || !m->work || !m->media_pixels || !m->ready_json ||
        !m->ready_structurals || !m->records || !m->projection_payload || !m->header || !m->service.route ||
        !m->churn_scratch.route ||
        !m->gateway_event || !m->response_event || !m->work_event || !m->stop_event || !m->churn_stop) {
        result.error = AccountModuleError::memory;
        return result;
    }
    // The READY worker runs while the client creates its window and device,
    // like the real module's Gateway handshake.
    m->ready_thread = CreateThread(nullptr, 256 * 1024, ready_thread, m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    m->work_thread = CreateThread(nullptr, 256 * 1024, work_thread, m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!m->ready_thread || !m->work_thread) {
        result.error = AccountModuleError::coordinator_start;
        return result;
    }
    m->started = true;
    g_module = m;
    result.instance = m;
    result.status = status_of(*m);
    result.session_loaded = true;
    return result;
}

AccountModuleStatus WINAPI module_pump(void* instance, u64, u64) noexcept {
    return status_of(*static_cast<Module*>(instance));
}

u32 WINAPI module_notification_handles(void* instance, HANDLE* output, u32 capacity) noexcept {
    auto* m = static_cast<Module*>(instance);
    u32 count = 0;
    if (m && capacity >= 2) {
        output[count++] = m->gateway_event;
        output[count++] = m->response_event;
    }
    return count;
}

void* WINAPI module_gateway_source(void* instance) noexcept {
    return instance;
}

GatewayClientPopResult WINAPI module_gateway_pop(void* source, GatewayClientEvent* event,
                                                 Slice<u8> output) noexcept {
    auto* m = static_cast<Module*>(source);
    if (!m || !event)
        return {0, GatewayClientPopError::invalid_input};
    AcquireSRWLockExclusive(&m->lock);
    if (m->event_cursor == m->event_count) {
        m->event_cursor = m->event_count = 0;
        m->event_used = 0;
        ResetEvent(m->gateway_event);
        ReleaseSRWLockExclusive(&m->lock);
        return {0, GatewayClientPopError::empty};
    }
    const QueuedEvent& queued = m->events[m->event_cursor];
    if (queued.bytes > output.count) {
        ReleaseSRWLockExclusive(&m->lock);
        return {queued.bytes, GatewayClientPopError::output_too_small};
    }
    copy_bytes(output.data, m->event_payload + queued.offset, queued.bytes);
    *event = queued.event;
    ++m->event_cursor;
    const u32 bytes = queued.bytes;
    ReleaseSRWLockExclusive(&m->lock);
    return {bytes, GatewayClientPopError::none};
}

bool WINAPI module_guild_subscribe(void* instance, const GatewayGuildSubscription* subscription) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !subscription)
        return false;
    for (u32 r = 0; r < subscription->range_count; ++r) {
        Work work{};
        work.kind = WorkKind::member_sync;
        work.a = subscription->guild_id;
        work.b = subscription->channel_id;
        work.e = subscription->ranges[r].first;
        work.f = subscription->ranges[r].last;
        push_work(*m, work);
    }
    return true;
}

bool WINAPI module_guild_members_request(void* instance, const GatewayGuildMemberRequest* request) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m)
        return false;
    if (!request || !request->user_count)
        return true;
    // The worker reads the user IDs from member_request_ids; one chunk in flight.
    AcquireSRWLockExclusive(&m->lock);
    copy_bytes(m->member_request_ids, request->user_ids, minimum<u32>(request->user_count, 100) * 8);
    ReleaseSRWLockExclusive(&m->lock);
    Work work{};
    work.kind = WorkKind::member_chunk;
    work.a = request->guild_id;
    work.b = request->nonce;
    work.e = minimum<u32>(request->user_count, 100);
    return push_work(*m, work);
}

GatewayStateRecoveryResult WINAPI module_gateway_refresh(void*) noexcept {
    return {};
}

RestClientSubmitResult submitted() noexcept {
    return {};
}
RestClientSubmitResult rejected() noexcept {
    RestClientSubmitResult result{};
    result.error = RestClientSubmitError::invalid_input;
    return result;
}

RestClientSubmitResult WINAPI module_history_submit(void* instance, u64 channel_id,
                                                    const MessageHistoryQuery* query,
                                                    u64 request_id) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !query || !request_id)
        return rejected();
    Work work{};
    work.kind = WorkKind::history;
    work.a = channel_id;
    work.b = query->cursor;
    work.e = query->limit;
    work.f = static_cast<u32>(query->position);
    work.request_id = request_id;
    return push_work(*m, work) ? submitted() : rejected();
}

RestClientSubmitResult WINAPI module_self_status_submit(void* instance, u64, u64, SelfStatusSettingsOperation,
                                                        GatewayUserStatus, u64 request_id, u64) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !request_id)
        return rejected();
    Work work{};
    work.kind = WorkKind::rest_error;
    work.request_id = request_id;
    return push_work(*m, work) ? submitted() : rejected();
}

RestClientTakeResult WINAPI module_rest_take(void* instance, RestClientResponse* response,
                                             Slice<u8> body) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !response)
        return {0, RestClientTakeError::invalid_input};
    AcquireSRWLockExclusive(&m->lock);
    if (!m->rest_count) {
        if (!m->media_count)
            ResetEvent(m->response_event);
        ReleaseSRWLockExclusive(&m->lock);
        return {0, RestClientTakeError::empty};
    }
    RestEntry& entry = m->rest[m->rest_head];
    if (entry.response.body_bytes > body.count) {
        ReleaseSRWLockExclusive(&m->lock);
        return {entry.response.body_bytes, RestClientTakeError::output_too_small};
    }
    copy_bytes(body.data, m->rest_bodies + entry.offset, entry.response.body_bytes);
    *response = entry.response;
    const u32 bytes = entry.response.body_bytes;
    m->rest_head = (m->rest_head + 1) % kRestCapacity;
    --m->rest_count;
    if (!m->rest_count && !m->media_count)
        ResetEvent(m->response_event);
    ReleaseSRWLockExclusive(&m->lock);
    return {bytes, RestClientTakeError::none};
}

AccountMediaRequestResult queued_media(Module& m, MediaAssetKey key, u64 source_message, u32 revision,
                                       u32 width, u32 height, WorkKind kind, u64 frame_index) noexcept {
    AccountMediaRequestResult result{};
    Work work{};
    work.kind = kind;
    work.key = key;
    work.a = source_message;
    work.b = revision;
    work.c = frame_index;
    work.e = width;
    work.f = height;
    result.state = push_work(m, work) ? AccountMediaRequestState::queued : AccountMediaRequestState::busy;
    return result;
}

AccountMediaRequestResult WINAPI module_media_request(void* instance, MediaAssetKey key, u64, u64) noexcept {
    return queued_media(*static_cast<Module*>(instance), key, 0, 0, 0, 0, WorkKind::media, 0);
}
AccountMediaRequestResult WINAPI module_media_guild_icon_request(void* instance,
                                                                 const AccountMediaGuildIconRequest* request,
                                                                 u64, u64) noexcept {
    if (!request)
        return {};
    return queued_media(*static_cast<Module*>(instance), request->key, 0, 0, 0, 0, WorkKind::media, 0);
}
AccountMediaRequestResult WINAPI module_media_attachment_request(void* instance,
                                                                 const AccountMediaAttachmentRequest* request,
                                                                 u64, u64) noexcept {
    if (!request)
        return {};
    return queued_media(*static_cast<Module*>(instance), request->key, request->source_message_id,
                        request->source_message_revision, request->proxy_width, request->proxy_height,
                        WorkKind::media, 0);
}
AccountMediaRequestResult WINAPI module_media_advance(void* instance, MediaAssetKey key, u16, u32 frame_index,
                                                      u64) noexcept {
    return queued_media(*static_cast<Module*>(instance), key, 0, 0, 0, 0, WorkKind::media_advance, frame_index);
}
bool WINAPI module_media_take(void* instance, AccountMediaFrame* frame) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !frame)
        return false;
    AcquireSRWLockExclusive(&m->lock);
    // The previously taken frame is released by this call.
    if (m->media_has_taken) {
        m->media_head = (m->media_head + 1) % kMediaSlots;
        --m->media_count;
        m->media_has_taken = false;
    }
    if (!m->media_count) {
        if (!m->rest_count)
            ResetEvent(m->response_event);
        ReleaseSRWLockExclusive(&m->lock);
        return false;
    }
    *frame = m->media[m->media_head].frame;
    m->media_has_taken = true;
    ReleaseSRWLockExclusive(&m->lock);
    return true;
}

void stop_call(Module& m) noexcept;
void release_call_textures(SyntheticCall& call) noexcept;

AccountModuleStatus WINAPI module_stop(void** instance, u32 timeout_ms, u64, u64) noexcept {
    AccountModuleStatus status{};
    auto* m = instance ? static_cast<Module*>(*instance) : nullptr;
    if (!m)
        return status;
    stop_call(*m);
    release_call_textures(m->call);
    com_release(m->call.device);
    if (m->call.stop)
        CloseHandle(m->call.stop);
    if (m->call.frame_event)
        CloseHandle(m->call.frame_event);
    SetEvent(m->stop_event);
    SetEvent(m->churn_stop);
    HANDLE threads[3] = {m->ready_thread, m->work_thread, m->churn_thread};
    for (HANDLE thread : threads)
        if (thread) {
            WaitForSingleObject(thread, timeout_ms);
            CloseHandle(thread);
        }
    HANDLE events[5] = {m->gateway_event, m->response_event, m->work_event, m->stop_event, m->churn_stop};
    for (HANDLE handle : events)
        if (handle)
            CloseHandle(handle);
    g_module = nullptr;
    *instance = nullptr;
    status.coordinator_phase = static_cast<u8>(UserAccountCoordinatorPhase::stopped);
    return status;
}

// Typed zero results for every entry the scenarios never exercise. REST
// submissions answer with a 404 so the client's single-flight lanes free up;
// a null request pointer (cancellation) answers nothing.
u64 g_stub_calls[160];

template <typename T> u64 pick_u64(T, u64 previous) noexcept { return previous; }
inline u64 pick_u64(u64 value, u64) noexcept { return value; }
template <typename T> bool null_request(T) noexcept { return false; }
template <typename T> bool null_request(T* pointer) noexcept { return pointer == nullptr; }
template <typename T> bool null_request(const T* pointer) noexcept { return pointer == nullptr; }

template <typename A, typename B> inline constexpr bool kSame = false;
template <typename A> inline constexpr bool kSame<A, A> = true;

template <u32 Slot, typename F> struct Stub;
template <u32 Slot, typename R, typename... A> struct Stub<Slot, R (*)(A...) noexcept> {
    static R WINAPI call(A... arguments) noexcept {
        ((void)arguments, ...);
        ++g_stub_calls[Slot];
        if (g_module) {
            ++g_module->counters.stub_calls;
            g_module->counters.last_stub_slot = Slot;
        }
        if constexpr (kSame<R, void>) {
            return;
        } else {
            if constexpr (kSame<R, RestClientSubmitResult>) {
                u64 request_id = 0;
                bool cancel = false;
                bool first = true;
                ((request_id = pick_u64(arguments, request_id),
                  cancel = cancel || (!first && null_request(arguments)), first = false),
                 ...);
                if (g_module && request_id && !cancel) {
                    Work work{};
                    work.kind = WorkKind::rest_error;
                    work.request_id = request_id;
                    push_work(*g_module, work);
                }
            }
            return R{};
        }
    }
};

// Synthetic call
DWORD WINAPI call_video_thread(void* context) noexcept {
    SyntheticCall& call = static_cast<Module*>(context)->call;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        return 1;
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    u64 due_qpc = static_cast<u64>(now.QuadPart);
    HANDLE handles[2] = {call.stop, timer};
    for (;;) {
        const u64 period = static_cast<u64>(frequency.QuadPart) / static_cast<u64>(maximum<LONG>(call.fps, 1));
        due_qpc += period;
        QueryPerformanceCounter(&now);
        const u64 at = static_cast<u64>(now.QuadPart);
        if (due_qpc + period < at)
            due_qpc = at; // A changed rate or a stall restarts the cadence.
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(
            maximum<u64>(due_qpc > at ? (due_qpc - at) * 10'000'000 / static_cast<u64>(frequency.QuadPart) : 0, 1));
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            break;
        InterlockedIncrement(&call.pictures);
        SetEvent(call.frame_event);
    }
    CloseHandle(timer);
    return 0;
}

void release_call_textures(SyntheticCall& call) noexcept {
    for (u32 i = 0; i < 2; ++i) {
        com_release(call.chroma[i]);
        com_release(call.luma[i]);
    }
    com_release(call.texture);
}

bool ensure_call_textures(SyntheticCall& call) noexcept {
    if (call.texture)
        return true;
    if (!call.device)
        return false;
    constexpr u32 kWidth = kPerfCallVideoWidth, kHeight = kPerfCallVideoHeight;
    constexpr u32 kSliceBytes = kWidth * kHeight * 3 / 2;
    static u8* pixels = static_cast<u8*>(region_take(kSliceBytes * 2));
    if (!pixels)
        return false;
    D3D11_SUBRESOURCE_DATA data[2]{};
    for (u32 slice = 0; slice < 2; ++slice) {
        u8* plane = pixels + slice * kSliceBytes;
        for (u32 y = 0; y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x)
                plane[y * kWidth + x] = static_cast<u8>(32 + (x + y + slice * 64) % 192);
        for (u32 i = kWidth * kHeight; i < kSliceBytes; ++i)
            plane[i] = 128;
        data[slice].pSysMem = plane;
        data[slice].SysMemPitch = kWidth;
    }
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = kWidth;
    texture.Height = kHeight;
    texture.MipLevels = 1;
    texture.ArraySize = 2;
    texture.Format = DXGI_FORMAT_NV12;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_IMMUTABLE;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT status = call.device->CreateTexture2D(&texture, data, &call.texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC plane{};
    plane.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    plane.Texture2DArray.MipLevels = 1;
    plane.Texture2DArray.ArraySize = 1;
    for (u32 i = 0; i < 2 && SUCCEEDED(status); ++i) {
        plane.Texture2DArray.FirstArraySlice = i;
        plane.Format = DXGI_FORMAT_R8_UNORM;
        status = call.device->CreateShaderResourceView(call.texture, &plane, &call.luma[i]);
        plane.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(status))
            status = call.device->CreateShaderResourceView(call.texture, &plane, &call.chroma[i]);
    }
    if (FAILED(status))
        release_call_textures(call);
    return SUCCEEDED(status);
}

bool start_call(Module& m, const PerfAccountControl& control) noexcept {
    SyntheticCall& call = m.call;
    InterlockedExchange(&call.fps, static_cast<LONG>(maximum<u32>(control.video_fps, 1)));
    if (call.active)
        return true;
    call.guild_id = control.guild_id;
    call.channel_id = control.channel_id;
    if (!call.stop)
        call.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!call.frame_event)
        call.frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!call.stop || !call.frame_event)
        return false;
    ResetEvent(call.stop);
    call.thread = CreateThread(nullptr, 64 * 1024, call_video_thread, &m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!call.thread)
        return false;
    call.active = call.started = true;
    SetEvent(m.gateway_event); // The client reads the voice snapshot on its next pump.
    return true;
}

void stop_call(Module& m) noexcept {
    SyntheticCall& call = m.call;
    if (call.thread) {
        SetEvent(call.stop);
        WaitForSingleObject(call.thread, INFINITE);
        CloseHandle(call.thread);
        call.thread = nullptr;
    }
    if (call.active) {
        call.active = false;
        SetEvent(m.gateway_event);
    }
}

bool WINAPI module_voice_snapshot(void* instance, GatewayClientVoiceSnapshot* output) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !output || !m->call.started)
        return false;
    *output = {};
    if (!m->call.active)
        return true;
    output->user_id = kPerfSelfUserId;
    output->guild_id = m->call.guild_id;
    output->channel_id = m->call.channel_id;
    output->generation = 1;
    output->phase = VoiceSetupPhase::credentials_ready;
    output->socket_phase = VoiceSocketPhase::open;
    output->udp_phase = VoiceUdpPhase::ready;
    return true;
}
bool WINAPI module_voice_video_snapshot(void* instance, GatewayClientVoiceVideoSnapshot* output) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !output || !m->call.active)
        return false;
    *output = {};
    output->generation = 1;
    output->count = kPerfCallCameras;
    output->codec = VoiceVideoCodec::h264;
    for (u32 i = 0; i < kPerfCallCameras; ++i) {
        auto& user = output->users[i];
        user.user_id = perf_user_id(1 + i);
        user.ssrc = kPerfCallFirstSsrc + i;
        user.kind = VoiceSourceKind::video;
    }
    return true;
}
HANDLE WINAPI module_voice_video_notification(void* instance) noexcept {
    auto* m = static_cast<Module*>(instance);
    return m && m->call.active ? m->call.frame_event : nullptr;
}
bool WINAPI module_voice_video_wanted(void* instance, u64, bool) noexcept {
    return instance != nullptr;
}
bool WINAPI module_voice_video_device(void* instance, ID3D11Device* device) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m)
        return false;
    release_call_textures(m->call);
    com_release(m->call.device);
    if (device)
        device->AddRef();
    m->call.device = device;
    return true;
}
bool WINAPI module_voice_video_frame(void* instance, u32 ssrc, VideoStreamFrame* output) noexcept {
    auto* m = static_cast<Module*>(instance);
    if (!m || !output || !m->call.active || ssrc < kPerfCallFirstSsrc ||
        ssrc >= kPerfCallFirstSsrc + kPerfCallCameras || !ensure_call_textures(m->call))
        return false;
    SyntheticCall& call = m->call;
    const u32 pictures = static_cast<u32>(call.pictures);
    const u32 slice = pictures & 1u;
    *output = {};
    output->gpu.texture = call.texture;
    output->gpu.subresource = slice;
    output->gpu.width = kPerfCallVideoWidth;
    output->gpu.height = kPerfCallVideoHeight;
    output->gpu.luma = call.luma[slice];
    output->gpu.chroma = call.chroma[slice];
    call.texture->AddRef();
    call.luma[slice]->AddRef();
    call.chroma[slice]->AddRef();
    output->ssrc = ssrc;
    output->sequence = pictures;
    output->display_width = static_cast<u16>(kPerfCallVideoWidth);
    output->display_height = static_cast<u16>(kPerfCallVideoHeight);
    return true;
}

} // namespace

bool fill_api(AccountModuleApi* api) noexcept {
    *api = {};
    api->version = kAccountModuleApiVersion;
    api->bytes = sizeof(AccountModuleApi);
    api->start = module_start;
    api->pump = module_pump;
    api->notification_handles = module_notification_handles;
    api->gateway_source = module_gateway_source;
    api->gateway_pop = module_gateway_pop;
    api->guild_subscribe = module_guild_subscribe;
    api->gateway_refresh = module_gateway_refresh;
    api->history_submit = module_history_submit;
    api->rest_take = module_rest_take;
    api->media_request = module_media_request;
    api->media_guild_icon_request = module_media_guild_icon_request;
    api->media_attachment_request = module_media_attachment_request;
    api->media_advance = module_media_advance;
    api->media_take = module_media_take;
    api->stop = module_stop;
    api->guild_members_request = module_guild_members_request;
    api->voice_snapshot = module_voice_snapshot;
    api->voice_video_snapshot = module_voice_video_snapshot;
    api->voice_video_wanted = module_voice_video_wanted;
    api->voice_video_device = module_voice_video_device;
    api->voice_video_frame = module_voice_video_frame;
    api->voice_video_notification = module_voice_video_notification;
#define MC_STUB(slot, field) api->field = Stub<slot, decltype(api->field)>::call
    MC_STUB(1, private_conversation);
    MC_STUB(2, attachment_upload);
    MC_STUB(3, upload);
    MC_STUB(4, message_submit);
    MC_STUB(5, message_delete);
    MC_STUB(6, message_edit);
    MC_STUB(7, message_reaction);
    MC_STUB(8, reaction_users);
    MC_STUB(9, message_typing);
    MC_STUB(10, media_url_refresh);
    MC_STUB(11, login_start);
    MC_STUB(12, login_submit_password);
    MC_STUB(13, login_submit_mfa);
    MC_STUB(14, login_send_sms);
    MC_STUB(15, login_start_remote_auth);
    MC_STUB(16, login_copy_remote_auth_qr);
    MC_STUB(17, login_copy_remote_auth_user);
    MC_STUB(18, login_cancel_remote_auth);
    MC_STUB(19, login_pump);
    MC_STUB(20, login_copy_captcha_secret);
    MC_STUB(21, login_submit_captcha);
    MC_STUB(22, login_copy_webauthn_challenge);
    MC_STUB(23, login_notification_handle);
    MC_STUB(24, login_stop);
    MC_STUB(25, profile_get);
    MC_STUB(26, application_command);
    MC_STUB(27, set_rest_trace);
    MC_STUB(28, login_submit_verification);
    MC_STUB(29, relationships);
    MC_STUB(30, message_ack);
    MC_STUB(31, login_submit_recovery);
    MC_STUB(32, login_start_reset);
    MC_STUB(33, login_submit_reset);
    MC_STUB(34, login_submit_phone_verify);
    MC_STUB(35, login_submit_phone_resend);
    MC_STUB(36, login_take_phone_reset_token);
    MC_STUB(37, video_select);
    MC_STUB(38, video_control);
    MC_STUB(39, video_status);
    MC_STUB(40, video_take);
    MC_STUB(41, video_acknowledge);
    MC_STUB(42, message_search);
    MC_STUB(43, login_start_browser);
    MC_STUB(44, login_copy_browser_url);
    MC_STUB(45, self_status_snapshot);
    api->self_status_submit = module_self_status_submit;
    MC_STUB(47, self_status_complete);
    MC_STUB(48, media_catalog);
    MC_STUB(49, catalog_preview_request);
    MC_STUB(50, gif_favorites);
    MC_STUB(51, channel_overview);
    MC_STUB(52, pinned_messages);
    MC_STUB(53, message_forward);
    MC_STUB(54, download);
    MC_STUB(55, thread_action);
    MC_STUB(56, thread_browser);
    MC_STUB(57, thread_creation);
    MC_STUB(58, forum_post);
    MC_STUB(59, media_local_preview);
    MC_STUB(60, logout);
    MC_STUB(61, bulk_message_ack);
    MC_STUB(62, poll);
    MC_STUB(63, guild_admin);
    MC_STUB(64, voice_request);
    MC_STUB(66, login_start_captcha_bridge);
    MC_STUB(67, login_captcha_bridge_status);
    MC_STUB(68, login_copy_captcha_bridge_url);
    MC_STUB(69, login_cancel_captcha_bridge);
    MC_STUB(70, voice_user_audio);
    MC_STUB(71, voice_audio_configure);
    MC_STUB(72, voice_audio_status);
    MC_STUB(73, guild_invite);
    MC_STUB(74, emoji_preferences);
    MC_STUB(75, account_settings);
    MC_STUB(76, action_captcha);
    MC_STUB(77, poll_create);
    MC_STUB(78, private_channel);
    MC_STUB(79, custom_status_snapshot);
    MC_STUB(80, custom_status_submit);
    MC_STUB(81, guild_settings);
    MC_STUB(82, mentions);
    MC_STUB(83, guild_assets);
    MC_STUB(88, stream_watch);
    MC_STUB(89, stream_snapshot);
    MC_STUB(90, stream_volume);
    MC_STUB(91, stream_preview);
    MC_STUB(92, stage);
    MC_STUB(93, message_interaction);
    MC_STUB(94, user_social);
    MC_STUB(95, guild_community);
    MC_STUB(96, local_presence);
    MC_STUB(97, activity_sharing);
    MC_STUB(98, soundboard);
    MC_STUB(99, soundboard_audio);
    MC_STUB(100, voice_message);
    MC_STUB(101, profile_edit);
    MC_STUB(102, profile_image);
    MC_STUB(103, guild_moderation);
    MC_STUB(104, guild_members_query_cancel);
    MC_STUB(105, channel_info);
    MC_STUB(106, collection_title);
    MC_STUB(107, client_focus);
    MC_STUB(108, media_vp8_cpu);
    MC_STUB(109, guild_automation);
    MC_STUB(110, credential_replace);
    MC_STUB(111, go_live);
    MC_STUB(112, go_live_snapshot);
    MC_STUB(113, go_live_preview);
#undef MC_STUB
    return true;
}

bool control(const PerfAccountControl* control) noexcept {
    Module* m = g_module;
    if (!m || !control)
        return false;
    switch (control->command) {
    case PerfAccountCommand::start_churn:
        if (m->churn_thread)
            return false;
        m->churn = *control;
        ResetEvent(m->churn_stop);
        m->churn_thread = CreateThread(nullptr, 256 * 1024, churn_thread, m, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        return m->churn_thread != nullptr;
    case PerfAccountCommand::stop_churn:
        if (!m->churn_thread)
            return true;
        SetEvent(m->churn_stop);
        WaitForSingleObject(m->churn_thread, INFINITE);
        CloseHandle(m->churn_thread);
        m->churn_thread = nullptr;
        return true;
    case PerfAccountCommand::start_call:
        return start_call(*m, *control);
    case PerfAccountCommand::stop_call:
        stop_call(*m);
        return true;
    case PerfAccountCommand::message_create: {
        Work work{};
        work.kind = WorkKind::churn_tick;
        work.a = control->channel_id;
        work.b = control->guild_id;
        work.c = 100'000 + m->churn_counter++;
        return push_work(*m, work);
    }
    }
    return false;
}

bool counters(PerfAccountCounters* output) noexcept {
    if (!output)
        return false;
    *output = g_module ? g_module->counters : PerfAccountCounters{};
    output->module_base = reinterpret_cast<u64>(g_region.base);
    output->module_reserved_bytes = g_region.reserved;
    output->module_committed_bytes = g_region.committed;
    return true;
}

} // namespace maicord::perf

extern "C" bool WINAPI maicord_account_get_api(maicord::u32 requested_version,
                                               maicord::AccountModuleApi* api) noexcept {
    return api && requested_version == maicord::kAccountModuleApiVersion && maicord::perf::fill_api(api);
}
extern "C" bool WINAPI maicord_perf_account_control(const maicord::perf::PerfAccountControl* control) noexcept {
    return maicord::perf::control(control);
}
extern "C" bool WINAPI maicord_perf_account_counters(maicord::perf::PerfAccountCounters* output) noexcept {
    return maicord::perf::counters(output);
}
extern "C" BOOL WINAPI maicord_perf_account_entry(HINSTANCE instance, DWORD reason, void*) noexcept {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}


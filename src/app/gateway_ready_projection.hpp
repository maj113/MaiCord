#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/reserved_bytes.hpp"

namespace maicord {
enum class GatewayReadyProjectionPhase : u8 { preflight, publication };
using GatewayReadyContinue = bool (*)(void*, GatewayReadyProjectionPhase) noexcept;
using GatewayReadyPublish = bool (*)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept;
using GatewayReadyGrowRecords = bool (*)(void*, u32, Slice<u32>*) noexcept;
struct GatewayReadyProjectionScratch {
    Slice<u32> arrays;
    Slice<u32> records;
    Slice<char> payload;
    Slice<char> header;
    GatewayReadyGrowRecords grow_records = nullptr;
    void* grow_records_context = nullptr;
    // Storage for a guild record that overflows `payload` (optional).
    GatewayClientLargeRecord large = {};
    // Optional: preflight keeps every typed record here and publication
    // replays them instead of routing READY a second time. Pages are
    // committed only during the call. A record larger than the mailbox ring,
    // or a full reservation, falls back to routing again.
    ReservedBytes* staging = nullptr;
};
struct GatewayReadyProjectionResult {
    GatewayClientDispatchRouteResult route;
    // Guild records left unavailable because their typed route failed, and
    // the first such failure (categorical codes only) for diagnostics.
    GatewayClientDispatchRouteResult degraded_route;
    u32 degraded_guilds = 0;
    GatewayClientFailureStage failure_stage = GatewayClientFailureStage::none;
    GatewayReadyProjectionPhase phase = GatewayReadyProjectionPhase::preflight;
    bool interrupted = false;
    bool publication_started = false;
    bool completed = false;
    constexpr bool ok() const noexcept { return route.ok() && !interrupted && completed; }
};
// Caller validates session identity/sequence and pins immutable READY sources
// until return. Preflight uses the same routes without invoking publish. Source
// and scratch ranges must be disjoint; no whole-READY typed bank is retained.
// A publication interruption may have admitted a prefix and is not rollback.
// Optional record growth preserves its base and existing bytes. Only typed
// scan-capacity failures retry, before that route emits anything; the active
// outer-array index remains disjoint and unchanged during every nested route.
GatewayReadyProjectionResult gateway_ready_project(const GatewayReady& ready,
    GatewayReadyProjectionScratch scratch, u64 sequence, u64 now_ms,
    GatewayReadyPublish publish, GatewayReadyContinue work, void* context) noexcept;
// Ancillary, row-atomic supplemental hydration. Invalid rows are skipped. The
// caller pins the source and fences the current Gateway session until return.
GatewayClientDispatchRouteResult gateway_ready_publish_supplemental_members(
    JsonValue data, u64 self_user_id, Slice<u32> arrays, Slice<u32> records, Slice<char> payload,
    u64 sequence, u64 now_ms, GatewayReadyPublish publish, void* context) noexcept;
// Internal READY routes; merged members replace the guild-local member list.
GatewayClientDispatchRouteResult gateway_client_route_ready_guild(const GatewayEnvelope& envelope,
                                                                  JsonValue members, Slice<u32> scratch,
                                                                  Slice<char> storage, u64 self_user_id,
                                                                  GatewayClientLargeRecord large,
                                                                  GatewayClientEvent* event,
                                                                  ConstSlice<u8>* payload) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_self_members(JsonValue members, u64 guild_id,
                                                                   u64 self_user_id, Slice<u32> scratch,
                                                                   Slice<char> storage,
                                                                   GatewayClientEvent* event,
                                                                   ConstSlice<u8>* payload) noexcept;
} // namespace maicord

#pragma once

#include "maicord/core/win32_records.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// The analytics heartbeat session the desktop client keeps per account: it is
// refreshed while the client is active and expires 30 minutes after its last
// use. Its UUID is client_heartbeat_session_id. Times are Unix milliseconds.
inline constexpr u64 kClientHeartbeatSessionIdleMs = 30 * 60'000;
// While active, the session is refreshed on this period.
inline constexpr u64 kClientHeartbeatIntervalMs = 15 * 60'000;

struct ClientHeartbeatSession {
    u64 created_ms;
    u64 last_used_ms;
    char uuid[kGatewayUserLaunchIdBytes];
    bool valid;
};

bool client_heartbeat_session_live(const ClientHeartbeatSession& session, u64 now_ms) noexcept;
// A restored or persisted session: a v4 UUID and ordered times.
bool client_heartbeat_session_valid(const ClientHeartbeatSession& session) noexcept;

// One account's request metadata, shared by its REST and Gateway workers and
// the UI thread. Like the desktop client, each request first adds the current
// heartbeat session and app state to the super properties, and the header is
// re-encoded only when one of them changes. The base property views must
// outlive the object; every function locks it.
struct ClientSessionMetadata {
    SrwLock lock;
    u32 revision;
    GatewayUserProperties properties;
    ClientHeartbeatSession session;
    // Monotonic time of the last activity refresh; zero before the first.
    u64 refreshed_at_ms;
    bool focused;
    // Request-time keys of the super-properties object: once added they keep
    // their place, and an absent heartbeat session leaves its key unset.
    bool heartbeat_key, app_state_key, app_state_first;
    bool heartbeat_present, app_state_focused;
    char heartbeat_id[kGatewayUserLaunchIdBytes];
    u32 super_properties_bytes;
    u8 super_properties[kRestUserSuperPropertiesMaximumBytes];
};

// False when the properties cannot be encoded. A restored session is kept only
// when valid; whether it is still live is decided at each request.
bool client_session_metadata_initialize(ClientSessionMetadata* metadata,
                                        const GatewayUserProperties& properties,
                                        const ClientHeartbeatSession* restored, bool focused) noexcept;

// The request-time update of the super properties at now_ms (Unix). Copies
// the header into output when its revision differs from *revision; output
// holds kRestUserSuperPropertiesMaximumBytes. Returns the header bytes.
u32 client_session_metadata_request(ClientSessionMetadata* metadata, u64 now_ms, u32* revision,
                                    Slice<u8> output) noexcept;

// Whether the window is focused, the app state the next request reports.
// True when it changed.
bool client_session_metadata_set_focus(ClientSessionMetadata* metadata, bool focused) noexcept;
bool client_session_metadata_focused(const ClientSessionMetadata* metadata) noexcept;

// The super-properties object as the last request left it, for Identify,
// with views into heartbeat_id storage owned by the caller.
GatewayUserProperties client_session_metadata_properties(
    const ClientSessionMetadata* metadata, char (&heartbeat_id)[kGatewayUserLaunchIdBytes]) noexcept;

// Uses the heartbeat session at now_ms (Unix): an active client starts a new
// one when there is none or it expired and marks it used; an inactive one only
// drops an expired session. Returns true when a new session started; *output
// receives the current session. False with an invalid *output also when no
// random UUID could be made.
bool client_session_metadata_use_session(ClientSessionMetadata* metadata, bool active, u64 now_ms,
                                         ClientHeartbeatSession* output) noexcept;
ClientHeartbeatSession client_session_metadata_session(const ClientSessionMetadata* metadata) noexcept;

// The periodic refresh while active: due kClientHeartbeatIntervalMs after the
// previous one (immediately before the first). Monotonic times.
u64 client_session_metadata_refresh_due(const ClientSessionMetadata* metadata) noexcept;
void client_session_metadata_refreshed(ClientSessionMetadata* metadata, u64 now_ms) noexcept;

} // namespace maicord

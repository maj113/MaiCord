#include "maicord/app/client_session_metadata.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

struct ExclusiveLock {
    const SrwLock& lock;
    explicit ExclusiveLock(const SrwLock& value) noexcept : lock(value) {
        AcquireSRWLockExclusive(lock.native());
    }
    ~ExclusiveLock() noexcept {
        ReleaseSRWLockExclusive(lock.native());
    }
};

Utf8View app_state(bool focused) noexcept {
    return focused ? utf8_literal("focused") : utf8_literal("unfocused");
}

GatewayUserProperties object_properties(const ClientSessionMetadata& metadata,
                                        const char* heartbeat_id) noexcept {
    GatewayUserProperties properties = metadata.properties;
    properties.client_heartbeat_session_id =
        metadata.heartbeat_present ? Utf8View{heartbeat_id, kGatewayUserLaunchIdBytes} : Utf8View{};
    properties.client_app_state =
        metadata.app_state_key ? app_state(metadata.app_state_focused) : Utf8View{};
    properties.app_state_first = metadata.app_state_first;
    return properties;
}

bool encode(ClientSessionMetadata* metadata) noexcept {
    u8 scratch[kRestUserSuperPropertiesMaximumBytes];
    const RestUserPropertiesResult written = write_rest_user_super_properties(
        object_properties(*metadata, metadata->heartbeat_id), {scratch, sizeof(scratch)},
        {metadata->super_properties, sizeof(metadata->super_properties)});
    if (!written.ok())
        return false;
    metadata->super_properties_bytes = written.bytes;
    ++metadata->revision;
    return true;
}

// Locked. The session the client would report at now_ms.
const ClientHeartbeatSession* live_session(const ClientSessionMetadata& metadata, u64 now_ms) noexcept {
    return client_heartbeat_session_live(metadata.session, now_ms) ? &metadata.session : nullptr;
}

} // namespace

bool client_heartbeat_session_live(const ClientHeartbeatSession& session, u64 now_ms) noexcept {
    return session.valid && now_ms < session.last_used_ms + kClientHeartbeatSessionIdleMs;
}

bool client_heartbeat_session_valid(const ClientHeartbeatSession& session) noexcept {
    if (!session.valid || session.created_ms == 0 || session.last_used_ms < session.created_ms)
        return false;
    for (u32 index = 0; index < kGatewayUserLaunchIdBytes; ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? session.uuid[index] != '-' : !ascii_is_hex_digit(session.uuid[index]))
            return false;
    }
    return true;
}

bool client_session_metadata_initialize(ClientSessionMetadata* metadata,
                                        const GatewayUserProperties& properties,
                                        const ClientHeartbeatSession* restored, bool focused) noexcept {
    if (!metadata)
        return false;
    reset_value(metadata);
    copy_value(&metadata->properties, properties);
    metadata->properties.client_heartbeat_session_id = {};
    metadata->properties.client_app_state = {};
    if (restored && client_heartbeat_session_valid(*restored))
        metadata->session = *restored;
    metadata->focused = focused;
    return encode(metadata);
}

u32 client_session_metadata_request(ClientSessionMetadata* metadata, u64 now_ms, u32* revision,
                                    Slice<u8> output) noexcept {
    if (!metadata || !revision || output.count < kRestUserSuperPropertiesMaximumBytes)
        return 0;
    ExclusiveLock guard(metadata->lock);
    // As the desktop client extends its super properties: a changed value is
    // set, and a key seen for the first time follows the existing ones.
    bool changed = false;
    const ClientHeartbeatSession* session = live_session(*metadata, now_ms);
    if (session ? !metadata->heartbeat_present ||
                      !bytes_equal(metadata->heartbeat_id, session->uuid, kGatewayUserLaunchIdBytes)
                : metadata->heartbeat_present) {
        metadata->heartbeat_key = true;
        metadata->heartbeat_present = session != nullptr;
        if (session)
            copy_bytes(metadata->heartbeat_id, session->uuid, kGatewayUserLaunchIdBytes);
        changed = true;
    }
    if (!metadata->app_state_key || metadata->app_state_focused != metadata->focused) {
        if (!metadata->app_state_key)
            metadata->app_state_first = !metadata->heartbeat_key;
        metadata->app_state_key = true;
        metadata->app_state_focused = metadata->focused;
        changed = true;
    }
    if (changed && !encode(metadata))
        return 0;
    if (*revision != metadata->revision) {
        copy_bytes(output.data, metadata->super_properties, metadata->super_properties_bytes);
        *revision = metadata->revision;
    }
    return metadata->super_properties_bytes;
}

bool client_session_metadata_set_focus(ClientSessionMetadata* metadata, bool focused) noexcept {
    if (!metadata)
        return false;
    ExclusiveLock guard(metadata->lock);
    const bool changed = metadata->focused != focused;
    metadata->focused = focused;
    return changed;
}

bool client_session_metadata_focused(const ClientSessionMetadata* metadata) noexcept {
    if (!metadata)
        return false;
    AcquireSRWLockShared(metadata->lock.native());
    const bool focused = metadata->focused;
    ReleaseSRWLockShared(metadata->lock.native());
    return focused;
}

GatewayUserProperties client_session_metadata_properties(
    const ClientSessionMetadata* metadata, char (&heartbeat_id)[kGatewayUserLaunchIdBytes]) noexcept {
    AcquireSRWLockShared(metadata->lock.native());
    copy_bytes(heartbeat_id, metadata->heartbeat_id, kGatewayUserLaunchIdBytes);
    const GatewayUserProperties properties = object_properties(*metadata, heartbeat_id);
    ReleaseSRWLockShared(metadata->lock.native());
    return properties;
}

bool client_session_metadata_use_session(ClientSessionMetadata* metadata, bool active, u64 now_ms,
                                         ClientHeartbeatSession* output) noexcept {
    if (!metadata || !output)
        return false;
    ExclusiveLock guard(metadata->lock);
    ClientHeartbeatSession& session = metadata->session;
    bool started = false;
    if (active) {
        if (!client_heartbeat_session_live(session, now_ms)) {
            char uuid[kGatewayUserLaunchIdBytes + 1];
            if (!user_client_random_uuid(uuid)) {
                reset_value(output);
                return false;
            }
            copy_bytes(session.uuid, uuid, kGatewayUserLaunchIdBytes);
            session.created_ms = now_ms;
            session.valid = true;
            started = true;
        }
        session.last_used_ms = now_ms;
    } else if (!client_heartbeat_session_live(session, now_ms)) {
        reset_value(&session);
    }
    *output = session;
    return started;
}

ClientHeartbeatSession client_session_metadata_session(const ClientSessionMetadata* metadata) noexcept {
    ClientHeartbeatSession session{};
    if (!metadata)
        return session;
    AcquireSRWLockShared(metadata->lock.native());
    session = metadata->session;
    ReleaseSRWLockShared(metadata->lock.native());
    return session;
}

u64 client_session_metadata_refresh_due(const ClientSessionMetadata* metadata) noexcept {
    AcquireSRWLockShared(metadata->lock.native());
    const u64 refreshed = metadata->refreshed_at_ms;
    ReleaseSRWLockShared(metadata->lock.native());
    return refreshed ? refreshed + kClientHeartbeatIntervalMs : 0;
}

void client_session_metadata_refreshed(ClientSessionMetadata* metadata, u64 now_ms) noexcept {
    ExclusiveLock guard(metadata->lock);
    metadata->refreshed_at_ms = now_ms;
}

} // namespace maicord

#pragma once

#include "maicord/platform/network_worker.hpp"
#include "maicord/protocol/cdn_http.hpp"

namespace maicord {

inline constexpr u32 kCdnClientMinimumResponseBytes = 4 * 1024;
inline constexpr u32 kCdnClientDefaultTimeoutMs = 30 * 1000;
inline constexpr u32 kCdnClientMaximumRedirects = 3;

enum class CdnClientPhase : u8 {
    empty,
    starting,
    idle,
    queued,
    resolving,
    connecting,
    tls_handshake,
    sending,
    receiving,
    redirecting,
    draining,
    response_ready,
    stopping,
    stopped,
    failed,
};

enum class CdnClientError : u8 {
    none,
    invalid_input,
    memory,
    event,
    worker,
    request,
    resolve,
    connect,
    tls,
    send,
    receive,
    response,
    redirect,
    redirect_limit,
    timeout,
    cancelled,
};

enum class CdnClientSubmitError : u8 {
    none,
    invalid_input,
    target_capacity,
    etag_capacity,
    busy,
    signal,
};

enum class CdnClientTakeError : u8 {
    none,
    invalid_input,
    empty,
    output_too_small,
    consumer_rejected,
};

struct CdnClientRequest {
    ConstSlice<u8> target;
    ConstSlice<u8> etag;
    u64 request_id;
    u32 timeout_ms;
    CdnHost host;
    CdnImageType image_type;
    // MP4 only, without an ETag; follows redirects with the same range.
    CdnByteRange range;
};

struct CdnClientResponse {
    u64 request_id;
    u64 elapsed_us;
    u64 expires_at_seconds;
    u32 body_bytes;
    u32 native_error;
    u32 etag_bytes;
    u32 conditional_etag_bytes;
    u16 status_code;
    u8 redirects;
    CdnClientError error;
    CdnHttpError http_error;
    bool store_allowed;
    bool must_revalidate;
    bool reused_connection;
    u8 etag[kCdnMaximumEtagBytes];
    u8 conditional_etag[kCdnMaximumEtagBytes];
    // Range requests only; see CdnResponseResult.
    CdnByteRange requested_range;
    u64 range_first;
    u64 entity_bytes;

    constexpr bool transport_ok() const noexcept {
        return error == CdnClientError::none;
    }
};

CdnResponseResult cdn_client_response_policy(
    const CdnClientResponse& response) noexcept;

struct CdnClientResult {
    u64 connections;
    u64 requests;
    u64 reused_requests;
    u64 responses;
    u32 native_error;
    CdnClientError error;
    CdnClientPhase phase;

    constexpr bool ok() const noexcept {
        return error == CdnClientError::none;
    }
};

struct CdnClientSubmitResult {
    CdnClientSubmitError error;

    constexpr bool ok() const noexcept {
        return error == CdnClientSubmitError::none;
    }
};

struct CdnClientTakeResult {
    u32 body_bytes;
    CdnClientTakeError error;

    constexpr bool ok() const noexcept {
        return error == CdnClientTakeError::none;
    }
};

using CdnClientResponseConsumer = bool (*)(
    void* context, const CdnClientResponse& response,
    ConstSlice<u8> body) noexcept;

struct CdnClient {
    NetworkWorker worker;
    void* runtime;
    HANDLE response_event;
    volatile LONG phase;
    volatile LONG error;
    volatile LONG native_error;
    volatile LONG request_state;
    volatile LONG64 current_request_id;
    volatile LONG64 connections;
    volatile LONG64 requests;
    volatile LONG64 reused_requests;
    volatile LONG64 responses;
};

CdnClientResult cdn_client_start(
    CdnClient* client, u32 response_capacity) noexcept;
CdnClientSubmitResult cdn_client_submit(
    CdnClient* client,
    const CdnClientRequest& request) noexcept;
bool cdn_client_cancel(CdnClient* client,
                       u64 request_id) noexcept;
CdnClientResult cdn_client_wait_response(
    CdnClient* client, u32 timeout_ms) noexcept;
CdnClientTakeResult cdn_client_take_response(
    CdnClient* client, CdnClientResponse* response,
    Slice<u8> body_output) noexcept;
// Invokes the consumer on the caller thread with the stable in-place body,
// then clears the published response. A rejected consumer leaves it ready.
CdnClientTakeResult cdn_client_consume_response(
    CdnClient* client, CdnClientResponseConsumer consumer,
    void* context) noexcept;
HANDLE cdn_client_notification_handle(
    const CdnClient* client) noexcept;
CdnClientResult cdn_client_status(
    const CdnClient* client) noexcept;
CdnClientResult cdn_client_stop(
    CdnClient* client, u32 timeout_ms) noexcept;
u32 cdn_client_runtime_bytes(u32 response_capacity) noexcept;

} // namespace maicord

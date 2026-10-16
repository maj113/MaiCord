#pragma once
#include "maicord/protocol/account_requirement.hpp"

#include "maicord/platform/network_worker.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/user_auth.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {

using RestTraceFunction = void (*)(const char* record) noexcept;
// Configure before creating any clients; callback remains valid until all
// workers have joined. Receives metadata only, never request bodies.
void rest_client_set_trace(RestTraceFunction trace) noexcept;

// Room for a kick, whose audit reason travels percent-encoded in the query.
inline constexpr u32 kRestClientMaximumTargetBytes = 128 + kRestAuditReasonMaximumEncodedBytes;
inline constexpr u32 kRestClientMaximumRequestBodyBytes = 64 * 1024;
// Opt-in ceiling for rest_client_submit_large (image data URIs and
// multipart uploads). The bank exists only for that one transaction.
inline constexpr u32 kRestClientMaximumLargeRequestBodyBytes = 12 * 1024 * 1024;
inline constexpr u32 kRestClientMinimumResponseBytes = 4 * 1024;
inline constexpr u32 kRestClientMaximumResponseBytes = 2 * 1024 * 1024;
inline constexpr u32 kRestUserSettingsMaximumBodyBytes = 2 * 1024 * 1024;
inline constexpr u64 kRestUserSettingsRouteId = 0x5553455454494e47ull;
inline constexpr u32 kRestClientDefaultTimeoutMs = 30 * 1000;
inline constexpr u32 kUserAuthMaximumRequestBodyBytes = 32 * 1024;
inline constexpr u32 kUserAuthResetRequestBodyBytes = 72 * 1024;
inline constexpr u32 kUserAuthMaximumResponseBytes = 32 * 1024;

enum class RestTransactionPhase : u8 {
    idle,
    acquired,
    sending,
    receiving,
    complete,
    cancelled,
    failed,
};

// Pure transaction lifetime used by the asynchronous worker. Plaintext bytes
// are committed only after the complete encrypted TLS record has been sent.
struct RestTransaction {
    RestRateLease lease;
    u64 request_id;
    u64 deadline_ms;
    u32 request_bytes;
    u32 committed_bytes;
    RestTransactionPhase phase;
    bool reused_connection;
};

bool rest_transaction_acquire(RestTransaction* transaction, u64 request_id,
                              const RestRateLease& lease, u64 deadline_ms,
                              bool reused_connection) noexcept;
bool rest_transaction_begin_send(RestTransaction* transaction, u32 request_bytes) noexcept;
bool rest_transaction_commit_plaintext(RestTransaction* transaction, u32 bytes) noexcept;
bool rest_transaction_complete(RestTransaction* transaction) noexcept;
bool rest_transaction_cancel(RestTransaction* transaction) noexcept;
bool rest_transaction_fail(RestTransaction* transaction) noexcept;

enum class RestClientPhase : u8 {
    empty,
    starting,
    idle,
    queued,
    rate_wait,
    resolving,
    connecting,
    tls_handshake,
    sending,
    receiving,
    draining,
    response_ready,
    stopping,
    stopped,
    failed,
};

enum class RestClientError : u8 {
    none,
    invalid_input,
    memory,
    event,
    worker,
    request,
    rate,
    resolve,
    connect,
    tls,
    send,
    receive,
    response,
    timeout,
    cancelled,
    account_restricted,
};

enum class RestClientSubmitError : u8 {
    none,
    invalid_input,
    target_capacity,
    body_capacity,
    busy,
    signal,
    account_restricted,
    memory,
};

constexpr RestClientError rest_client_response_error(
    RestResponseError error) noexcept {
    // A clean TLS close_notify can still truncate an HTTP response (including
    // an idle keep-alive socket closed before the next request). It is a
    // transport failure, not a malformed response to be permanently rejected.
    return error == RestResponseError::unexpected_close ? RestClientError::receive
                                                               : RestClientError::response;
}

enum class RestClientTakeError : u8 {
    none,
    invalid_input,
    empty,
    output_too_small,
    consumer_rejected,
};

struct RestClientRequest {
    ConstSlice<u8> target;
    ConstSlice<u8> body;
    RestRouteKey route;
    u64 request_id;
    u32 timeout_ms;
    bool json_body;
    bool identity_response;
    // Percent-encoded X-Audit-Log-Reason value, copied at submit; empty omits it.
    ConstSlice<u8> audit_reason;
    // rest_extra_headers_valid lines, copied at submit; may hold secrets.
    ConstSlice<u8> extra_headers;
    // Non-empty sends the body as multipart/form-data with this boundary
    // (rest_multipart_boundary_valid); copied at submit.
    ConstSlice<u8> multipart_boundary;
    // With an empty audit_reason, still sends the header with no value (the
    // official client's "" reason). Refused on GET like any reason.
    bool send_empty_audit_reason = false;
};

// All pointed-to values must already reside in locked SecureBuffers. The
// transport copies them synchronously into its own locked arena; the caller
// may clear its buffers as soon as submit returns.
struct RestUserAuthRequest {
    const SecureBuffer* body;
    const SecureBuffer* captcha_key;
    const SecureBuffer* captcha_rqtoken;
    const SecureBuffer* captcha_session_id;
    u64 request_id;
    u32 timeout_ms;
    UserAuthOperation operation;
};

// Numeric, session-local counters and an endpoint template ID, never a URL,
// channel ID, nonce, credential, or message content.
struct RestRequestDiagnostics {
    u64 route_id;
    u64 attempts;
    u64 completions;
    RestMethod method;
};

struct RestClientResponse {
    RateLimitHeaders rate;
    u64 request_id;
    u64 retry_at_ms;
    u64 elapsed_us;
    u32 body_bytes;
    u32 native_error;
    u16 status_code;
    RestClientError error;
    RestContentType content_type;
    RestContentEncoding content_encoding;
    RestBodyKind body_kind;
    bool rate_limited;
    bool global_rate_limit;
    bool reused_connection;
    ApiRejection rejection;
    RestRequestDiagnostics diagnostics;
    u32 wire_body_bytes;
    u32 decode_us;
    RestContentEncoding wire_content_encoding;

    constexpr bool transport_ok() const noexcept {
        return error == RestClientError::none;
    }
};

using RestUserAuthConsumeFunction = bool (*)(void* context,
                                             const RestClientResponse& response,
                                             ConstSlice<u8> locked_body) noexcept;

struct RestClientResult {
    u64 connections;
    u64 requests;
    u64 reused_requests;
    u64 responses;
    u32 native_error;
    RestClientError error;
    RestClientPhase phase;
    bool authentication_rejected;

    constexpr bool ok() const noexcept {
        return error == RestClientError::none;
    }
};

struct RestClientSubmitResult {
    RestClientSubmitError error;

    constexpr bool ok() const noexcept {
        return error == RestClientSubmitError::none;
    }
};

struct RestClientTakeResult {
    u32 body_bytes;
    RestClientTakeError error;

    constexpr bool ok() const noexcept {
        return error == RestClientTakeError::none;
    }
};

struct RestSharedState;
struct RestClient {
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
    volatile LONG authentication_rejected;
    volatile LONG required_action;
    RestSharedState* shared;
};

// Logout-specific take: unlike a generic response take, this consumes and
// erases only the authenticated logout response and exposes no body.
// Preparation reserves PRIMARY, cancels only the unrelated request it captured,
// and takes that request's final response. Busy means the same intent must
// wait; an already-accepted logout is never cancelled by preparation.
RestClientSubmitResult rest_client_prepare_logout(RestClient* client,
                                                  u64 request_id) noexcept;
bool rest_client_cancel_logout(RestClient* client, u64 request_id) noexcept;
RestClientTakeResult rest_client_take_logout_response(
    RestClient* client, u64 request_id, RestClientResponse* response) noexcept;

// One account-wide scheduler shared by two independent connections. The lock
// only covers scheduling and committing results, never DNS, TLS, HTTP, decoding
// or waiting.
struct RestSharedState {
    SrwLock lock;
    RestRateScheduler scheduler;
    RestRateBucket buckets[32];
    RestRateBinding bindings[64];
    RestClient* clients[2];
    RestClient* mutation_owner;
    volatile LONG authentication_rejected;
    volatile LONG required_action;
};

// Choose shared scheduling at startup to avoid allocating unused private tables.
// Shared state must outlive both workers, including failed stop attempts.

// Non-secret normal-user request metadata. All views are copied synchronously
// into the client; timezone may be empty when no verified IANA name is
// available. The credential still moves only after complete startup.
struct RestUserConfiguration {
    ConstSlice<u8> super_properties;
    Utf8View locale;
    Utf8View timezone;
};

// Start requires a zero-initialized or fully stopped client. A live client is
// rejected without changing its allocations, handles, or credential storage.
// Starts a credential-free client for documented public endpoints such as Get
// Gateway. The worker connects lazily on its first request.
RestClientResult rest_client_start_public(RestClient* client,
                                          u32 response_capacity,
                                          RestSharedState* shared = nullptr) noexcept;

// Starts a scoped OAuth2 Bearer client. The credential moves only on success;
// the source remains intact on validation/allocation/thread failure.
// Normal Discord user sessions use rest_client_start_user instead.
RestClientResult rest_client_start_authorized(RestClient* client,
                                              SecureBuffer* credential,
                                              RestAuthorization authorization,
                                              u32 response_capacity) noexcept;

// Starts a normal-user client. Authorization is the session token with no
// authorization-scheme prefix, and every request carries the copied public
// client metadata.
RestClientResult rest_client_start_user(RestClient* client, SecureBuffer* credential,
                                        const RestUserConfiguration* configuration,
                                        u32 response_capacity,
                                        RestSharedState* shared = nullptr) noexcept;

// Starts a credential-free client whose request surface is restricted to the
// enumerated user-auth operations, including the one-time Remote Auth ticket
// exchange. Request/TLS-record/decrypted-response storage is VirtualLock-backed
// and erased before release.
// Startup caches nonsecret client-properties, locale, and optional IANA timezone
// in the ordinary owner allocation. Auth requests never attach Authorization.
RestClientResult rest_client_start_user_auth(RestClient* client,
                                             u32 response_capacity) noexcept;

// Reset alone selects a larger protected request partition. Both accepted
// profiles preserve the request/response alias and synchronous copy contract.
RestClientResult rest_client_start_user_auth_capacity(
    RestClient* client, u32 response_capacity, u32 request_capacity) noexcept;
u32 rest_client_user_auth_capacity_runtime_bytes(
    u32 response_capacity, u32 request_capacity) noexcept;

// One request may be queued or active. Target/body bytes are copied before
// the request is handed to the worker. The route is a normalized caller-owned identity
// used only for server-learned rate scheduling.
void rest_client_set_required_action(RestClient* client,
                                     AccountRequirement action) noexcept;
RestClientSubmitResult rest_client_submit(RestClient* client,
                                          const RestClientRequest& request) noexcept;
// The common request shape: default timeout, no identity response, audit
// reason, extra headers or multipart boundary.
inline RestClientSubmitResult rest_client_submit(RestClient* client,
                                                 ConstSlice<u8> target, ConstSlice<u8> body,
                                                 RestRouteKey route, u64 request_id,
                                                 bool json_body) noexcept {
    return rest_client_submit(
        client, {target, body, route, request_id, kRestClientDefaultTimeoutMs, json_body, false});
}
// Same contract with a body up to kRestClientMaximumLargeRequestBodyBytes.
// Bodies above the ordinary ceiling are copied into a bank allocated for this
// transaction (sized for the body or the response, whichever is larger) and
// erased and released with the response; smaller bodies take the fixed path.
RestClientSubmitResult rest_client_submit_large(RestClient* client,
                                                const RestClientRequest& request) noexcept;
// Opt-in for a body at *body_owner from VirtualAlloc with MEM_RESERVE |
// MEM_COMMIT and PAGE_READWRITE, requested with exactly request.body.count
// bytes. request.body.data must equal *body_owner. The caller has sole ownership.
// A body above the ordinary ceiling transfers if it also covers the response
// capacity. Otherwise, submission copies it as above. Transfer sets *body_owner
// to null before publication. REST retains ownership on every return path;
// a concurrent claimant can keep the bank until response or stop cleanup.
// On return, the caller must erase/release only a nonnull *body_owner, regardless
// of the result. Existing queued/TLS/response lifetime rules remain unchanged.
RestClientSubmitResult rest_client_submit_large_owned(
    RestClient* client, const RestClientRequest& request, u8** body_owner) noexcept;
// A bodyless GET, or a POST with an ordinary JSON body (a members search),
// whose response may exceed the configured capacity (a large list such as a
// server's bans): the response lands in a bank of
// response_capacity bytes allocated for this transaction and released with
// the response. Take it with output of at least the reported body size.
RestClientSubmitResult rest_client_submit_large_response(RestClient* client,
                                                         const RestClientRequest& request,
                                                         u32 response_capacity) noexcept;
// Normal-user settings-proto/2 GET/PATCH only, with the normalized route above
// and no major parameter. A transaction-owned body bank is copied
// synchronously and released after response consumption or joined shutdown.
// Short output keeps the response and bank for a later take.
RestClientSubmitResult rest_client_submit_user_settings(
    RestClient* client, const RestClientRequest& request,
    u32 response_capacity) noexcept;
RestClientSubmitResult rest_client_submit_user_auth(
    RestClient* client, const RestUserAuthRequest& request) noexcept;
// A true result atomically claims the pending/active request: the response
// for that request is cancelled, never a success committed concurrently.
// False leaves a response that is already being committed or ready unchanged.
bool rest_client_cancel(RestClient* client, u64 request_id) noexcept;
// Cancel as a submit result: invalid input for a null client or zero ID,
// none when the request was claimed, busy when it was not.
inline RestClientSubmitResult rest_client_cancel_result(RestClient* client,
                                                        u64 request_id) noexcept {
    if (!client || !request_id)
        return {RestClientSubmitError::invalid_input};
    return {rest_client_cancel(client, request_id) ? RestClientSubmitError::none
                                                           : RestClientSubmitError::busy};
}
// Producer thread. Copies header lines (e.g. a solved CAPTCHA) that the next
// submit with exactly this request ID appends once; any other submit leaves
// them armed. Empty headers or a zero ID clear. Erased when consumed.
bool rest_client_arm_headers(RestClient* client, u64 request_id, ConstSlice<u8> headers) noexcept;

RestClientResult rest_client_wait_response(RestClient* client,
                                           u32 timeout_ms) noexcept;
RestClientTakeResult rest_client_take_response(RestClient* client,
                                               RestClientResponse* response,
                                               Slice<u8> body_output) noexcept;
// Invokes the consumer synchronously over the internal locked response body.
// Rejection leaves the response ready and untouched so the caller can retry.
RestClientTakeResult rest_client_consume_user_auth_response(RestClient* client,
                                                            RestClientResponse* response,
                                                            RestUserAuthConsumeFunction consumer,
                                                            void* context) noexcept;
HANDLE rest_client_notification_handle(const RestClient* client) noexcept;
RestClientResult rest_client_status(const RestClient* client) noexcept;
RestClientResult rest_client_stop(RestClient* client, u32 timeout_ms) noexcept;
// Stops an auth worker and preserves only a published HTTP 429 request ID and
// retry deadline before releasing storage. No response body is visited. A failed
// join retains the owner; receipt is empty unless a complete response is ready.
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept;

// Committed runtime bytes for a configured response body capacity.
u32 rest_client_runtime_bytes(u32 response_capacity) noexcept;
u32 rest_client_user_runtime_bytes(u32 response_capacity) noexcept;
// Committed ordinary+locked bytes for the user-auth client.
u32 rest_client_user_auth_runtime_bytes(u32 response_capacity) noexcept;

} // namespace maicord

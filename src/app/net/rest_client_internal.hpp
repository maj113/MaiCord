#pragma once
// Shared by rest_client.cpp and its parts (rest_client_*.cpp); private to them.
#include "maicord/app/net/rest_client.hpp"
#include "request_state.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/platform/net/tls_stream.hpp"
#include "maicord/media/compression/deflate.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace rest_client_detail {}
using namespace rest_client_detail;
namespace rest_client_detail {

extern RestTraceFunction g_trace;
extern volatile LONG g_trace_serial;

inline constexpr wchar_t kApiHostWide[] = L"discord.com";
inline constexpr u32 kEncryptedInputBytes = 128 * 1024;
inline constexpr u32 kHandshakeOutputBytes = 256 * 1024;
inline constexpr u32 kRecordOutputBytes = 20 * 1024;

inline constexpr u32 kUserAuthEncryptedInputBytes = 32 * 1024;
inline constexpr u32 kUserAuthRecordOutputBytes = 20 * 1024;

inline constexpr u32 kRateStructuralCount = 256;
inline constexpr u32 kRateBucketCount = 32;
inline constexpr u32 kRateBindingCount = 64;
inline constexpr u32 kRequestSignalValue = 0x52514553u;
inline constexpr u32 kCancelSignalValue = 0x43414E43u;
inline constexpr u32 kRateSignalValue = 0x52415445u;
inline constexpr u32 kMaximumRequestTimeoutMs = 10 * 60 * 1000;
inline constexpr u32 kMaximumCredentialBytes = 2 * 1024;

struct RestEndpointKey {
    u64 key_low, key_high;
    u32 path_bytes;
};
struct RestEndpointLease {
    u16 index, generation; // generation zero means no reservation
};
enum class EndpointGateKind : u8 {
    acquired,
    waiting,
    busy,
    error
};
struct EndpointGateResult {
    EndpointGateKind kind;
    u64 wait_until_ms;
};

RestEndpointKey endpoint_key(ConstSlice<u8> target) noexcept;
EndpointGateResult endpoint_gate_acquire(RestEndpointGate* gate, RestEndpointKey key, u64 now_ms,
                                         RestEndpointLease* lease) noexcept;
bool endpoint_gate_release(RestEndpointGate* gate, RestEndpointLease* lease, u64 now_ms,
                           bool settled = true) noexcept;

struct RestClientCommand {
    RestRouteKey route;
    RestEndpointKey endpoint_key;
    u64 request_id;
    i64 submitted_counter;
    u32 target_bytes;
    u32 body_bytes;
    u32 head_bytes;
    u32 timeout_ms;
    bool json_body;
    u32 audit_reason_bytes;
    u32 extra_headers_bytes;
    u32 multipart_boundary_bytes;
    bool send_empty_audit_reason;
    // A GET the client retries: tries left, failed tries so far, and when the next may be admitted.
    u8 retries_left, failed_requests;
    u64 retry_not_before_ms;
};

struct RestClientRuntime {
    TlsStream transport;
    NetworkSignal request_signal;
    NetworkSignal cancel_signal;
    NetworkSignal rate_signal;
    RestClient* owner;
    RestResponse response;
    RestResponseStep response_step;
    RestRateScheduler* scheduler;
    RestEndpointGate* endpoint_gate;
    RestEndpointLease endpoint_lease;
    RestTransaction transaction;
    RestClientCommand command;
    RestClientResponse completed;
    // Request-kind ID set by the producer; survives worker command erasure
    // and is read atomically by logout cancellation and response intake.
    volatile LONG64 logout_request_id;
    volatile LONG64 logout_reservation_id;
    u64 logout_preempt_id;
    bool logout_cancelled;
    SecureBuffer credential;
    SecureBuffer secret_storage;
    Clock clock;
    i64 request_started_counter;
    i64 phase_counter, first_byte_counter, receive_counter, ready_counter;
    u64 trace_request_id;
    u32 trace_worker_id;
    u8 trace_lane;
    RestClientPhase trace_phase;
    // Producer-only rejection aggregation, separate from the active command.
    u64 busy_since_ms, busy_count;
    u8* request_target;
    u8* request_body;
    u8* encrypted_storage;
    u8* handshake_output;
    u8* record_output;
    u8* request_head;
    u8* response_input;
    u8* response_body;
    u8* decompression;
    u8* default_body;
    u8* extended_body;
    u32 extended_body_bytes;
    u32 default_response_capacity;
    u32 decompression_capacity;
    DeflateWorkspace inflate_workspace;
    u32 wire_body_bytes;
    u32 decode_us;
    RestContentEncoding wire_encoding;
    bool transport_ready;
    bool write_commit_failed;
    bool response_callback_failed;
    bool draining;
    bool stopping;
    bool user_auth_mode;
    bool waiting_for_peer;
    u8* user_super_properties;
    char* user_locale;
    char* user_timezone;
    char* user_accept_language;
    ClientSessionMetadata* metadata;
    u32 metadata_revision;
    u64 rate_wait_until_ms;
    u64 transport_idle_since_ms;
    u32 allocation_bytes;
    u32 response_capacity;
    u32 request_body_capacity;
    u32 request_head_capacity;
    u32 connection_request_count;
    u32 pending_native_error;
    u32 user_super_properties_bytes;
    u32 user_locale_bytes;
    u32 user_timezone_bytes;
    u32 user_accept_language_bytes;
    u32 rate_structural_scratch[kRateStructuralCount];
    RestAuthorization authorization;
    RestClientError pending_error;
    u8 audit_reason[kRestAuditReasonMaximumEncodedBytes];
    u8 extra_headers[kRestExtraHeadersMaximumBytes];
    u8 multipart_boundary[kRestMultipartBoundaryMaximumBytes];
    // Producer-owned one-shot headers for armed_request_id.
    u8 armed_headers[kRestExtraHeadersMaximumBytes];
    u64 armed_request_id;
    u32 armed_headers_bytes;
    // The Cookie value of the request being written, erased once it is.
    u8 cookie_header[kCookieHeaderMaximumBytes];
};

struct RateLock {
    RestSharedState* shared;
    explicit RateLock(RestClientRuntime* runtime) noexcept : shared(runtime->owner->shared) {
        if (shared)
            AcquireSRWLockExclusive(shared->lock.native());
    }
    ~RateLock() noexcept {
        if (shared)
            ReleaseSRWLockExclusive(shared->lock.native());
    }
};

RestRateScheduler* rate_scheduler(RestClientRuntime* runtime) noexcept;
void rate_changed(RestClientRuntime* runtime) noexcept;
void cancel_rate_lease(RestClientRuntime* runtime) noexcept;
bool authentication_rejected(const RestClient* client) noexcept;
AccountRequirement required_action(RestClient* client) noexcept;
u32 user_auth_secure_bytes(u32 response_capacity, u32 request_capacity) noexcept;
u32 allocation_bytes(u32 response_capacity, bool user_auth_mode, bool normal_user, bool shared = false) noexcept;
void assign_storage(RestClientRuntime* runtime, RestSharedState* shared = nullptr) noexcept;
void publish_phase(RestClient* client, RestClientPhase phase) noexcept;
void trace_identity(TraceRecord& line, const RestClientRuntime* runtime, u64 request) noexcept;
void publish_worker_phase(RestClient* client, RestClientRuntime* runtime,
                          RestClientPhase phase) noexcept;
void trace_busy(RestClient* client, RestClientRuntime* runtime,
                const RestClientRequest& request) noexcept;
RestClientResult invalid_result() noexcept;
RestClientRuntime* runtime_of(RestClient* client) noexcept;
bool worker_ending(const RestClient* client) noexcept;
RestClientResult client_result(const RestClient* client) noexcept;
void publish_client_failure(RestClient* client, RestClientError error,
                            u32 native_error) noexcept;
bool valid_user_configuration(const RestUserConfiguration& configuration) noexcept;
bool copy_user_configuration(RestClientRuntime* runtime,
                             const RestUserConfiguration& configuration) noexcept;
CookieJar* request_cookies(const RestClientRuntime* runtime) noexcept;
CookieRequest cookie_request(const RestClientRuntime* runtime) noexcept;
bool initialize_auth_user_configuration(RestClientRuntime* runtime) noexcept;
bool valid_locked_value(const SecureBuffer* value, u32 maximum_bytes, bool required) noexcept;
Utf8View locked_utf8(const SecureBuffer* value) noexcept;
RestRouteKey user_auth_route(UserAuthOperation operation) noexcept;
void erase_command(RestClientRuntime* runtime, bool erase_body = true) noexcept;
void release_decompression(RestClientRuntime* runtime) noexcept;
void release_extended_body(RestClientRuntime* runtime) noexcept;
void release_completed_response(RestClient* client, RestClientRuntime* runtime,
                                u32 body_bytes) noexcept;
void close_transport(RestClientRuntime* runtime) noexcept;
void retire_idle_transport(RestClientRuntime* runtime, u64 now_ms) noexcept;
bool transaction_in_progress(const RestTransaction& transaction) noexcept;
bool end_transaction(RestTransaction* transaction, RestTransactionPhase phase) noexcept;
u32 next_wait(const RestClientRuntime* runtime, u64 now_ms) noexcept;
void publish_request_error(RestClient* client, RestClientRuntime* runtime,
                           RestClientError error, u32 native_error) noexcept;
void begin_drain(RestClient* client, RestClientRuntime* runtime, RestClientError error,
                 u32 native_error, bool cancelled) noexcept;
NetworkWorkerStep finish_drain(RestClient* client, RestClientRuntime* runtime) noexcept;
TlsStreamReadAction response_plaintext_received(void* opaque, ConstSlice<u8> plaintext,
                                                bool peer_closed) noexcept;
TlsStreamEvent begin_resolution(RestClient* client, RestClientRuntime* runtime,
                                NetworkLoop* loop) noexcept;
bool reject_restricted_request(RestClient* client, RestClientRuntime* runtime) noexcept;
TlsStreamEvent begin_request_send(RestClient* client, RestClientRuntime* runtime) noexcept;
bool finalize_response(RestClient* client, RestClientRuntime* runtime,
                       const RestResponseStep& step, u64 now_ms) noexcept;
void destroy_client_storage(RestClient* client, SecureBuffer* restore) noexcept;
RestClientResult start_client(RestClient* client, SecureBuffer* credential,
                              RestAuthorization authorization,
                              const RestUserConfiguration* user_configuration,
                              u32 response_capacity, bool user_auth_mode,
                              RestSharedState* shared = nullptr,
                              u32 request_capacity = kUserAuthMaximumRequestBodyBytes) noexcept;

} // namespace rest_client_detail
} // namespace maicord

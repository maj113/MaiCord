#pragma once

#include "maicord/core/byte_queue.hpp"
#include "maicord/protocol/cookie_jar.hpp"
#include "maicord/protocol/http1.hpp"

namespace maicord {
struct GatewayUserProperties;
enum class JsonReadError : u8;
enum class JsonScanError : u8;

inline constexpr u32 kRestMaximumHeaderBytes = 32 * 1024;
inline constexpr u32 kRestMaximumHeaderFields = 128;
inline constexpr u32 kRateLimitBucketMaximumBytes = 64;
inline constexpr u32 kRestUserSuperPropertiesMaximumBytes = 2 * 1024;
inline constexpr u32 kRestUserLocaleMaximumBytes = 32;
inline constexpr u32 kRestUserTimezoneMaximumBytes = 64;
inline constexpr u32 kRestUserAcceptLanguageMaximumBytes = 256;
// Moderation reason recorded in the guild audit log. Bounded in UTF-8 bytes so
// the percent-encoded header always fits; the service limit is 512 characters.
inline constexpr u32 kRestAuditReasonMaximumBytes = 512;
inline constexpr u32 kRestAuditReasonMaximumEncodedBytes = kRestAuditReasonMaximumBytes * 3;
// Preformatted "Name: value\r\n" lines for per-request headers: CAPTCHA
// retries (X-Captcha-Key/-Rqtoken/-Session-Id), X-Context-Properties and
// X-Discord-Original-MD5.
inline constexpr u32 kRestExtraHeadersMaximumBytes = 9 * 1024;

enum class RestMethod : u8 {
    get,
    post,
    put,
    patch,
    delete_method,
    head,
};

enum class RestAuthorization : u8 {
    none,
    normal_user,
};

// Normal-user requests use the session token verbatim and attach client
// metadata headers. The timezone is optional because a native Windows zone name
// must not be mislabeled as an IANA zone; an empty Accept-Language is omitted.
// locale is the Discord interface locale (X-Discord-Locale).
struct RestUserRequestHeaders {
    ConstSlice<u8> super_properties;
    Utf8View locale;
    Utf8View timezone;
    Utf8View accept_language;
};
// True when super_properties is canonical padded base64 and every other field is graphic ASCII within its bound.
bool rest_user_headers_valid(const RestUserRequestHeaders& headers) noexcept;

struct RestRequestHead {
    ConstSlice<u8> target;
    ConstSlice<u8> token;
    u64 content_length;
    RestMethod method;
    RestAuthorization authorization;
    // False sends no Content-Type; a bodyless POST or PUT still carries
    // Content-Length: 0.
    bool json_body;
    const RestUserRequestHeaders* user_headers;
    bool accept_gzip;
    // Already percent-encoded (rest_encode_audit_reason); empty omits
    // the X-Audit-Log-Reason header unless send_empty_audit_reason is set.
    ConstSlice<u8> audit_reason;
    // Must satisfy rest_extra_headers_valid; appended verbatim.
    ConstSlice<u8> extra_headers;
    // Non-empty with a body selects multipart/form-data with this boundary
    // instead of application/json (rest_multipart_boundary_valid).
    ConstSlice<u8> multipart_boundary;
    // An empty reason still sends "X-Audit-Log-Reason: " with no value (a null
    // reason sends nothing). Never on GET.
    bool send_empty_audit_reason = false;
    // The Cookie header value (cookie_jar_header); empty sends none. Only on
    // requests with user_headers.
    ConstSlice<u8> cookie{};
};

// RFC 2046 boundary restricted to [A-Za-z0-9_-], 1..70 bytes.
inline constexpr u32 kRestMultipartBoundaryMaximumBytes = 70;
bool rest_multipart_boundary_valid(const ConstSlice<u8>& boundary) noexcept;

enum class RestRequestError : u8 {
    none,
    invalid_input,
    invalid_target,
    invalid_token,
    output_capacity,
};

struct RestRequestWriteResult {
    u32 bytes;
    RestRequestError error;
    // Where the extra headers end: further per-request X- headers go here.
    u32 extra_headers_end;

    constexpr bool ok() const noexcept {
        return error == RestRequestError::none;
    }
};

struct RestAuditReasonResult {
    u32 bytes;
    bool ok;
};

// Percent-encodes a UTF-8 reason exactly as encodeURIComponent does (letters,
// digits and -_.!~*'() pass through), for the X-Audit-Log-Reason header or a
// reason query parameter. Empty input yields zero bytes. Rejects invalid
// UTF-8, control characters and reasons over the byte bound; output is
// untouched on failure.
RestAuditReasonResult rest_encode_audit_reason(const Utf8View& reason, const Slice<u8>& output) noexcept;
// Accepts only output shaped like rest_encode_audit_reason's.
bool rest_audit_reason_valid(const ConstSlice<u8>& encoded) noexcept;
// Only the allowlisted names; values are visible ASCII without CR/LF.
bool rest_extra_headers_valid(const ConstSlice<u8>& headers) noexcept;
// Appends one allowlisted header line; returns the new length, or ~0u when it
// does not fit or the value is invalid. The output is unchanged on failure.
u32 rest_append_extra_header(Slice<u8> output, u32 used, Utf8View name, ConstSlice<u8> value) noexcept;

// Writes one complete Discord HTTP/1.1 request head for discord.com, in
// Chromium's field order. Requests with user_headers are the https://discord.com
// page's XMLHttpRequests: they also carry the client hints, fetch metadata,
// Origin (other than on GET and HEAD) and the page as Referer, the app's
// https://discord.com/channels/@me for a normal user and
// https://discord.com/login before sign-in. Capacity is checked before any
// byte (including a credential) is copied to output.
// The caller owns secure erasure of the plaintext output after TLS framing.
RestRequestWriteResult write_rest_request_head(const RestRequestHead& request,
                                               Slice<u8> output) noexcept;

enum class RestUserPropertiesError : u8 {
    none,
    invalid_input,
    invalid_properties,
    json_capacity,
    output_capacity,
};

struct RestUserPropertiesResult {
    u32 bytes;
    RestUserPropertiesError error;

    constexpr bool ok() const noexcept {
        return error == RestUserPropertiesError::none;
    }
};

// Writes base64(JSON(properties)) for X-Super-Properties. The property schema
// is shared with the normal-user Gateway Identify; callers provide scratch so
// this stays allocation-free.
RestUserPropertiesResult write_rest_user_super_properties(
    const GatewayUserProperties& properties, Slice<u8> json_scratch, Slice<u8> output) noexcept;

enum class RateLimitScope : u8 {
    none,
    user,
    global,
    shared,
};

enum RateLimitField : u32 {
    rate_limit_field_limit = 1u << 0,
    rate_limit_field_remaining = 1u << 1,
    rate_limit_field_reset = 1u << 2,
    rate_limit_field_reset_after = 1u << 3,
    rate_limit_field_bucket = 1u << 4,
    rate_limit_field_global = 1u << 5,
    rate_limit_field_scope = 1u << 6,
    rate_limit_field_retry_after = 1u << 7,
};

struct RateLimitHeaders {
    u64 reset_epoch_ms;
    u64 reset_after_ms;
    u64 retry_after_ms;
    u32 limit;
    u32 remaining;
    u32 fields;
    u8 bucket[kRateLimitBucketMaximumBytes];
    u8 bucket_bytes;
    RateLimitScope scope;
    bool global;
};

// Coarse response representation metadata. These values do not keep header text,
// so they are safe to include in diagnostics.
enum class RestContentType : u8 {
    missing,
    json,
    html,
    text,
    other,
};

enum class RestContentEncoding : u8 {
    missing,
    identity,
    gzip,
    deflate,
    brotli,
    zstd,
    other,
};

enum class RestBodyKind : u8 {
    empty,
    json_object,
    json_array,
    markup,
    utf8_text,
    gzip,
    zlib,
    zstd,
    binary,
};

struct RestRepresentation {
    RestContentType content_type;
    RestContentEncoding content_encoding;
    RestBodyKind body_kind;
};

enum class RestHeadError : u8 {
    none,
    invalid_input,
    http,
    duplicate_rate_field,
    invalid_rate_value,
    bucket_capacity,
    inconsistent_rate_fields,
};

struct RestHeadResult {
    HttpResponseHead head;
    RateLimitHeaders rate;
    u32 error_offset;
    HttpError http_error;
    RestHeadError error;
    HttpParseState state;
    RestRepresentation representation;

    constexpr bool ok() const noexcept {
        return state == HttpParseState::complete && error == RestHeadError::none;
    }
};

// Parses one complete/partial response head and captures Discord's optional
// rate-limit fields. Decimal second values are rounded upward to milliseconds
// so a deadline is never scheduled early. A 429 may supply its retry deadline
// here or in the JSON body; callers must require one before scheduling retry.
RestHeadResult parse_rest_response_head(
    ConstSlice<u8> input, u32 maximum_header_bytes = kRestMaximumHeaderBytes,
    u32 maximum_fields = kRestMaximumHeaderFields) noexcept;

// Inspects only fixed leading-byte classes. It does not parse, copy, or retain
// the body and is suitable for secret-safe failure diagnostics.
RestBodyKind rest_classify_body(ConstSlice<u8> body) noexcept;

enum class RateLimitBodyError : u8 {
    none,
    invalid_input,
    scan_failed,
    invalid_json,
    duplicate_field,
    invalid_retry_after,
    invalid_global,
    missing_required_field,
    invalid_code,
};

struct RateLimitBodyResult {
    u64 retry_after_ms;
    u32 error_offset;
    RateLimitBodyError error;
    JsonScanError scan_error;
    JsonReadError read_error;
    bool global;

    constexpr bool ok() const noexcept {
        return error == RateLimitBodyError::none;
    }
};

RateLimitBodyResult decode_rate_limit_body(Utf8View body, Slice<u32> structural_scratch,
                                           u32 maximum_depth) noexcept;

// Reads search indexing retry_after seconds, rounded up to milliseconds. An
// omitted retry_after requires code 110000; a supplied code must also be 110000.
// Valid missing/zero delays permit a short retry. Global never establishes a
// 429 rate-limit scope through this decoder.
RateLimitBodyResult decode_search_indexing_body(Utf8View body, Slice<u32> structural_scratch,
                                            u32 maximum_depth) noexcept;

enum class RestMajorParameter : u8 {
    none,
    channel,
    guild,
    webhook,
};

// route_id names a normalized endpoint template owned by the caller. major_key
// is the channel/guild ID or a stable webhook-ID+token hash. Discord's bucket
// hash excludes this top-level resource, so both are kept in the bucket key.
struct RestRouteKey {
    u64 route_id;
    u64 major_key;
    RestMethod method;
    RestMajorParameter major;
};
// A nonzero route id with a known method whose major key matches its major parameter.
bool rest_route_valid(const RestRouteKey& route) noexcept;
// An origin-form request target: one leading '/', visible ASCII only, no '#'.
bool rest_target_valid(ConstSlice<u8> target) noexcept;

struct RestRateBucket {
    u64 route_id;
    u64 major_key;
    u64 reset_at_ms;
    u64 last_used_ms;
    u32 limit;
    u32 remaining;
    u32 fields;
    u32 in_flight;
    u16 generation;
    u8 hash[kRateLimitBucketMaximumBytes];
    u8 hash_bytes;
    RestMethod method;
    RestMajorParameter major;
    bool used;
    bool canonical;
};

struct RestRateBinding {
    RestRouteKey route;
    u64 last_used_ms;
    u16 bucket_index;
    u16 bucket_generation;
    u16 generation;
    bool used;
    bool in_flight;
};

struct RestRateScheduler {
    Slice<RestRateBucket> buckets;
    Slice<RestRateBinding> bindings;
    u64 global_reset_at_ms;
    u32 acquired;
    u32 waited;
    u32 busy;
    u32 observations;
    u32 migrations;
    u32 cancellations;
    u32 invalid_requests;
};

struct RestRateLease {
    u16 binding_index;
    u16 binding_generation;
    u16 bucket_index;
    u16 bucket_generation;
    bool active;
};

enum class RestRateAcquireKind : u8 {
    acquired,
    waiting,
    busy,
    error,
};

enum class RestRateError : u8 {
    none,
    invalid_input,
    capacity,
    stale_lease,
    invalid_observation,
    missing_retry_deadline,
    incomplete_bucket_deadline,
};

struct RestRateAcquireResult {
    RestRateLease lease;
    u64 wait_until_ms;
    RestRateAcquireKind kind;
    RestRateError error;

    constexpr bool acquired_request() const noexcept {
        return kind == RestRateAcquireKind::acquired && error == RestRateError::none;
    }
};

struct RestRateObservation {
    // Absolute monotonic deadline from Retry-After, or a 429 header/body maximum.
    // Zero when absent. Metadata alone does not make a response retryable.
    u64 retry_at_ms;
    RestRateError error;
    bool rate_limited;
    bool global;

    constexpr bool ok() const noexcept {
        return error == RestRateError::none;
    }
};

bool rest_rate_scheduler_initialize(RestRateScheduler* scheduler,
                                    Slice<RestRateBucket> buckets,
                                    Slice<RestRateBinding> bindings) noexcept;
RestRateAcquireResult rest_rate_acquire(RestRateScheduler* scheduler,
                                        const RestRouteKey& route,
                                        u64 now_ms) noexcept;
RestRateObservation rest_rate_observe(RestRateScheduler* scheduler,
                                      RestRateLease* lease,
                                      const RestHeadResult& response_head,
                                      const RateLimitBodyResult* rate_limit_body,
                                      u64 now_ms, u64 unix_time_ms) noexcept;
bool rest_rate_cancel(RestRateScheduler* scheduler, RestRateLease* lease) noexcept;

using RestResponseState = HttpResponseDecoderState;
using RestResponseError = HttpResponseDecoderError;

struct RestResponse {
    HttpResponseDecoder http;
    RestHeadResult parsed_head;
    RestMethod request_method;
    // Optional: each complete head's Set-Cookie fields are stored here for
    // cookie_request (set after rest_response_initialize).
    CookieJar* cookies;
    CookieRequest cookie_request;
};

using RestResponseStep = HttpResponseDecoderStep;

bool rest_response_initialize(RestResponse* response, Slice<u8> input_storage,
                              Slice<u8> body_output,
                              RestMethod request_method = RestMethod::get) noexcept;

// Feeds decrypted HTTP bytes. One-byte fragmentation, interim 1xx heads,
// content-length and chunked bodies are supported. until-close bodies complete
// only when peer_closed is true and make the connection non-reusable.
RestResponseStep rest_response_feed(RestResponse* response, ConstSlice<u8> input,
                                    bool peer_closed = false) noexcept;

} // namespace maicord

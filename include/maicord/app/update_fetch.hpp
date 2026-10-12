#pragma once

#include "maicord/app/update_manifest.hpp"

namespace maicord {

// Release coordinates shared with tools/release: version numbers, update
// locations, redirect allowlist, rate limits and check scheduling. The
// network is reached only through an UpdateTransport, so tests use a fake.
inline constexpr u32 kUpdateRepositoryMaximumBytes = 100;
inline constexpr u32 kUpdateOwnerMaximumBytes = 39; // a GitHub login
inline constexpr u32 kUpdateHostNameCapacity = 64;  // "<owner>.github.io" and the fixed hosts
inline constexpr u32 kUpdateTargetCapacity = 4096;
inline constexpr u32 kUpdateFetchMaximumRedirects = 5;
inline constexpr u64 kUpdateDefaultArtifactCap = 64ull * 1024 * 1024;
// Releases sign 90-day manifests (7,776,000 s); keep slack for re-signing.
inline constexpr u64 kUpdateMaximumValiditySeconds = 100ull * 86400;
inline constexpr u64 kUpdateCheckIntervalSeconds = 86400;
inline constexpr u64 kUpdateBackoffBaseSeconds = 300;
inline constexpr u32 kUpdateStableSequence = 999;

// ((major*1000 + minor)*1000 + patch)*1000 + n, n = preview number or 999.
constexpr u64 update_version_number(u32 major, u32 minor, u32 patch, u32 preview) noexcept {
    return ((static_cast<u64>(major) * 1000 + minor) * 1000 + patch) * 1000 + (preview ? preview : kUpdateStableSequence);
}
constexpr bool update_version_components_valid(u32 major, u32 minor, u32 patch, u32 preview) noexcept {
    return major < 1000 && minor < 1000 && patch < 1000 && preview < kUpdateStableSequence;
}
constexpr UpdateChannel update_version_channel(u64 version) noexcept {
    return version % 1000 == kUpdateStableSequence ? UpdateChannel::stable : UpdateChannel::preview;
}
// "1.2.3" or "1.2.3-preview.4"; returns bytes written, 0 when it does not fit.
u32 update_version_text(u64 version, Slice<char> output) noexcept;

// GitHub "owner/repo". The owner is a GitHub login (1-39 ASCII letters,
// digits and inner '-'), which is also the DNS label of its Pages host; the
// name uses letters, digits, '-', '_' and '.', and is not "." or "..".
bool update_repository_valid(Utf8View repository) noexcept;
// 130 hex digits of an uncompressed SEC1 point (04 || X || Y).
bool update_public_key_from_hex(Utf8View hex, Slice<u8> output) noexcept;

enum class UpdateHost : u8 { none, github, objects, release_assets, pages };
// The fixed host names; pages is per repository and travels in UpdateLocation.
Utf8View update_host_name(UpdateHost) noexcept;

// An origin and origin-relative target. pages holds "<owner>.github.io" in
// lowercase whenever host is pages.
struct UpdateLocation {
    UpdateHost host;
    u32 pages_bytes, target_bytes;
    char pages[kUpdateHostNameCapacity];
    char target[kUpdateTargetCapacity];
};
Utf8View update_location_host(const UpdateLocation&) noexcept;

enum class UpdateAsset : u8 { manifest, signature, package };
// Channel manifests and their signatures come from the repository's GitHub
// Pages site, outside any release:
//   https://<owner>.github.io/<repo>/updates/maicord-<channel>.manifest.json[.sig]
// Packages come from the immutable release of their version, whose tag is
// derived from the signed version number:
//   https://github.com/<owner>/<repo>/releases/download/v<version text>/MaiCord-x64-<version>.zip
// A package version must map to a release tag (n = 1..999, major < 1000).
bool update_asset_location(Utf8View repository, UpdateChannel, UpdateAsset, u64 version, UpdateLocation*) noexcept;
// Location header: absolute https on an allowed host, or origin-relative on
// the current host. github.com and the release-asset hosts redirect among
// themselves; the Pages host only to itself. Userinfo, ports, fragments and
// control bytes are refused. next may be current; it is unchanged on failure.
bool update_redirect_target(const UpdateLocation& current, ConstSlice<u8> location, UpdateLocation* next) noexcept;

struct UpdateHttpResponse {
    u64 content_length;
    u64 body_bytes;
    u64 retry_after_seconds;
    u64 ratelimit_reset_unix;
    u32 location_bytes;
    u32 native_error; // transport failures: Winsock, resolver or SECURITY_STATUS code
    u16 status;
    bool has_content_length, has_retry_after, has_ratelimit_reset, ratelimit_exhausted;
    char location[kUpdateTargetCapacity + 128];
};
enum class UpdateTransportError : u8 {
    none, invalid_input, resolve, connect, tls, send, receive, http, oversize, sink, cancelled,
};
using UpdateBodySink = bool (*)(void* context, ConstSlice<u8> bytes) noexcept;
struct UpdateHttpRequest {
    UpdateHost host;
    Utf8View host_name;
    ConstSlice<char> target;
    u64 maximum_body_bytes;
    UpdateBodySink sink;
    void* sink_context;
};
// One GET per call. Records the status and observed fields for every
// response; streams only 2xx bodies, refusing announced or received bytes
// above maximum_body_bytes with `oversize`.
struct UpdateTransport {
    void* context;
    UpdateTransportError (*get)(void* context, const UpdateHttpRequest&, UpdateHttpResponse*) noexcept;
};
// Shared field observer for transports: Location, Retry-After (seconds),
// x-ratelimit-remaining and x-ratelimit-reset.
void update_http_observe_field(UpdateHttpResponse*, ConstSlice<u8> name, ConstSlice<u8> value) noexcept;

enum class UpdateFetchError : u8 {
    none, invalid_input, transport, redirect, not_found, rate_limited, status, oversize, size,
};
struct UpdateFetchResult {
    UpdateFetchError error;
    UpdateTransportError transport;
    u16 status;
    u32 redirects;
    u32 native_error;
    u64 bytes;
    u64 retry_at_unix;
    constexpr bool ok() const noexcept { return error == UpdateFetchError::none; }
};
// Starts at `start` and follows at most five allowlisted redirects, logging
// each response (host, status, sizes; never targets). A nonzero exact_bytes
// requires that body size. 429, or 403/503 with rate-limit fields, report the
// earliest permitted retry.
UpdateFetchResult update_fetch(const UpdateTransport&, const UpdateLocation& start, u64 maximum_bytes,
    u64 exact_bytes, UpdateBodySink, void* sink_context, u64 now_unix) noexcept;

// Next automatic check: 24 h after success with +-1 h jitter; otherwise
// exponential backoff from five minutes up to 24 h with up to 25% jitter,
// never before a server-requested retry time.
u64 update_next_check(u64 now_unix, u32 consecutive_failures, u64 retry_at_unix, u32 random) noexcept;

} // namespace maicord

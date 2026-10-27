#pragma once

#include "maicord/app/update_manifest.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

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
// The longest wait between checks, and the cap on server-requested retries.
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
// Stable releases end in 999; every other sequence (1-998) is a preview:
// the master build that many commits after the last stable release.
constexpr UpdateChannel update_version_channel(u64 version) noexcept {
    return version % 1000 == kUpdateStableSequence ? UpdateChannel::stable : UpdateChannel::preview;
}
// "1.2.3" or "1.2.3-preview.4" (plugin index versions); returns bytes
// written, 0 when it does not fit.
u32 update_version_text(u64 version, Slice<char> output) noexcept;
// An application build: "1.2.3", or "1.2.4-dev.5" for a master build,
// followed by "+<commit>" when commit is not empty (at most 12 characters).
u32 update_build_version_text(u64 version, Utf8View commit, Slice<char> output) noexcept;
// Bounded appender for update and catalog locations: a write past the end
// clears ok and leaves the output alone.
struct UpdateTextWriter {
    Slice<char> out;
    u32 used;
    bool ok;
    void add(const char* text, u32 bytes) noexcept {
        if (!ok || bytes > out.count - used) { ok = false; return; }
        copy_bytes(out.data + used, text, bytes); used += bytes;
    }
    void add(Utf8View text) noexcept { add(text.data, text.bytes); }
    void number(u64 value) noexcept { char digits[20]; add(digits, decimal_write(digits, value)); }
};

// GitHub "owner/repo". The owner is a GitHub login (1-39 ASCII letters,
// digits and inner '-'), which is also the DNS label of its Pages host; the
// name uses letters, digits, '-', '_' and '.', and is not "." or "..".
bool update_repository_valid(Utf8View repository) noexcept;
// 130 hex digits of an uncompressed SEC1 point (04 || X || Y).
bool update_public_key_from_hex(Utf8View hex, Slice<u8> output) noexcept;

// pages is a repository's "<owner>.github.io"; provider is any other
// validated host (a plugin catalog provider). Both are closed origins.
enum class UpdateHost : u8 { none, github, objects, release_assets, pages, provider };
// The fixed host names; pages and provider hosts travel in UpdateLocation.
Utf8View update_host_name(UpdateHost) noexcept;

// An origin and origin-relative target. origin holds the lowercase host name
// whenever host is pages or provider.
struct UpdateLocation {
    UpdateHost host;
    u32 origin_bytes, target_bytes;
    char origin[kUpdateHostNameCapacity];
    char target[kUpdateTargetCapacity];
};
Utf8View update_location_host(const UpdateLocation&) noexcept;
// A DNS name of at most 64 bytes: two or more dot-separated labels of 1-63
// ASCII letters, digits and inner '-'. The last label is not all digits (no
// IPv4 literal) and not "localhost".
bool update_origin_host_valid(Utf8View host) noexcept;
// A provider-origin location: host as update_origin_host_valid (stored in
// lowercase) and an origin-relative target starting with '/' made of RFC 3986
// path and query characters. out is unchanged on failure.
bool update_provider_location(Utf8View host, ConstSlice<char> target, UpdateLocation* out) noexcept;

enum class UpdateAsset : u8 { manifest, signature, package, changelog, changelog_signature };
// Channel manifests, changelogs and their signatures come from the
// repository's GitHub Pages site, outside any release:
//   https://<owner>.github.io/<repo>/updates/p<protocol>/maicord-stable.{manifest,changelog}.json[.sig]
//   https://<owner>.github.io/<repo>/updates/p<protocol>/preview/maicord-preview.{manifest,changelog}.json[.sig]
// A stable package comes from the immutable release of its version, whose
// tag is derived from the signed version number; a preview package sits
// beside the preview manifest:
//   https://github.com/<owner>/<repo>/releases/download/v<X.Y.Z>/MaiCord-x64-p<protocol>-<version>.zip
//   https://<owner>.github.io/<repo>/updates/p<protocol>/preview/MaiCord-x64-p<protocol>-<version>.zip
// A package version needs a sequence of 1..999 and major < 1000.
bool update_asset_location(Utf8View repository, UpdateChannel, UpdateAsset, u64 version, UpdateLocation*) noexcept;
// Location header: absolute https on an allowed host, or origin-relative on
// the current host. github.com and the release-asset hosts redirect among
// themselves; a Pages or provider origin only to itself. Userinfo, ports,
// fragments and control bytes are refused. next may be current; it is
// unchanged on failure.
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
// above maximum_body_bytes with `oversize`. update_fetch logs its responses
// under log_category ("update" when null).
struct UpdateTransport {
    void* context;
    UpdateTransportError (*get)(void* context, const UpdateHttpRequest&, UpdateHttpResponse*) noexcept;
    const char* log_category;
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
// Body sink into a fixed buffer; refuses bytes beyond capacity.
struct UpdateBufferSink {
    u8* data;
    u32 capacity, used;
};
bool update_buffer_sink(void* context, ConstSlice<u8> bytes) noexcept;

// Starts at `start` and follows at most five allowlisted redirects, logging
// each response (host, status, sizes; never targets). A nonzero exact_bytes
// requires that body size. 429, or 403/503 with rate-limit fields, report the
// earliest permitted retry.
UpdateFetchResult update_fetch(const UpdateTransport&, const UpdateLocation& start, u64 maximum_bytes,
    u64 exact_bytes, UpdateBodySink, void* sink_context, u64 now_unix) noexcept;

// How often MaiCord checks in the background.
enum class UpdateCheckInterval : u8 { hour, four_hours, twelve_hours, day };
inline constexpr u32 kUpdateCheckIntervalCount = 4;
// Preview builds land several times a day.
inline constexpr UpdateCheckInterval kUpdateDefaultCheckInterval = UpdateCheckInterval::four_hours;
constexpr u64 update_check_interval_seconds(UpdateCheckInterval interval) noexcept {
    switch (interval) {
    case UpdateCheckInterval::hour: return 3600;
    case UpdateCheckInterval::twelve_hours: return 12 * 3600;
    case UpdateCheckInterval::day: return 86400;
    default: return 4 * 3600;
    }
}

// Next automatic check: interval_seconds after success with jitter of 10%
// (at most 1 h either way); otherwise exponential backoff from five minutes
// up to interval_seconds with up to 25% jitter, never before a
// server-requested retry time.
u64 update_next_check(u64 now_unix, u32 consecutive_failures, u64 retry_at_unix, u32 random,
    u64 interval_seconds = kUpdateCheckIntervalSeconds) noexcept;

// The first check after launch runs 5 to 15 s after the first frame is
// presented.
inline constexpr u64 kUpdateFirstCheckDelaySeconds = 5, kUpdateFirstCheckJitterSeconds = 10;
constexpr u64 update_first_check(u64 now_unix, u32 random) noexcept {
    return now_unix + kUpdateFirstCheckDelaySeconds + random % (kUpdateFirstCheckJitterSeconds + 1);
}
// The updater's deadline. Changing the interval moves it to the last check
// plus the new interval (backoff and retry time kept), which may be past.
struct UpdateSchedule {
    u64 next_unix, checked_unix, retry_at_unix;
    u32 failures;
};
void update_schedule_finish(UpdateSchedule*, u64 now_unix, u32 consecutive_failures, u64 retry_at_unix, u32 random,
    u64 interval_seconds) noexcept;
void update_schedule_retarget(UpdateSchedule*, u64 interval_seconds, u32 random) noexcept;
constexpr bool update_schedule_due(const UpdateSchedule& schedule, u64 now_unix) noexcept {
    return now_unix >= schedule.next_unix;
}

} // namespace maicord

#include "maicord/app/update_fetch.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {
struct Writer {
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
Utf8View as_text(ConstSlice<u8> bytes) noexcept { return {reinterpret_cast<const char*>(bytes.data), bytes.count}; }
bool equal_case(ConstSlice<u8> a, Utf8View b) noexcept { return ascii_equal_ignore_case(as_text(a), b); }
bool decimal(ConstSlice<u8> text, u64* value) noexcept {
    u32 at = 0, end = text.count;
    while (at < end && (text[at] == ' ' || text[at] == '\t')) ++at;
    while (end > at && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return end - at <= 19 && decimal_parse({as_text(text).data + at, end - at}, value);
}
bool alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool target_byte(u8 c) noexcept {
    // RFC 3986 path/query characters; percent-encoding passes through as-is.
    if (alnum(static_cast<char>(c))) return true;
    constexpr char allowed[] = "-._~!$&'()*+,;=:@/?%";
    for (char a : allowed) if (a && c == static_cast<u8>(a)) return true;
    return false;
}
constexpr Utf8View host_names[]{{}, utf8_literal("github.com"), utf8_literal("objects.githubusercontent.com"),
    utf8_literal("release-assets.githubusercontent.com")};
constexpr u32 kFixedHosts = 4;
constexpr Utf8View kPagesSuffix = utf8_literal(".github.io");
// Pages and provider hosts are per location and closed: they redirect only to themselves.
bool closed_origin(UpdateHost host) noexcept { return host == UpdateHost::pages || host == UpdateHost::provider; }
bool location_valid(const UpdateLocation& l) noexcept {
    return l.host != UpdateHost::none && l.host <= UpdateHost::provider && l.target_bytes && l.target_bytes <= kUpdateTargetCapacity &&
        l.target[0] == '/' && (!closed_origin(l.host) || (l.origin_bytes && l.origin_bytes <= kUpdateHostNameCapacity));
}
// Host part of a Location value for diagnostics; relative targets stay on `current`.
Utf8View location_host(ConstSlice<u8> location, Utf8View current) noexcept {
    const Utf8View text = as_text(location);
    u32 at = 0;
    if (text.bytes > 8 && ascii_equal_ignore_case({text.data, 8}, utf8_literal("https://"))) at = 8;
    else if (text.bytes > 7 && ascii_equal_ignore_case({text.data, 7}, utf8_literal("http://"))) at = 7;
    else return current;
    u32 end = at;
    while (end < text.bytes && text.data[end] != '/' && text.data[end] != '?' && text.data[end] != '#') ++end;
    return {text.data + at, end - at};
}
} // namespace

u32 update_version_text(u64 version, Slice<char> output) noexcept {
    if (!output.data) return 0;
    Writer w{output, 0, true};
    const u64 n = version % 1000, base = version / 1000;
    w.number(base / 1000000); w.add(".", 1); w.number(base / 1000 % 1000); w.add(".", 1); w.number(base % 1000);
    if (n != kUpdateStableSequence) { w.add(utf8_literal("-preview.")); w.number(n); }
    return w.ok ? w.used : 0;
}

u32 update_build_version_text(u64 version, Utf8View commit, Slice<char> output) noexcept {
    if (!output.data || commit.bytes > 12) return 0;
    Writer w{output, 0, true};
    const u64 n = version % 1000, base = version / 1000;
    w.number(base / 1000000); w.add(".", 1); w.number(base / 1000 % 1000); w.add(".", 1); w.number(base % 1000);
    if (n != kUpdateStableSequence) { w.add(utf8_literal("-dev.")); w.number(n); }
    if (commit.bytes) { w.add("+", 1); w.add(commit); }
    return w.ok ? w.used : 0;
}

bool update_repository_valid(Utf8View repository) noexcept {
    if (!repository.data || !repository.bytes || repository.bytes > kUpdateRepositoryMaximumBytes) return false;
    const char* p = repository.data;
    u32 slash = 0;
    while (slash < repository.bytes && p[slash] != '/') ++slash;
    // The owner doubles as the DNS label of "<owner>.github.io".
    if (!slash || slash > kUpdateOwnerMaximumBytes || slash == repository.bytes || p[0] == '-' || p[slash - 1] == '-') return false;
    for (u32 i = 0; i < slash; ++i) if (!alnum(p[i]) && p[i] != '-') return false;
    const char* name = p + slash + 1;
    const u32 n = repository.bytes - slash - 1;
    // "." and ".." would change the request path; a second '/' is refused below.
    if (!n || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) return false;
    for (u32 i = 0; i < n; ++i) if (!alnum(name[i]) && name[i] != '-' && name[i] != '_' && name[i] != '.') return false;
    return true;
}

bool update_public_key_from_hex(Utf8View hex, Slice<u8> output) noexcept {
    if (!hex.data || hex.bytes != 130 || !output.data || output.count < 65) return false;
    u8 key[65]{};
    for (u32 i = 0; i < 65; ++i) {
        i32 v[2]{};
        for (u32 j = 0; j < 2; ++j) {
            v[j] = ascii_hex_value(hex.data[i * 2 + j]);
            if (v[j] < 0) return false;
        }
        key[i] = static_cast<u8>(v[0] << 4 | v[1]);
    }
    if (key[0] != 4) return false;
    copy_bytes(output.data, key, 65); return true;
}

Utf8View update_host_name(UpdateHost host) noexcept {
    return static_cast<u32>(host) < kFixedHosts ? host_names[static_cast<u32>(host)] : Utf8View{};
}
Utf8View update_location_host(const UpdateLocation& l) noexcept {
    return closed_origin(l.host) ? Utf8View{l.origin, minimum(l.origin_bytes, kUpdateHostNameCapacity)} : update_host_name(l.host);
}

bool update_origin_host_valid(Utf8View host) noexcept {
    if (!host.data || !host.bytes || host.bytes > kUpdateHostNameCapacity) return false;
    u32 labels = 0, start = 0;
    bool digits = true;
    for (u32 i = 0; i <= host.bytes; ++i) {
        if (i == host.bytes || host.data[i] == '.') {
            const u32 n = i - start;
            if (!n || n > 63 || host.data[start] == '-' || host.data[i - 1] == '-') return false;
            ++labels;
            if (i == host.bytes && (digits || ascii_equal_ignore_case({host.data + start, n}, utf8_literal("localhost")))) return false;
            start = i + 1; digits = true;
        } else {
            const char c = host.data[i];
            if (!alnum(c) && c != '-') return false;
            digits &= c >= '0' && c <= '9';
        }
    }
    return labels >= 2;
}

bool update_provider_location(Utf8View host, ConstSlice<char> target, UpdateLocation* out) noexcept {
    if (!out || !update_origin_host_valid(host) || !target.data || !target.count || target.count > kUpdateTargetCapacity ||
        target[0] != '/' || (target.count > 1 && target[1] == '/')) return false;
    for (u32 i = 0; i < target.count; ++i) if (!target_byte(static_cast<u8>(target[i]))) return false;
    out->host = UpdateHost::provider;
    for (u32 i = 0; i < host.bytes; ++i) out->origin[i] = ascii_lower(host.data[i]);
    out->origin_bytes = host.bytes;
    copy_bytes(out->target, target.data, target.count);
    out->target_bytes = target.count;
    return true;
}

bool update_asset_location(Utf8View repository, UpdateChannel channel, UpdateAsset asset, u64 version, UpdateLocation* out) noexcept {
    if (!out || !update_repository_valid(repository) || channel > UpdateChannel::preview ||
        asset > UpdateAsset::changelog_signature) return false;
    u32 slash = 0;
    while (repository.data[slash] != '/') ++slash;
    Writer w{{out->target, kUpdateTargetCapacity}, 0, true};
    out->origin_bytes = 0;
    const bool stable_package = update_version_channel(version) == UpdateChannel::stable;
    if (asset == UpdateAsset::package && (!(version % 1000) || version / 1000000000 >= 1000)) return false;
    if (asset == UpdateAsset::package && stable_package) {
        char tag[32];
        const u32 tag_bytes = update_version_text(version, {tag, sizeof(tag)});
        if (!tag_bytes) return false;
        out->host = UpdateHost::github;
        w.add("/", 1); w.add(repository); w.add(utf8_literal("/releases/download/v")); w.add(tag, tag_bytes);
        w.add("/", 1); w.add(kUpdateArtifactPrefix); w.number(version); w.add(utf8_literal(".zip"));
    } else {
        out->host = UpdateHost::pages;
        for (u32 i = 0; i < slash; ++i) out->origin[i] = ascii_lower(repository.data[i]);
        copy_bytes(out->origin + slash, kPagesSuffix.data, kPagesSuffix.bytes);
        out->origin_bytes = slash + kPagesSuffix.bytes;
        w.add("/", 1); w.add({repository.data + slash + 1, repository.bytes - slash - 1});
        w.add(kUpdateProtocolDirectory);
        if (asset == UpdateAsset::package) {
            w.add(utf8_literal("preview/")); w.add(kUpdateArtifactPrefix); w.number(version); w.add(utf8_literal(".zip"));
        } else {
            w.add(channel == UpdateChannel::stable ? utf8_literal("maicord-stable.") : utf8_literal("preview/maicord-preview."));
            const bool changelog = asset == UpdateAsset::changelog || asset == UpdateAsset::changelog_signature;
            w.add(changelog ? utf8_literal("changelog.json") : utf8_literal("manifest.json"));
            if (asset == UpdateAsset::signature || asset == UpdateAsset::changelog_signature) w.add(utf8_literal(".sig"));
        }
    }
    out->target_bytes = w.ok ? w.used : 0;
    return w.ok;
}

bool update_redirect_target(const UpdateLocation& current, ConstSlice<u8> location, UpdateLocation* next) noexcept {
    if (!next || !location_valid(current) || !location.data || !location.count) return false;
    UpdateHost host = current.host;
    u32 at = 0;
    if (location[0] != '/') {
        constexpr Utf8View scheme = utf8_literal("https://");
        if (location.count <= scheme.bytes || !equal_case({location.data, scheme.bytes}, scheme)) return false;
        u32 slash = scheme.bytes;
        while (slash < location.count && location[slash] != '/') ++slash;
        if (slash == location.count) return false;
        const ConstSlice<u8> authority{location.data + scheme.bytes, slash - scheme.bytes};
        host = UpdateHost::none;
        // Pages and provider origins are closed: they never hand off to
        // another host, and the release hosts never hand off to them.
        if (closed_origin(current.host)) {
            if (equal_case(authority, {current.origin, current.origin_bytes})) host = current.host;
        } else {
            for (u32 i = 1; i < kFixedHosts; ++i) if (equal_case(authority, host_names[i])) host = static_cast<UpdateHost>(i);
        }
        if (host == UpdateHost::none) return false;
        at = slash;
    } else if (location.count > 1 && location[1] == '/') return false; // network-path reference
    const u32 bytes = location.count - at;
    if (!bytes || bytes > kUpdateTargetCapacity) return false;
    for (u32 i = at; i < location.count; ++i) if (!target_byte(location[i])) return false;
    if (next != &current) { next->origin_bytes = current.origin_bytes; copy_bytes(next->origin, current.origin, sizeof(next->origin)); }
    copy_bytes(next->target, location.data + at, bytes);
    next->target_bytes = bytes; next->host = host;
    return true;
}

void update_http_observe_field(UpdateHttpResponse* r, ConstSlice<u8> name, ConstSlice<u8> value) noexcept {
    if (!r) return;
    u64 n = 0;
    if (equal_case(name, utf8_literal("location"))) {
        if (value.count <= sizeof(r->location)) { copy_bytes(r->location, value.data, value.count); r->location_bytes = value.count; }
        else r->location_bytes = 0;
    } else if (equal_case(name, utf8_literal("retry-after"))) {
        if (decimal(value, &n)) { r->retry_after_seconds = n; r->has_retry_after = true; }
    } else if (equal_case(name, utf8_literal("x-ratelimit-reset"))) {
        if (decimal(value, &n)) { r->ratelimit_reset_unix = n; r->has_ratelimit_reset = true; }
    } else if (equal_case(name, utf8_literal("x-ratelimit-remaining"))) {
        r->ratelimit_exhausted = decimal(value, &n) && n == 0;
    }
}

bool update_buffer_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* b = static_cast<UpdateBufferSink*>(context);
    if (bytes.count > b->capacity - b->used) return false;
    copy_bytes(b->data + b->used, bytes.data, bytes.count); b->used += bytes.count; return true;
}

UpdateFetchResult update_fetch(const UpdateTransport& transport, const UpdateLocation& start, u64 maximum_bytes,
    u64 exact_bytes, UpdateBodySink sink, void* sink_context, u64 now) noexcept {
    UpdateFetchResult result{};
    if (!transport.get || !location_valid(start) || !maximum_bytes || exact_bytes > maximum_bytes || !sink) {
        result.error = UpdateFetchError::invalid_input; return result;
    }
    UpdateLocation at = start;
    for (;;) {
        UpdateHttpResponse response{};
        const UpdateHttpRequest request{at.host, update_location_host(at), {at.target, at.target_bytes}, maximum_bytes, sink, sink_context};
        result.transport = transport.get(transport.context, request, &response);
        result.status = response.status; result.native_error = response.native_error; result.bytes = response.body_bytes;
        // One line per response: host, status and sizes. Targets carry signed
        // query strings and provider paths, so they are never logged.
        UpdateLog line(transport.log_category, "http");
        line.host("host", request.host_name);
        if (result.transport != UpdateTransportError::none) {
            line.field("transport", static_cast<u8>(result.transport)).field("native", response.native_error)
                .field("status", response.status).field("bytes", response.body_bytes);
            if (response.has_content_length) line.field("length", response.content_length);
            line.write(LogLevel::warning);
            result.error = result.transport == UpdateTransportError::oversize ? UpdateFetchError::oversize : UpdateFetchError::transport;
            return result;
        }
        const u16 s = response.status;
        line.field("status", s);
        if (s == 301 || s == 302 || s == 303 || s == 307 || s == 308) {
            const ConstSlice<u8> location{reinterpret_cast<const u8*>(response.location), response.location_bytes};
            line.host("next", location_host(location, request.host_name));
            if (result.redirects == kUpdateFetchMaximumRedirects || !response.location_bytes || !update_redirect_target(at, location, &at)) {
                line.field("refused", 1).write(LogLevel::warning);
                result.error = UpdateFetchError::redirect; return result;
            }
            line.write();
            ++result.redirects; continue;
        }
        if (s >= 200 && s < 300) {
            if (response.has_content_length) line.field("length", response.content_length);
            line.field("bytes", response.body_bytes);
            if (exact_bytes) line.field("expected", exact_bytes);
            if (s != 200) result.error = UpdateFetchError::status;
            else if (response.body_bytes > maximum_bytes) result.error = UpdateFetchError::oversize;
            else if ((exact_bytes && response.body_bytes != exact_bytes) || !response.body_bytes) result.error = UpdateFetchError::size;
            line.write(result.ok() ? LogLevel::info : LogLevel::warning);
            return result;
        }
        if (s == 404) { line.write(LogLevel::warning); result.error = UpdateFetchError::not_found; return result; }
        const bool limited = s == 429 || ((s == 403 || s == 503) &&
            (response.has_retry_after || (response.ratelimit_exhausted && response.has_ratelimit_reset)));
        if (limited) {
            result.error = UpdateFetchError::rate_limited;
            if (response.has_retry_after) result.retry_at_unix = now + minimum<u64>(response.retry_after_seconds, kUpdateCheckIntervalSeconds);
            else if (response.has_ratelimit_reset && response.ratelimit_reset_unix > now)
                result.retry_at_unix = minimum<u64>(response.ratelimit_reset_unix, now + kUpdateCheckIntervalSeconds);
            line.field("retry_at", result.retry_at_unix).write(LogLevel::warning);
            return result;
        }
        line.write(LogLevel::warning);
        result.error = UpdateFetchError::status; return result;
    }
}

u64 update_next_check(u64 now, u32 failures, u64 retry_at, u32 random, u64 interval) noexcept {
    if (!interval) interval = kUpdateCheckIntervalSeconds;
    const u64 jitter = minimum<u64>(interval / 10, 3600);
    u64 delay;
    if (!failures) delay = interval - jitter + (jitter ? random % (2 * jitter) : 0);
    else {
        delay = failures > 20 ? interval : minimum<u64>(interval, kUpdateBackoffBaseSeconds << (failures - 1));
        delay += random % (delay / 4 + 1);
        delay = minimum<u64>(delay, interval);
    }
    const u64 next = now + delay;
    return retry_at > next ? retry_at : next;
}

void update_schedule_finish(UpdateSchedule* s, u64 now, u32 failures, u64 retry_at, u32 random, u64 interval) noexcept {
    if (!s) return;
    s->checked_unix = now; s->retry_at_unix = retry_at; s->failures = failures;
    s->next_unix = update_next_check(now, failures, retry_at, random, interval);
}
void update_schedule_retarget(UpdateSchedule* s, u64 interval, u32 random) noexcept {
    // Until the first check has run, its startup deadline stands.
    if (s && s->checked_unix) s->next_unix = update_next_check(s->checked_unix, s->failures, s->retry_at_unix, random, interval);
}

} // namespace maicord

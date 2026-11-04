#include "maicord/app/plugins/http.hpp"
#include "maicord/app/update/log.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord {

// ---- Hosts ------------------------------------------------------------------------

namespace {

// Discord's own domains: the client's token and sessions belong to these, so no plugin request
// reaches them, whatever the plugin declares or a redirect names.
constexpr char kDeniedDomains[] =
    "discord.com\0discordapp.com\0discordapp.net\0discord.gg\0discord.media\0discordcdn.com\0discord.co\0"
    "discord.gift\0discord.gifts\0discord.new\0discord.dev\0discord.design\0discordstatus.com\0discordsays.com\0"
    "discordmerch.com\0";

Utf8View pattern_base(const Utf8View& pattern) noexcept {
    return pattern.bytes > 2 && pattern.data[0] == '*' && pattern.data[1] == '.'
               ? Utf8View{pattern.data + 2, pattern.bytes - 2}
               : pattern;
}

} // namespace

bool plugin_network_host_denied(Utf8View host) noexcept {
    for (NameList list{kDeniedDomains}; list; list.next()) {
        const Utf8View domain = list.name();
        if (host.bytes < domain.bytes) continue;
        const u32 prefix = host.bytes - domain.bytes;
        if (bytes_equal(host.data + prefix, domain.data, domain.bytes) && (!prefix || host.data[prefix - 1] == '.'))
            return true;
    }
    return false;
}

bool plugin_network_pattern_valid(const Utf8View& pattern) noexcept {
    const Utf8View base = pattern_base(pattern);
    if (!update_origin_host_valid(base)) return false;
    for (u32 i = 0; i < base.bytes; ++i)
        if (base.data[i] >= 'A' && base.data[i] <= 'Z') return false;
    return !plugin_network_host_denied(base);
}

bool plugin_network_pattern_matches(const Utf8View& pattern, const Utf8View& host) noexcept {
    const Utf8View base = pattern_base(pattern);
    if (base.data == pattern.data) return utf8_equal(pattern, host);
    // "*.example.com": a subdomain with at least one label in front of ".example.com".
    return host.bytes > base.bytes + 1 && host.data[host.bytes - base.bytes - 1] == '.' &&
           bytes_equal(host.data + (host.bytes - base.bytes), base.data, base.bytes);
}

bool plugin_network_hosts_valid(const mc_str* hosts, u32 count, bool has_completion_hook) noexcept {
    if (!count) return !hosts;
    if (!hosts || count > MC_NETWORK_HOSTS_MAX || !has_completion_hook) return false;
    for (u32 i = 0; i < count; ++i) {
        if (!hosts[i].data || !plugin_network_pattern_valid({hosts[i].data, hosts[i].bytes})) return false;
        for (u32 j = 0; j < i; ++j)
            if (utf8_equal({hosts[i].data, hosts[i].bytes}, {hosts[j].data, hosts[j].bytes})) return false;
    }
    return true;
}

// ---- Broker state -----------------------------------------------------------------

namespace {

constexpr u32 kSlots = 8;
constexpr u32 kPatterns = MC_NETWORK_HOSTS_MAX; // the loader allows no more declared hosts
constexpr u32 kHostBytes = kUpdateHostNameCapacity;
constexpr u32 kHeaderTextBytes = 4096;
constexpr u64 kWindowMilliseconds = 60'000;
constexpr DWORD kStopWaitMilliseconds = 3000;
constexpr Utf8View kUserAgent = utf8_literal("MaiCord-Plugin");

enum class State : u8 { free, queued, running, done };

struct HeaderRef {
    u16 name_at, name_bytes, value_at, value_bytes;
};

struct Slot {
    State state;
    // Disabled while running: nobody receives the result.
    bool discard;
    u8 method, pattern_count, header_count;
    u8 pattern_bytes[kPatterns];
    u32 generation;
    u64 owner_id, tag, gesture_until_ms;
    u32 max_response, body_bytes;
    // The result.
    u32 error, status;
    u8* response;
    u32 response_reserved, response_committed, response_used;
    bool too_large;
    UpdateLocation location;
    HeaderRef headers[MC_HTTP_HEADERS_MAX];
    char header_text[kHeaderTextBytes];
    // The hosts the plugin could reach when it asked: its approved patterns.
    char patterns[kPatterns][kHostBytes];
    u8 body[MC_HTTP_BODY_MAX];
};

}  // namespace

struct PluginHttp {
    SrwLock lock;
    Thread* thread;
    Waitable notify;
    PluginHttpTransport transport;
    // The HTTPS transport a request is using, so that close can cancel it.
    UpdateHttpsTransport* volatile active;
    volatile Atomic32 stopping;
    // Guarded by lock: a worker thread is alive and will look for queued slots before it exits.
    bool worker_running;
    Slot slots[kSlots];
};

namespace {

u32 align_page(u32 bytes) noexcept {
    return (bytes + 4095u) & ~4095u;
}

void release_response(Slot& slot) noexcept {
    if (slot.response) pages_release(slot.response);
    slot.response = nullptr;
    slot.response_reserved = slot.response_committed = slot.response_used = 0;
}

PluginHttp* http_of(PluginRuntime* runtime, bool create) noexcept {
    if (runtime->http || !create) return runtime->http;
    auto* http = commit_pages<PluginHttp>();
    if (!http) return nullptr;
    http->notify = event_create(false, false);
    if (!http->notify) {
        pages_release(http);
        return nullptr;
    }
    runtime->http = http;
    return http;
}

// ---- Validation (UI thread) -------------------------------------------------------

// An absolute https URL -> the request location. No userinfo, port or fragment: the host must be a DNS
// name and the target made of path and query characters.
bool https_location(Utf8View url, UpdateLocation* out) noexcept {
    constexpr Utf8View scheme = utf8_literal("https://");
    if (url.bytes <= scheme.bytes || !ascii_equal_ignore_case({url.data, scheme.bytes}, scheme)) return false;
    u32 end = scheme.bytes;
    while (end < url.bytes && url.data[end] != '/' && url.data[end] != '?') ++end;
    char target[kUpdateTargetCapacity];
    u32 count = 0;
    if (end == url.bytes || url.data[end] == '?') target[count++] = '/';
    if (url.bytes - end > sizeof(target) - count) return false;
    copy_bytes(target + count, url.data + end, url.bytes - end);
    count += url.bytes - end;
    return update_provider_location({url.data + scheme.bytes, end - scheme.bytes}, {target, count}, out);
}

bool header_name_reserved(Utf8View name) noexcept {
    static constexpr char reserved[] =
        "host\0content-length\0transfer-encoding\0connection\0accept-encoding\0cookie\0cookie2\0set-cookie\0"
        "user-agent\0upgrade\0expect\0te\0trailer\0keep-alive\0proxy-authorization\0proxy-connection\0";
    for (NameList list{reserved}; list; list.next())
        if (ascii_equal_ignore_case(name, list.name())) return true;
    return false;
}

// Copies the request's headers into the slot. MC_E_INVALID for a malformed header, MC_E_CAPACITY past the text budget.
mc_status copy_headers(Slot& slot, const mc_http_request& request) noexcept {
    if (request.header_count > MC_HTTP_HEADERS_MAX || (request.header_count && !request.headers)) return MC_E_INVALID;
    u32 used = 0;
    for (u32 i = 0; i < request.header_count; ++i) {
        const mc_http_header& header = request.headers[i];
        const Utf8View name{header.name.data, header.name.bytes}, value{header.value.data, header.value.bytes};
        if (!name.bytes || name.bytes > 64 || !value.bytes || value.bytes > MC_HTTP_HEADER_VALUE_MAX || !name.data ||
            !value.data)
            return MC_E_INVALID;
        for (u32 j = 0; j < name.bytes; ++j)
            if (!ascii_is_alnum(name.data[j]) && name.data[j] != '-') return MC_E_INVALID;
        for (u32 j = 0; j < value.bytes; ++j)
            if (value.data[j] < ' ' || value.data[j] > '~') return MC_E_INVALID;
        if (header_name_reserved(name)) return MC_E_INVALID;
        for (u32 j = 0; j < i; ++j)
            if (ascii_equal_ignore_case(name, {slot.header_text + slot.headers[j].name_at, slot.headers[j].name_bytes}))
                return MC_E_INVALID;
        if (name.bytes + value.bytes > kHeaderTextBytes - used) return MC_E_CAPACITY;
        copy_bytes(slot.header_text + used, name.data, name.bytes);
        copy_bytes(slot.header_text + used + name.bytes, value.data, value.bytes);
        slot.headers[i] = {static_cast<u16>(used), static_cast<u16>(name.bytes), static_cast<u16>(used + name.bytes),
                           static_cast<u16>(value.bytes)};
        used += name.bytes + value.bytes;
    }
    slot.header_count = static_cast<u8>(request.header_count);
    return MC_OK;
}

bool slot_allows(const Slot& slot, Utf8View host) noexcept {
    if (plugin_network_host_denied(host)) return false;
    for (u32 i = 0; i < slot.pattern_count; ++i)
        if (plugin_network_pattern_matches({slot.patterns[i], slot.pattern_bytes[i]}, host)) return true;
    return false;
}

// ---- The worker -------------------------------------------------------------------

bool sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto& slot = *static_cast<Slot*>(context);
    if (bytes.count > slot.max_response - slot.response_used) {
        slot.too_large = true;
        return false;
    }
    const u32 need = slot.response_used + bytes.count;
    if (need > slot.response_committed) {
        // Commit in steps so a small response stays small.
        u32 target = slot.response_committed + 64 * 1024;
        if (target < need) target = need;
        target = align_page(target);
        if (target > slot.response_reserved) target = slot.response_reserved;
        if (!pages_commit_at(slot.response + slot.response_committed, target - slot.response_committed))
            return false;
        slot.response_committed = target;
    }
    copy_bytes(slot.response + slot.response_used, bytes.data, bytes.count);
    slot.response_used = need;
    return true;
}

// Where a redirect leads, when the plugin may follow it: https on a host its patterns allow, never a
// Discord host. Relative references stay on the current host.
bool redirect_target(const Slot& slot, const UpdateLocation& current, ConstSlice<u8> location,
                     UpdateLocation* next) noexcept {
    if (!location.count) return false;
    const char* text = reinterpret_cast<const char*>(location.data);
    if (text[0] == '/') {
        if (location.count > 1 && text[1] == '/') return false;
        return update_provider_location({current.origin, current.origin_bytes}, {text, location.count}, next);
    }
    UpdateLocation candidate{};
    if (!https_location({text, location.count}, &candidate)) return false;
    if (!slot_allows(slot, {candidate.origin, candidate.origin_bytes})) return false;
    *next = candidate;
    return true;
}

void finish(Slot& slot, u32 error, u32 status) noexcept {
    slot.error = error;
    slot.status = status;
    if (error) slot.response_used = 0;
}

void run(Slot& slot, const PluginHttpTransport& transport) noexcept {
    // Reserve the whole allowed response up front; pages are committed as bytes arrive.
    slot.response_reserved = align_page(slot.max_response ? slot.max_response : 4096);
    slot.response = static_cast<u8*>(pages_reserve(slot.response_reserved));
    if (!slot.response) return finish(slot, MC_HTTP_E_NETWORK, 0);

    UpdateLocation at = slot.location;
    bool custom_headers = true;
    for (u32 hop = 0;; ++hop) {
        HttpsHeader headers[MC_HTTP_HEADERS_MAX + 1];
        u32 header_count = 0;
        headers[header_count++] = {utf8_literal("User-Agent"), kUserAgent};
        for (u32 i = 0; custom_headers && i < slot.header_count; ++i)
            headers[header_count++] = {{slot.header_text + slot.headers[i].name_at, slot.headers[i].name_bytes},
                                       {slot.header_text + slot.headers[i].value_at, slot.headers[i].value_bytes}};
        HttpsExchangeOptions options{};
        options.headers = {headers, header_count};
        options.body = slot.method == MC_HTTP_POST ? ConstSlice<u8>{slot.body, slot.body_bytes} : ConstSlice<u8>{};
        options.read_error_body = true;
        slot.response_used = 0;
        const Utf8View host{at.origin, at.origin_bytes};
        const UpdateHttpRequest request{at.host, host, {at.target, at.target_bytes}, slot.max_response, sink, &slot};
        UpdateHttpResponse response{};
        const UpdateTransportError sent = transport.exchange(transport.context, request, options, &response);
        UpdateLog line("plugins", "plugin_http");
        line.hex("owner", slot.owner_id).host("host", host).field("status", response.status)
            .field("bytes", response.body_bytes);
        if (sent != UpdateTransportError::none) {
            line.field("transport", static_cast<u8>(sent)).field("native", response.native_error)
                .write(LogLevel::warning);
            const bool large = sent == UpdateTransportError::oversize || slot.too_large;
            return finish(slot, large ? MC_HTTP_E_TOO_LARGE : MC_HTTP_E_NETWORK, 0);
        }
        const u16 status = response.status;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            // POST bodies are never resent, and only approved hosts are followed.
            const ConstSlice<u8> location{reinterpret_cast<const u8*>(response.location), response.location_bytes};
            UpdateLocation next{};
            const bool allowed = slot.method == MC_HTTP_GET && hop < MC_HTTP_REDIRECTS_MAX &&
                                 redirect_target(slot, at, location, &next);
            line.field("redirect", allowed ? 1 : 0).write(allowed ? LogLevel::info : LogLevel::warning);
            if (!allowed) return finish(slot, MC_HTTP_E_REDIRECT, 0);
            // The plugin's own headers (API keys) go to the host it asked for, not to another one.
            if (next.origin_bytes != at.origin_bytes || !bytes_equal(next.origin, at.origin, at.origin_bytes))
                custom_headers = false;
            at = next;
            continue;
        }
        line.write(status >= 200 && status < 400 ? LogLevel::info : LogLevel::warning);
        return finish(slot, MC_HTTP_OK, status);
    }
}

UpdateTransportError exchange_https(void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                                    UpdateHttpResponse* response) noexcept {
    return https_exchange(static_cast<UpdateHttpsTransport*>(context), request, options, response);
}

// The next queued request, marked running; null when none is queued (or the broker is stopping), in which
// case `leave` also records that this worker is going away.
Slot* take(PluginHttp* http, bool leave) noexcept {
    Slot* slot = nullptr;
    AcquireSRWLockExclusive(http->lock.native());
    if (!read_acquire(&http->stopping)) {
        for (u32 i = 0; i < kSlots && !slot; ++i)
            if (http->slots[i].state == State::queued) slot = &http->slots[i];
    }
    if (slot) slot->state = State::running;
    else if (leave) http->worker_running = false;
    ReleaseSRWLockExclusive(http->lock.native());
    return slot;
}

u32 worker_main(void* parameter) noexcept {
    auto* http = static_cast<PluginHttp*>(parameter);
    const PluginHttpTransport injected = http->transport;
    UpdateHttpsTransport* https = nullptr;
    const auto release_https = [&]() noexcept {
        if (!https) return;
        // close cancels under the same lock, so it never reaches a transport being released.
        AcquireSRWLockExclusive(http->lock.native());
        http->active = nullptr;
        ReleaseSRWLockExclusive(http->lock.native());
        update_https_release(https);
        pages_release(https);
        https = nullptr;
    };
    for (;;) {
        Slot* slot = take(http, false);
        if (!slot) {
            // Idle: give the network stack back, then look once more before leaving.
            release_https();
            slot = take(http, true);
            if (!slot) break;
        }
        PluginHttpTransport transport = injected;
        if (!transport.exchange) {
            if (!https) {
                https = commit_pages<UpdateHttpsTransport>();
                if (https && !update_https_initialize(https)) {
                    pages_release(https);
                    https = nullptr;
                }
                if (https) {
                    AcquireSRWLockExclusive(http->lock.native());
                    http->active = https;
                    ReleaseSRWLockExclusive(http->lock.native());
                }
            }
            if (https) transport = {https, exchange_https};
        }
        if (transport.exchange) run(*slot, transport);
        else finish(*slot, MC_HTTP_E_NETWORK, 0);
        AcquireSRWLockExclusive(http->lock.native());
        slot->state = State::done;
        ReleaseSRWLockExclusive(http->lock.native());
        event_set(http->notify);
    }
    return 0;
}

// Starts a worker when none is looking for queued requests.
bool ensure_worker(PluginHttp* http) noexcept {
    AcquireSRWLockExclusive(http->lock.native());
    const bool alive = http->worker_running;
    if (!alive) http->worker_running = true;
    ReleaseSRWLockExclusive(http->lock.native());
    if (alive) return true;
    // The previous worker has left its loop (worker_running was cleared under the lock); its handle only needs closing.
    if (http->thread) thread_close(http->thread);
    http->thread = thread_start(worker_main, http, 512 * 1024, nullptr);
    if (!http->thread) {
        AcquireSRWLockExclusive(http->lock.native());
        http->worker_running = false;
        ReleaseSRWLockExclusive(http->lock.native());
    }
    return http->thread != nullptr;
}

}  // namespace

// ---- Public -----------------------------------------------------------------------

bool plugin_http_use_transport(PluginRuntime* runtime, const PluginHttpTransport& transport) noexcept {
    PluginHttp* http = runtime && transport.exchange ? http_of(runtime, true) : nullptr;
    if (!http) return false;
    http->transport = transport;
    return true;
}

mc_status plugin_http_submit(PluginBinding* binding, const mc_http_request& request, u64 tag) noexcept {
    if (!(binding->permissions & MC_PERM_NETWORK)) return MC_E_DENIED;
    if ((request.method != MC_HTTP_GET && request.method != MC_HTTP_POST) || request.flags || request.reserved ||
        !request.url.data || (request.body_bytes && !request.body) ||
        (request.method == MC_HTTP_GET ? request.body_bytes != 0 : !request.body_bytes) ||
        request.max_response_bytes > MC_HTTP_RESPONSE_MAX)
        return MC_E_INVALID;
    if (request.body_bytes > MC_HTTP_BODY_MAX) return MC_E_CAPACITY;
    UpdateLocation location{};
    if (request.url.bytes > MC_HTTP_URL_MAX || !https_location({request.url.data, request.url.bytes}, &location))
        return MC_E_INVALID;
    const Utf8View host{location.origin, location.origin_bytes};

    // The hosts the user approved, and whether this request's host is one of them.
    const mc_descriptor& descriptor = *binding->image.descriptor;
    bool allowed = false;
    for (u32 i = 0; i < descriptor.network_count; ++i) {
        if (!(binding->approved_hosts & (1u << i))) continue;
        allowed = allowed || plugin_network_pattern_matches({descriptor.network[i].data, descriptor.network[i].bytes}, host);
    }
    if (!allowed || plugin_network_host_denied(host)) return MC_E_DENIED;

    PluginRuntime* runtime = binding->runtime;
    const u64 now = clock_monotonic_milliseconds();
    if (!binding->http_window_count || now - binding->http_window_ms >= kWindowMilliseconds) {
        binding->http_window_ms = now;
        binding->http_window_count = 0;
    }
    if (binding->http_window_count >= MC_HTTP_REQUESTS_PER_MINUTE) return MC_E_RATE_LIMITED;

    PluginHttp* http = http_of(runtime, true);
    if (!http) return MC_E_BUSY;
    // Only the UI thread takes a free slot, so it stays ours until it is queued below.
    Slot* slot = nullptr;
    u32 pending = 0;
    AcquireSRWLockShared(http->lock.native());
    for (u32 i = 0; i < kSlots; ++i) {
        Slot& candidate = http->slots[i];
        if (candidate.state == State::free) {
            if (!slot) slot = &candidate;
        } else if (candidate.owner_id == binding->owner_id && candidate.generation == binding->generation) {
            ++pending;
        }
    }
    ReleaseSRWLockShared(http->lock.native());
    if (!slot || pending >= MC_HTTP_PENDING_MAX) return MC_E_BUSY;

    reset_value(slot);
    const mc_status status = copy_headers(*slot, request);
    if (status == MC_OK) {
        slot->method = static_cast<u8>(request.method);
        slot->owner_id = binding->owner_id;
        slot->generation = binding->generation;
        slot->tag = tag;
        slot->max_response = request.max_response_bytes ? request.max_response_bytes : MC_HTTP_RESPONSE_MAX;
        slot->body_bytes = request.body_bytes;
        copy_bytes(slot->body, request.body, request.body_bytes);
        copy_value(&slot->location, location);
        slot->gesture_until_ms = (binding->ctx.flags & MC_CTX_GESTURE) ? binding->gesture_until_ms : 0;
        for (u32 i = 0; i < descriptor.network_count; ++i) {
            if (!(binding->approved_hosts & (1u << i))) continue;
            const mc_str& pattern = descriptor.network[i];
            copy_bytes(slot->patterns[slot->pattern_count], pattern.data, pattern.bytes);
            slot->pattern_bytes[slot->pattern_count++] = static_cast<u8>(pattern.bytes);
        }
    }
    AcquireSRWLockExclusive(http->lock.native());
    slot->state = status == MC_OK ? State::queued : State::free;
    ReleaseSRWLockExclusive(http->lock.native());
    if (status != MC_OK) return status;
    if (!ensure_worker(http)) {
        AcquireSRWLockExclusive(http->lock.native());
        slot->state = State::free;
        ReleaseSRWLockExclusive(http->lock.native());
        return MC_E_BUSY;
    }
    ++binding->http_window_count;
    return MC_OK;
}

HANDLE plugin_http_notification(PluginRuntime* runtime) noexcept {
    return runtime && runtime->http ? runtime->http->notify : nullptr;
}

u32 plugin_http_deliver(PluginRuntime* runtime) noexcept {
    PluginHttp* http = runtime ? runtime->http : nullptr;
    if (!http) return 0;
    u32 delivered = 0;
    for (u32 i = 0; i < kSlots; ++i) {
        Slot& slot = http->slots[i];
        AcquireSRWLockShared(http->lock.native());
        const bool done = slot.state == State::done;
        ReleaseSRWLockShared(http->lock.native());
        if (!done) continue;
        // Leave the slot before the plugin runs: its handler may start the next request.
        AcquireSRWLockExclusive(http->lock.native());
        const u64 owner = slot.owner_id, tag = slot.tag, gesture_until = slot.gesture_until_ms;
        const u32 generation = slot.generation, error = slot.error, status = slot.status;
        const bool discard = slot.discard;
        u8* const response = slot.response;
        const u32 response_bytes = slot.response_used;
        slot.response = nullptr;
        slot.state = State::free;
        ReleaseSRWLockExclusive(http->lock.native());
        PluginBinding* binding = nullptr;
        for (u32 b = 0; b < runtime->binding_count; ++b)
            if (runtime->bindings[b]->owner_id == owner && runtime->bindings[b]->generation == generation)
                binding = runtime->bindings[b];
        if (binding && !discard) {
            PluginCompletion completion{};
            completion.kind = PluginCompletionKind::http;
            completion.tag = tag;
            completion.http_error = error;
            completion.http_status = status;
            completion.body = {response, response_bytes};
            completion.inherited_gesture_until_ms = gesture_until;
            plugin_binding_complete(binding, completion);
            ++delivered;
        }
        if (response) pages_release(response);
    }
    return delivered;
}

void plugin_http_cancel(PluginRuntime* runtime, const PluginBinding* binding, bool drop) noexcept {
    PluginHttp* http = runtime ? runtime->http : nullptr;
    if (!http) return;
    bool woke = false;
    AcquireSRWLockExclusive(http->lock.native());
    for (u32 i = 0; i < kSlots; ++i) {
        Slot& slot = http->slots[i];
        if (slot.state == State::free || slot.owner_id != binding->owner_id || slot.generation != binding->generation)
            continue;
        if (drop) {
            if (slot.state == State::running) slot.discard = true;
            else {
                release_response(slot);
                slot.state = State::free;
            }
        } else if (slot.state == State::queued) {
            slot.error = MC_HTTP_E_DENIED;
            slot.state = State::done;
            woke = true;
        }
    }
    ReleaseSRWLockExclusive(http->lock.native());
    if (woke) event_set(http->notify);
}

void plugin_http_close(PluginRuntime* runtime) noexcept {
    PluginHttp* http = runtime ? runtime->http : nullptr;
    if (!http) return;
    InterlockedExchange(&http->stopping, 1);
    AcquireSRWLockExclusive(http->lock.native());
    if (UpdateHttpsTransport* https = http->active) update_https_cancel(https);
    ReleaseSRWLockExclusive(http->lock.native());
    if (http->thread && !thread_wait(http->thread, kStopWaitMilliseconds)) return;
    // A worker that does not stop keeps its memory; nothing else touches it.
    runtime->http = nullptr;
    if (http->thread) thread_close(http->thread);
    waitable_close(http->notify);
    for (Slot& slot : http->slots) release_response(slot);
    pages_release(http);
}

} // namespace maicord
